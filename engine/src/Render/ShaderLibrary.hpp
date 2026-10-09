// Shader programs built from GLSL files on disk (engine/shaders), with
// #include support, per-program #define permutations and hot reload: every
// half second the library checks the modification time of each program's
// source files (including headers) and recompiles the ones that changed.
// A program that fails to recompile keeps its previous binary and the error
// goes to the console.
//
// Files are found in, in order: the source tree's engine/shaders folder
// (HOLLOW_SHADER_SOURCE_DIR, set by CMake, so edits hot reload while
// developing) and <exe dir>/shaders (copied there by the build).
#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hollow {

using ProgramId = std::uint32_t;
constexpr ProgramId kNoProgram = 0xFFFFFFFFu;

class ShaderLibrary {
public:
    ShaderLibrary();
    ~ShaderLibrary();
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    // Returns false if no shader directory exists.
    bool initialize();
    void shutdown();
    const std::filesystem::path& directory() const { return directory_; }

    // `defines` is a list like {"ALPHA_MASK", "PASS=2"}. Programs are
    // cached by (files, defines). Throws std::runtime_error if the first
    // compile fails.
    ProgramId graphics(const std::string& vertex, const std::string& fragment, const std::vector<std::string>& defines = {});
    ProgramId compute(const std::string& file, const std::vector<std::string>& defines = {});

    GLuint program(ProgramId id) const;
    // glUseProgram, returns the GL name.
    GLuint use(ProgramId id) const;
    // Cached uniform location (uniforms not in blocks; -1 if absent).
    GLint uniform(ProgramId id, const char* name);

    // Recompiles programs whose files changed (rate-limited internally).
    // Returns the number of programs rebuilt.
    int hot_reload(bool force = false);
    std::uint64_t generation() const { return generation_; }
    std::size_t program_count() const { return programs_.size(); }

private:
    struct Program {
        std::vector<std::string> stages; // file names, by stage
        std::vector<GLenum> stage_types;
        std::vector<std::string> defines;
        GLuint gl = 0;
        std::vector<std::filesystem::path> dependencies;
        std::filesystem::file_time_type newest{};
        std::unordered_map<std::string, GLint> uniforms;
    };

    ProgramId create(std::vector<std::string> stages, std::vector<GLenum> types, const std::vector<std::string>& defines);
    // Builds (or throws). Fills dependencies.
    GLuint build(Program& program);
    std::string preprocess(const std::string& file, const std::vector<std::string>& defines, std::vector<std::filesystem::path>& dependencies, GLenum stage);
    std::string load_with_includes(const std::filesystem::path& path, std::vector<std::filesystem::path>& dependencies, int file_index, int depth,
        std::vector<std::filesystem::path>& file_table);

    std::filesystem::path directory_;
    std::vector<Program> programs_;
    std::unordered_map<std::string, ProgramId> lookup_;
    double last_check_ = 0.0;
    std::uint64_t generation_ = 0;
};

} // namespace Hollow
