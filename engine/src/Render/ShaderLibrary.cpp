// Implementation of ShaderLibrary.hpp.
#include "Render/ShaderLibrary.hpp"

#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Hollow {
namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string stage_name(GLenum stage) {
    switch (stage) {
    case GL_VERTEX_SHADER: return "vertex";
    case GL_FRAGMENT_SHADER: return "fragment";
    case GL_COMPUTE_SHADER: return "compute";
    case GL_GEOMETRY_SHADER: return "geometry";
    default: return "shader";
    }
}

std::string make_key(const std::vector<std::string>& stages, const std::vector<std::string>& defines) {
    std::string key;
    for (const auto& s : stages) {
        key += s;
        key += '|';
    }
    key += '#';
    for (const auto& d : defines) {
        key += d;
        key += ';';
    }
    return key;
}

std::filesystem::file_time_type newest_time(const std::vector<std::filesystem::path>& files) {
    std::filesystem::file_time_type newest{};
    for (const auto& file : files) {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(file, ec);
        if (!ec && time > newest) {
            newest = time;
        }
    }
    return newest;
}

// Rewrites "0(123)" style locations in a driver log into "file.glsl:123".
std::string annotate_log(const std::string& log, const std::vector<std::filesystem::path>& files) {
    std::string out = log;
    for (std::size_t i = files.size(); i-- > 0;) {
        const std::string name = to_utf8(files[i].filename());
        // NVIDIA: "0(12) : error", Mesa: "0:12(5): error", AMD: "ERROR: 0:12:"
        out = std::regex_replace(out, std::regex("\\b" + std::to_string(i) + "\\(([0-9]+)\\)"), name + ":$1");
        out = std::regex_replace(out, std::regex("\\b" + std::to_string(i) + ":([0-9]+)"), name + ":$1");
    }
    return out;
}

} // namespace

ShaderLibrary::ShaderLibrary() = default;

ShaderLibrary::~ShaderLibrary() {
    shutdown();
}

bool ShaderLibrary::initialize() {
    std::vector<std::filesystem::path> candidates;
#ifdef HOLLOW_SHADER_SOURCE_DIR
    candidates.emplace_back(path_from_utf8(HOLLOW_SHADER_SOURCE_DIR));
#endif
    candidates.push_back(executable_directory() / "shaders");
    candidates.push_back(std::filesystem::current_path() / "shaders");
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate / "common.glsl", ec)) {
            directory_ = candidate;
            log_info("Shaders: " + to_utf8(candidate));
            return true;
        }
    }
    return false;
}

void ShaderLibrary::shutdown() {
    for (Program& program : programs_) {
        if (program.gl != 0 && glDeleteProgram != nullptr) {
            glDeleteProgram(program.gl);
        }
        program.gl = 0;
    }
    programs_.clear();
    lookup_.clear();
}

std::string ShaderLibrary::load_with_includes(const std::filesystem::path& path, std::vector<std::filesystem::path>& dependencies, int file_index, int depth,
    std::vector<std::filesystem::path>& file_table) {
    if (depth > 16) {
        throw std::runtime_error("shader #include nesting too deep at " + to_utf8(path));
    }
    std::string source;
    try {
        source = read_text_file(path);
    } catch (const std::exception&) {
        throw std::runtime_error("can't read shader file " + to_utf8(path));
    }
    dependencies.push_back(path);

    std::ostringstream out;
    std::istringstream in(source);
    std::string line;
    int line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 8, "#include") == 0) {
            const std::size_t open = line.find('"', first);
            const std::size_t close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
            if (close == std::string::npos) {
                throw std::runtime_error(std::format("{}:{}: malformed #include", to_utf8(path.filename()), line_number));
            }
            const std::filesystem::path include = directory_ / line.substr(open + 1, close - open - 1);
            if (std::find(dependencies.begin(), dependencies.end(), include) != dependencies.end()) {
                out << "\n"; // already included (every header is include-once)
                continue;
            }
            const int include_index = static_cast<int>(file_table.size());
            file_table.push_back(include);
            out << "#line 1 " << include_index << "\n";
            out << load_with_includes(include, dependencies, include_index, depth + 1, file_table);
            out << "#line " << (line_number + 1) << " " << file_index << "\n";
            continue;
        }
        if (first != std::string::npos && line.compare(first, 12, "#pragma once") == 0) {
            out << "\n";
            continue;
        }
        out << line << "\n";
    }
    return out.str();
}

std::string ShaderLibrary::preprocess(const std::string& file, const std::vector<std::string>& defines, std::vector<std::filesystem::path>& dependencies, GLenum stage) {
    std::ostringstream out;
    out << "#version 450 core\n";
    out << "#extension GL_ARB_shader_draw_parameters : enable\n";
    switch (stage) {
    case GL_VERTEX_SHADER: out << "#define STAGE_VERTEX 1\n"; break;
    case GL_FRAGMENT_SHADER: out << "#define STAGE_FRAGMENT 1\n"; break;
    case GL_COMPUTE_SHADER: out << "#define STAGE_COMPUTE 1\n"; break;
    default: break;
    }
    for (const std::string& define : defines) {
        const std::size_t eq = define.find('=');
        if (eq == std::string::npos) {
            out << "#define " << define << " 1\n";
        } else {
            out << "#define " << define.substr(0, eq) << " " << define.substr(eq + 1) << "\n";
        }
    }
    std::vector<std::filesystem::path> file_table{directory_ / file};
    out << "#line 1 0\n";
    out << load_with_includes(directory_ / file, dependencies, 0, 0, file_table);
    return out.str();
}

GLuint ShaderLibrary::build(Program& program) {
    std::vector<std::filesystem::path> dependencies;
    std::vector<GLuint> shaders;
    auto cleanup = [&shaders] {
        for (GLuint shader : shaders) {
            glDeleteShader(shader);
        }
    };
    for (std::size_t i = 0; i < program.stages.size(); ++i) {
        const GLenum type = program.stage_types[i];
        std::vector<std::filesystem::path> stage_deps;
        std::string source;
        try {
            source = preprocess(program.stages[i], program.defines, stage_deps, type);
        } catch (...) {
            cleanup();
            throw;
        }
        const GLuint shader = glCreateShader(type);
        const char* text = source.c_str();
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint length = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetShaderInfoLog(shader, length, nullptr, log.data());
            glDeleteShader(shader);
            cleanup();
            std::string defines;
            for (const auto& d : program.defines) {
                defines += " " + d;
            }
            // The file table for line annotations: main file + includes in
            // inclusion order (matches the indices handed out above).
            std::vector<std::filesystem::path> table{directory_ / program.stages[i]};
            for (std::size_t d = 1; d < stage_deps.size(); ++d) {
                table.push_back(stage_deps[d]);
            }
            throw std::runtime_error(std::format("{} shader {} [{}] failed to compile:\n{}", stage_name(type), program.stages[i], defines, annotate_log(log, table)));
        }
        shaders.push_back(shader);
        dependencies.insert(dependencies.end(), stage_deps.begin(), stage_deps.end());
    }

    const GLuint gl = glCreateProgram();
    for (GLuint shader : shaders) {
        glAttachShader(gl, shader);
    }
    glLinkProgram(gl);
    for (GLuint shader : shaders) {
        glDetachShader(gl, shader);
    }
    cleanup();
    GLint linked = 0;
    glGetProgramiv(gl, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint length = 0;
        glGetProgramiv(gl, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(gl, length, nullptr, log.data());
        glDeleteProgram(gl);
        throw std::runtime_error(std::format("shader program ({}) failed to link:\n{}", program.stages.front(), log));
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    program.dependencies = std::move(dependencies);
    program.newest = newest_time(program.dependencies);
    program.uniforms.clear();
    return gl;
}

ProgramId ShaderLibrary::create(std::vector<std::string> stages, std::vector<GLenum> types, const std::vector<std::string>& defines) {
    const std::string key = make_key(stages, defines);
    if (const auto it = lookup_.find(key); it != lookup_.end()) {
        return it->second;
    }
    Program program;
    program.stages = std::move(stages);
    program.stage_types = std::move(types);
    program.defines = defines;
    program.gl = build(program); // throws on the first failure
    const ProgramId id = static_cast<ProgramId>(programs_.size());
    programs_.push_back(std::move(program));
    lookup_[key] = id;
    return id;
}

ProgramId ShaderLibrary::graphics(const std::string& vertex, const std::string& fragment, const std::vector<std::string>& defines) {
    return create({vertex, fragment}, {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER}, defines);
}

ProgramId ShaderLibrary::compute(const std::string& file, const std::vector<std::string>& defines) {
    return create({file}, {GL_COMPUTE_SHADER}, defines);
}

GLuint ShaderLibrary::program(ProgramId id) const {
    return id < programs_.size() ? programs_[id].gl : 0;
}

GLuint ShaderLibrary::use(ProgramId id) const {
    const GLuint gl = program(id);
    glUseProgram(gl);
    return gl;
}

GLint ShaderLibrary::uniform(ProgramId id, const char* name) {
    if (id >= programs_.size()) {
        return -1;
    }
    Program& program = programs_[id];
    const auto it = program.uniforms.find(name);
    if (it != program.uniforms.end()) {
        return it->second;
    }
    const GLint location = glGetUniformLocation(program.gl, name);
    program.uniforms.emplace(name, location);
    return location;
}

int ShaderLibrary::hot_reload(bool force) {
    const double now = now_seconds();
    if (!force && now - last_check_ < 0.5) {
        return 0;
    }
    last_check_ = now;
    int rebuilt = 0;
    for (Program& program : programs_) {
        const auto newest = newest_time(program.dependencies);
        if (!force && newest <= program.newest) {
            continue;
        }
        program.newest = newest; // don't retry a broken file every check
        try {
            const GLuint gl = build(program);
            glDeleteProgram(program.gl);
            program.gl = gl;
            ++rebuilt;
        } catch (const std::exception& error) {
            log_error(error.what());
        }
    }
    if (rebuilt > 0) {
        ++generation_;
        log_info(std::format("Reloaded {} shader program(s)", rebuilt));
    }
    return rebuilt;
}

} // namespace Hollow
