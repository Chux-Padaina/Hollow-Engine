#include "Forge/Importer.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace Forge {
namespace {

struct ObjIndex {
    int position = 0;
    int uv = 0;
    int normal = 0;
};

struct ObjPartBuilder {
    std::string name;
    Material material;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

float parse_float(const std::string& text) {
    return std::stof(text);
}

int parse_int_or_zero(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    return std::stoi(text);
}

int resolve_index(int index, int size) {
    if (index > 0) {
        return index - 1;
    }
    if (index < 0) {
        return size + index;
    }
    return -1;
}

ObjIndex parse_obj_index(const std::string& token) {
    ObjIndex out;
    std::stringstream stream(token);
    std::string item;
    std::getline(stream, item, '/');
    out.position = parse_int_or_zero(item);
    if (std::getline(stream, item, '/')) {
        out.uv = parse_int_or_zero(item);
    }
    if (std::getline(stream, item, '/')) {
        out.normal = parse_int_or_zero(item);
    }
    return out;
}

std::string resolve_texture_path(const std::filesystem::path& material_path, const std::string& texture_name) {
    if (texture_name.empty()) {
        return {};
    }
    std::filesystem::path texture_path(texture_name);
    if (texture_path.is_relative()) {
        texture_path = material_path.parent_path() / texture_path;
    }
    return texture_path.lexically_normal().string();
}

std::string read_mtl_texture_name(std::stringstream& stream) {
    std::string token;
    std::string texture_name;
    while (stream >> token) {
        if (!token.empty() && token[0] != '-') {
            texture_name = token;
        }
    }
    return texture_name;
}

std::map<std::string, Material> load_mtl_file(const std::filesystem::path& path) {
    std::map<std::string, Material> materials;
    std::ifstream file(path);
    if (!file) {
        return materials;
    }

    std::string current_name;
    std::string line;
    while (std::getline(file, line)) {
        std::stringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag == "newmtl") {
            stream >> current_name;
            materials[current_name] = Material{};
        } else if (tag == "Kd" && !current_name.empty()) {
            stream >> materials[current_name].base_color.x >> materials[current_name].base_color.y >> materials[current_name].base_color.z;
        } else if (tag == "Pm" && !current_name.empty()) {
            stream >> materials[current_name].metallic;
        } else if (tag == "Pr" && !current_name.empty()) {
            stream >> materials[current_name].roughness;
        } else if (tag == "Ns" && !current_name.empty()) {
            float ns = 0.0f;
            stream >> ns;
            materials[current_name].roughness = std::clamp(1.0f - ns / 1000.0f, 0.04f, 1.0f);
        } else if ((tag == "map_Kd" || tag == "map_BaseColor") && !current_name.empty()) {
            const std::string texture_name = read_mtl_texture_name(stream);
            materials[current_name].base_color_texture_path = resolve_texture_path(path, texture_name);
        } else if ((tag == "map_Pr" || tag == "map_Roughness" || tag == "map_Pm" || tag == "map_Metallic") && !current_name.empty()) {
            const std::string texture_name = read_mtl_texture_name(stream);
            materials[current_name].metallic_roughness_texture_path = resolve_texture_path(path, texture_name);
        } else if ((tag == "map_Bump" || tag == "bump" || tag == "map_Kn" || tag == "norm") && !current_name.empty()) {
            const std::string texture_name = read_mtl_texture_name(stream);
            materials[current_name].normal_texture_path = resolve_texture_path(path, texture_name);
        } else if ((tag == "map_Ke" || tag == "map_Emissive") && !current_name.empty()) {
            const std::string texture_name = read_mtl_texture_name(stream);
            materials[current_name].emissive_texture_path = resolve_texture_path(path, texture_name);
        }
    }
    return materials;
}

ObjPartBuilder& part_for_material(std::vector<ObjPartBuilder>& builders, const std::string& material_name, const std::map<std::string, Material>& materials) {
    const std::string name = material_name.empty() ? "default" : material_name;
    auto it = std::find_if(builders.begin(), builders.end(), [&name](const ObjPartBuilder& part) {
        return part.name == name;
    });
    if (it != builders.end()) {
        return *it;
    }

    ObjPartBuilder builder;
    builder.name = name;
    if (auto material = materials.find(name); material != materials.end()) {
        builder.material = material->second;
    }
    builders.push_back(std::move(builder));
    return builders.back();
}

void normalize_parts(std::vector<MeshPart>& parts) {
    Vec3 min_v{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    Vec3 max_v{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };
    bool has_vertex = false;
    for (const MeshPart& part : parts) {
        for (const Vertex& vertex : part.vertices) {
            min_v.x = std::min(min_v.x, vertex.position.x);
            min_v.y = std::min(min_v.y, vertex.position.y);
            min_v.z = std::min(min_v.z, vertex.position.z);
            max_v.x = std::max(max_v.x, vertex.position.x);
            max_v.y = std::max(max_v.y, vertex.position.y);
            max_v.z = std::max(max_v.z, vertex.position.z);
            has_vertex = true;
        }
    }
    if (!has_vertex) {
        return;
    }

    const Vec3 center = (min_v + max_v) * 0.5f;
    const Vec3 extents = max_v - min_v;
    const float max_extent = std::max({extents.x, extents.y, extents.z, 0.001f});
    const float scale = 2.5f / max_extent;
    for (MeshPart& part : parts) {
        float radius = 0.001f;
        for (Vertex& vertex : part.vertices) {
            vertex.position = (vertex.position - center) * scale;
            radius = std::max(radius, length(vertex.position));
        }
        part.bounds_radius = radius;
    }
}

ImportedModel load_obj(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("could not open OBJ file");
    }

    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;
    std::map<std::string, Material> materials;
    std::vector<ObjPartBuilder> builders;
    std::string current_material = "default";

    std::string line;
    while (std::getline(file, line)) {
        std::stringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag == "v") {
            Vec3 p;
            stream >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "vt") {
            Vec2 uv;
            stream >> uv.x >> uv.y;
            uvs.push_back(uv);
        } else if (tag == "vn") {
            Vec3 n;
            stream >> n.x >> n.y >> n.z;
            normals.push_back(normalize(n));
        } else if (tag == "mtllib") {
            std::string mtl_name;
            stream >> mtl_name;
            auto loaded = load_mtl_file(path.parent_path() / mtl_name);
            materials.insert(loaded.begin(), loaded.end());
        } else if (tag == "usemtl") {
            stream >> current_material;
        } else if (tag == "f") {
            std::vector<ObjIndex> face;
            std::string token;
            while (stream >> token) {
                face.push_back(parse_obj_index(token));
            }
            if (face.size() < 3) {
                continue;
            }

            ObjPartBuilder& part = part_for_material(builders, current_material, materials);
            for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                ObjIndex tri[] = {face[0], face[i], face[i + 1]};
                Vec3 triangle_positions[3]{};
                for (int corner = 0; corner < 3; ++corner) {
                    const int pi = resolve_index(tri[corner].position, static_cast<int>(positions.size()));
                    if (pi < 0 || pi >= static_cast<int>(positions.size())) {
                        throw std::runtime_error("OBJ face references an invalid vertex");
                    }
                    triangle_positions[corner] = positions[pi];
                }
                const Vec3 face_normal = normalize(cross(triangle_positions[1] - triangle_positions[0], triangle_positions[2] - triangle_positions[0]));

                for (int corner = 0; corner < 3; ++corner) {
                    Vertex vertex;
                    vertex.position = triangle_positions[corner];
                    const int ni = resolve_index(tri[corner].normal, static_cast<int>(normals.size()));
                    vertex.normal = ni >= 0 && ni < static_cast<int>(normals.size()) ? normals[ni] : face_normal;
                    const int uvi = resolve_index(tri[corner].uv, static_cast<int>(uvs.size()));
                    vertex.uv = uvi >= 0 && uvi < static_cast<int>(uvs.size()) ? uvs[uvi] : Vec2{};
                    part.indices.push_back(static_cast<std::uint32_t>(part.vertices.size()));
                    part.vertices.push_back(vertex);
                }
            }
        }
    }

    ImportedModel imported;
    for (ObjPartBuilder& builder : builders) {
        if (builder.vertices.empty()) {
            continue;
        }
        MeshPart part;
        part.name = builder.name;
        part.primitive = MeshPrimitive::Custom;
        part.material = builder.material;
        part.vertices = std::move(builder.vertices);
        part.indices = std::move(builder.indices);
        imported.parts.push_back(std::move(part));
    }
    normalize_parts(imported.parts);
    return imported;
}

} // namespace

ImportedModel import_model_file(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".obj" || ext == ".OBJ") {
        return load_obj(path);
    }
    throw std::runtime_error("unsupported import format; currently C++ supports OBJ while glTF is still in the Rust reference");
}

std::string supported_import_extensions() {
    return ".obj";
}

} // namespace Forge
