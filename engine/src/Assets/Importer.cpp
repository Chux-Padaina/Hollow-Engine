// Implements Importer.hpp: parses glTF/GLB (via Json.hpp) and Wavefront OBJ
// (with its companion MTL material files) into ImportedPart data. All
// parsing helpers are file-local; import_model_file() dispatches on the file
// extension.

#include "Hollow/Assets/Importer.hpp"
#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Json.hpp"
#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace Hollow {
namespace {

struct GltfDocument {
    JsonValue json;
    std::vector<std::vector<unsigned char>> buffers;
    std::vector<std::string> image_paths;
    std::vector<int> node_joint; // node index -> skeleton joint (skinned/animated files)
};

struct AttributeReader {
    const unsigned char* data = nullptr;
    int component_type = 0;
    int component_count = 0;
    int count = 0;
    int stride = 0;
    bool normalized = false;
};

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

Mat4 gltf_node_transform(const JsonValue& node) {
    if (const JsonValue* matrix = node.find("matrix"); matrix != nullptr && matrix->is_array() && matrix->array_value.size() == 16) {
        Mat4 out;
        for (std::size_t i = 0; i < 16; ++i) {
            out.m[i] = matrix->at(i).as_float();
        }
        return out;
    }

    Vec3 translation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    if (const JsonValue* t = node.find("translation"); t != nullptr && t->size() >= 3) {
        translation = {t->at(0).as_float(), t->at(1).as_float(), t->at(2).as_float()};
    }
    if (const JsonValue* sc = node.find("scale"); sc != nullptr && sc->size() >= 3) {
        scale = {sc->at(0).as_float(1.0f), sc->at(1).as_float(1.0f), sc->at(2).as_float(1.0f)};
    }
    // Build the rotation straight from the quaternion; going through Euler
    // angles loses precision near +-90 degrees of pitch.
    Mat4 rotation = Mat4::identity();
    if (const JsonValue* r = node.find("rotation"); r != nullptr && r->size() >= 4) {
        float x = r->at(0).as_float();
        float y = r->at(1).as_float();
        float z = r->at(2).as_float();
        float w = r->at(3).as_float(1.0f);
        const float len = std::sqrt(x * x + y * y + z * z + w * w);
        if (len > 1e-8f) {
            x /= len;
            y /= len;
            z /= len;
            w /= len;
        }
        rotation.m[0] = 1.0f - 2.0f * (y * y + z * z);
        rotation.m[1] = 2.0f * (x * y + z * w);
        rotation.m[2] = 2.0f * (x * z - y * w);
        rotation.m[4] = 2.0f * (x * y - z * w);
        rotation.m[5] = 1.0f - 2.0f * (x * x + z * z);
        rotation.m[6] = 2.0f * (y * z + x * w);
        rotation.m[8] = 2.0f * (x * z + y * w);
        rotation.m[9] = 2.0f * (y * z - x * w);
        rotation.m[10] = 1.0f - 2.0f * (x * x + y * y);
    }
    return Mat4::translation(translation) * rotation * Mat4::scale(scale);
}

int component_size(int component_type) {
    switch (component_type) {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    case 5125:
    case 5126:
        return 4;
    default:
        throw std::runtime_error("unsupported glTF accessor component type");
    }
}

int type_component_count(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    throw std::runtime_error("unsupported glTF accessor type");
}

float read_component(const unsigned char* data, int component_type, bool normalized) {
    switch (component_type) {
    case 5120: {
        const auto value = *reinterpret_cast<const signed char*>(data);
        return normalized ? std::max(-1.0f, static_cast<float>(value) / 127.0f) : static_cast<float>(value);
    }
    case 5121: {
        const auto value = *data;
        return normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
    }
    case 5122: {
        std::int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return normalized ? std::max(-1.0f, static_cast<float>(value) / 32767.0f) : static_cast<float>(value);
    }
    case 5123: {
        std::uint16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
    }
    case 5125: {
        std::uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(value);
    }
    case 5126: {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        throw std::runtime_error("unsupported glTF component type");
    }
}

std::uint32_t read_index_component(const unsigned char* data, int component_type) {
    switch (component_type) {
    case 5121:
        return *data;
    case 5123: {
        std::uint16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    case 5125: {
        std::uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        throw std::runtime_error("unsupported glTF index component type");
    }
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
    std::filesystem::path texture_path = path_from_utf8(texture_name);
    if (texture_path.is_relative()) {
        texture_path = material_path.parent_path() / texture_path;
    }
    return to_utf8(texture_path.lexically_normal());
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
        log_warning(std::format("OBJ material library '{}' not found", to_utf8(path)));
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
        } else if (tag == "Ke" && !current_name.empty()) {
            stream >> materials[current_name].emissive_factor.x >> materials[current_name].emissive_factor.y >> materials[current_name].emissive_factor.z;
        } else if ((tag == "d" || tag == "Tr") && !current_name.empty()) {
            float value = 1.0f;
            stream >> value;
            // OBJ has two conventions for the same concept: `d` is opacity
            // (1 = fully opaque), `Tr` is transparency (0 = fully opaque).
            const float opacity = std::clamp(tag == "d" ? value : 1.0f - value, 0.0f, 1.0f);
            materials[current_name].base_color_alpha = opacity;
            if (opacity < 0.999f) {
                materials[current_name].alpha_mode = AlphaMode::Blend;
            }
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

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("unexpected end of GLB file");
    }
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

std::vector<unsigned char> decode_data_uri(const std::string& uri) {
    const std::size_t comma = uri.find(',');
    if (comma == std::string::npos || uri.find(";base64") == std::string::npos) {
        throw std::runtime_error("only base64 data URIs are supported");
    }

    std::vector<unsigned char> out;
    out.reserve((uri.size() - comma) * 3 / 4);
    std::uint32_t value = 0;
    int bits = 0;
    for (std::size_t i = comma + 1; i < uri.size(); ++i) {
        const int decoded = base64_value(uri[i]);
        if (decoded < 0) {
            continue; // whitespace / padding
        }
        // Keep only the bits still waiting to be emitted so the accumulator
        // never overflows (the old version shifted an int without bound).
        value = ((value << 6) | static_cast<std::uint32_t>(decoded)) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((value >> bits) & 0xFFu));
        }
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decode_uri_path(const std::string& uri) {
    std::string out;
    out.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = hex_value(uri[i + 1]);
            const int lo = hex_value(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(uri[i]);
    }
    return out;
}

// Embedded glTF images are written to %TEMP%/HollowEngine so WIC can decode
// them from a path. The file name includes a hash of the model's full path
// (and its size/time), so two different models that share a file name no
// longer overwrite each other's textures.
std::filesystem::path extracted_image_path(const std::filesystem::path& model_path, int index, const std::string& mime_type) {
    std::string ext = ".png";
    if (mime_type == "image/jpeg") {
        ext = ".jpg";
    } else if (mime_type == "image/webp") {
        ext = ".webp";
    }
    std::error_code ec;
    std::string identity = to_utf8(std::filesystem::absolute(model_path, ec));
    identity += "|" + std::to_string(std::filesystem::file_size(model_path, ec));
    const auto write_time = std::filesystem::last_write_time(model_path, ec);
    identity += "|" + std::to_string(write_time.time_since_epoch().count());
    const std::size_t hash = std::hash<std::string>{}(identity);

    const std::filesystem::path directory = std::filesystem::temp_directory_path(ec) / "HollowEngine";
    std::filesystem::create_directories(directory, ec);
    return directory / std::format("{}_{:016x}_{}{}", to_utf8(model_path.stem()), hash, index, ext);
}

void write_extracted_image(const std::filesystem::path& path, const unsigned char* data, std::size_t size) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == size) {
        return; // already extracted by an earlier import of the same file
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) {
        log_warning(std::format("Could not extract embedded texture to '{}'", to_utf8(path)));
    }
}

AttributeReader accessor_reader(const GltfDocument& document, int accessor_index) {
    const JsonValue& accessors = document.json.at("accessors");
    if (!accessors.is_array() || accessor_index < 0 || accessor_index >= static_cast<int>(accessors.array_value.size())) {
        throw std::runtime_error("glTF accessor index out of range");
    }
    const JsonValue& accessor = accessors.at(static_cast<std::size_t>(accessor_index));
    const int component_type = accessor.at("componentType").as_int();
    const int component_count = type_component_count(accessor.at("type").as_string());
    const int count = accessor.at("count").as_int();
    if (count < 0) {
        throw std::runtime_error("glTF accessor has a negative count");
    }

    const int buffer_view_index = accessor.at("bufferView").as_int(-1);
    if (buffer_view_index < 0) {
        // Per spec an accessor without a bufferView is all zeros (sparse
        // accessors aren't supported; their base values are still used).
        AttributeReader zeros;
        zeros.component_type = component_type;
        zeros.component_count = component_count;
        zeros.count = count;
        return zeros;
    }

    const JsonValue& buffer_view = document.json.at("bufferViews").at(static_cast<std::size_t>(buffer_view_index));
    const int buffer_index = buffer_view.at("buffer").as_int(-1);
    if (buffer_index < 0 || buffer_index >= static_cast<int>(document.buffers.size())) {
        throw std::runtime_error("glTF buffer index out of range");
    }

    const std::size_t byte_offset = static_cast<std::size_t>(std::max(0, buffer_view.at("byteOffset").as_int())) + static_cast<std::size_t>(std::max(0, accessor.at("byteOffset").as_int()));
    const int packed_stride = component_size(component_type) * component_count;
    const int stride = buffer_view.at("byteStride").as_int(packed_stride);
    const auto& buffer = document.buffers[static_cast<std::size_t>(buffer_index)];
    const std::size_t needed = count == 0 ? 0 : byte_offset + static_cast<std::size_t>(stride) * static_cast<std::size_t>(count - 1) + static_cast<std::size_t>(packed_stride);
    if (stride < packed_stride || needed > buffer.size()) {
        throw std::runtime_error("glTF accessor range exceeds buffer");
    }

    return {
        buffer.data() + byte_offset,
        component_type,
        component_count,
        count,
        stride,
        accessor.at("normalized").as_bool(false),
    };
}

Vec3 read_vec3(const AttributeReader& reader, int index, Vec3 fallback = {}) {
    if (index < 0 || index >= reader.count || reader.component_count < 3) {
        return fallback;
    }
    if (reader.data == nullptr) {
        return {};
    }
    const unsigned char* data = reader.data + static_cast<std::size_t>(index) * static_cast<std::size_t>(reader.stride);
    const int size = component_size(reader.component_type);
    return {
        read_component(data, reader.component_type, reader.normalized),
        read_component(data + size, reader.component_type, reader.normalized),
        read_component(data + size * 2, reader.component_type, reader.normalized),
    };
}

Vec2 read_vec2(const AttributeReader& reader, int index, Vec2 fallback = {}) {
    if (index < 0 || index >= reader.count || reader.component_count < 2) {
        return fallback;
    }
    if (reader.data == nullptr) {
        return {};
    }
    const unsigned char* data = reader.data + static_cast<std::size_t>(index) * static_cast<std::size_t>(reader.stride);
    const int size = component_size(reader.component_type);
    return {
        read_component(data, reader.component_type, reader.normalized),
        read_component(data + size, reader.component_type, reader.normalized),
    };
}

// All components of element `index` as floats (empty when out of range).
std::vector<float> read_element(const AttributeReader& reader, int index) {
    std::vector<float> out;
    if (reader.data == nullptr || index < 0 || index >= reader.count) {
        return out;
    }
    const unsigned char* data = reader.data + static_cast<std::size_t>(index) * static_cast<std::size_t>(reader.stride);
    const int size = component_size(reader.component_type);
    for (int c = 0; c < reader.component_count; ++c) {
        out.push_back(read_component(data + c * size, reader.component_type, reader.normalized));
    }
    return out;
}

std::vector<std::uint32_t> read_indices(const GltfDocument& document, const JsonValue& primitive, int vertex_count) {
    std::vector<std::uint32_t> out;
    if (const JsonValue* indices = primitive.find("indices")) {
        const AttributeReader reader = accessor_reader(document, indices->as_int(-1));
        if (reader.data == nullptr) {
            throw std::runtime_error("glTF index accessor has no data");
        }
        out.reserve(static_cast<std::size_t>(reader.count));
        for (int i = 0; i < reader.count; ++i) {
            out.push_back(read_index_component(reader.data + static_cast<std::size_t>(i) * static_cast<std::size_t>(reader.stride), reader.component_type));
        }
    } else {
        out.reserve(static_cast<std::size_t>(vertex_count));
        for (int i = 0; i < vertex_count; ++i) {
            out.push_back(static_cast<std::uint32_t>(i));
        }
    }

    // Drop triangles that reference vertices that don't exist instead of
    // handing out-of-range indices to the GPU.
    std::vector<std::uint32_t> valid;
    valid.reserve(out.size());
    std::size_t dropped = 0;
    for (std::size_t i = 0; i + 2 < out.size(); i += 3) {
        const auto limit = static_cast<std::uint32_t>(vertex_count);
        if (out[i] < limit && out[i + 1] < limit && out[i + 2] < limit) {
            valid.insert(valid.end(), {out[i], out[i + 1], out[i + 2]});
        } else {
            ++dropped;
        }
    }
    if (dropped > 0) {
        log_warning(std::format("glTF primitive: dropped {} triangle(s) with out-of-range indices", dropped));
    }
    return valid;
}

std::string texture_image_path(const GltfDocument& document, const JsonValue& texture_info) {
    if (!texture_info.is_object()) {
        return {};
    }
    const int texture_index = texture_info.at("index").as_int(-1);
    const JsonValue& textures = document.json.at("textures");
    if (!textures.is_array() || texture_index < 0 || texture_index >= static_cast<int>(textures.array_value.size())) {
        return {};
    }
    const int source = textures.at(static_cast<std::size_t>(texture_index)).at("source").as_int(-1);
    if (source < 0 || source >= static_cast<int>(document.image_paths.size())) {
        return {};
    }
    return document.image_paths[static_cast<std::size_t>(source)];
}

Material read_gltf_material(const GltfDocument& document, int material_index) {
    Material material;
    material.base_color = {1.0f, 1.0f, 1.0f};
    material.roughness = 1.0f;
    material.metallic = 1.0f;
    const JsonValue& materials = document.json.at("materials");
    if (!materials.is_array() || material_index < 0 || material_index >= static_cast<int>(materials.array_value.size())) {
        // glTF's default material is metallic=1, roughness=1; a plain
        // dielectric reads far better for untextured geometry.
        material.metallic = 0.0f;
        return material;
    }

    const JsonValue& material_json = materials.at(static_cast<std::size_t>(material_index));
    const JsonValue& pbr = material_json.at("pbrMetallicRoughness");
    if (pbr.is_object()) {
        const JsonValue& factor = pbr.at("baseColorFactor");
        if (factor.is_array() && factor.array_value.size() >= 3) {
            material.base_color = {factor.at(0).as_float(1.0f), factor.at(1).as_float(1.0f), factor.at(2).as_float(1.0f)};
            if (factor.array_value.size() >= 4) {
                material.base_color_alpha = std::clamp(factor.at(3).as_float(1.0f), 0.0f, 1.0f);
            }
        }
        material.metallic = std::clamp(pbr.at("metallicFactor").as_float(1.0f), 0.0f, 1.0f);
        material.roughness = std::clamp(pbr.at("roughnessFactor").as_float(1.0f), 0.04f, 1.0f);
        material.base_color_texture_path = texture_image_path(document, pbr.at("baseColorTexture"));
        material.metallic_roughness_texture_path = texture_image_path(document, pbr.at("metallicRoughnessTexture"));
    }
    const JsonValue& normal_texture = material_json.at("normalTexture");
    material.normal_texture_path = texture_image_path(document, normal_texture);
    material.normal_strength = normal_texture.at("scale").as_float(1.0f);

    const JsonValue& emissive_factor = material_json.at("emissiveFactor");
    if (emissive_factor.is_array() && emissive_factor.array_value.size() >= 3) {
        material.emissive_factor = {emissive_factor.at(0).as_float(0.0f), emissive_factor.at(1).as_float(0.0f), emissive_factor.at(2).as_float(0.0f)};
    }
    if (const JsonValue* strength = material_json.at("extensions").at("KHR_materials_emissive_strength").find("emissiveStrength")) {
        material.emissive_factor *= strength->as_float(1.0f);
    }
    material.emissive_texture_path = texture_image_path(document, material_json.at("emissiveTexture"));

    const JsonValue& occlusion_texture = material_json.at("occlusionTexture");
    material.occlusion_texture_path = texture_image_path(document, occlusion_texture);
    material.occlusion_strength = std::clamp(occlusion_texture.at("strength").as_float(1.0f), 0.0f, 1.0f);

    const std::string alpha_mode = material_json.at("alphaMode").as_string("OPAQUE");
    if (alpha_mode == "MASK") {
        material.alpha_mode = AlphaMode::Mask;
    } else if (alpha_mode == "BLEND") {
        material.alpha_mode = AlphaMode::Blend;
    } else {
        material.alpha_mode = AlphaMode::Opaque;
    }
    material.alpha_cutoff = material_json.at("alphaCutoff").as_float(0.5f);
    material.double_sided = material_json.at("doubleSided").as_bool(false);

    // Material extensions.
    const JsonValue& ext = material_json.at("extensions");
    if (ext.find("KHR_materials_unlit") != nullptr) {
        material.unlit = true;
    }
    if (const JsonValue* ior = ext.at("KHR_materials_ior").find("ior")) {
        material.ior = std::max(ior->as_float(1.5f), 1.0f);
    }
    if (const JsonValue& spec = ext.at("KHR_materials_specular"); spec.is_object()) {
        material.specular = std::clamp(spec.at("specularFactor").as_float(1.0f), 0.0f, 1.0f);
    }
    if (const JsonValue& cc = ext.at("KHR_materials_clearcoat"); cc.is_object()) {
        material.clearcoat = std::clamp(cc.at("clearcoatFactor").as_float(0.0f), 0.0f, 1.0f);
        material.clearcoat_roughness = std::clamp(cc.at("clearcoatRoughnessFactor").as_float(0.0f), 0.0f, 1.0f);
        material.clearcoat_texture_path = texture_image_path(document, cc.at("clearcoatTexture"));
    }
    if (const JsonValue& sheen = ext.at("KHR_materials_sheen"); sheen.is_object()) {
        const JsonValue& c = sheen.at("sheenColorFactor");
        if (c.is_array() && c.array_value.size() >= 3) {
            material.sheen_color = {c.at(0).as_float(0.0f), c.at(1).as_float(0.0f), c.at(2).as_float(0.0f)};
        }
        material.sheen_roughness = std::clamp(sheen.at("sheenRoughnessFactor").as_float(0.0f), 0.0f, 1.0f);
        material.sheen_texture_path = texture_image_path(document, sheen.at("sheenColorTexture"));
    }
    if (const JsonValue& tr = ext.at("KHR_materials_transmission"); tr.is_object()) {
        material.transmission = std::clamp(tr.at("transmissionFactor").as_float(0.0f), 0.0f, 1.0f);
        material.transmission_texture_path = texture_image_path(document, tr.at("transmissionTexture"));
    }
    if (const JsonValue& vol = ext.at("KHR_materials_volume"); vol.is_object()) {
        material.thickness = std::max(vol.at("thicknessFactor").as_float(0.0f), 0.0f);
        const JsonValue& c = vol.at("attenuationColor");
        if (c.is_array() && c.array_value.size() >= 3) {
            material.attenuation_color = {c.at(0).as_float(1.0f), c.at(1).as_float(1.0f), c.at(2).as_float(1.0f)};
        }
        material.attenuation_distance = vol.at("attenuationDistance").as_float(0.0f);
    }
    if (const JsonValue& an = ext.at("KHR_materials_anisotropy"); an.is_object()) {
        material.anisotropy = std::clamp(an.at("anisotropyStrength").as_float(0.0f), -1.0f, 1.0f);
        material.anisotropy_rotation = an.at("anisotropyRotation").as_float(0.0f);
    }
    if (const JsonValue& dt = ext.at("KHR_materials_diffuse_transmission"); dt.is_object()) {
        // Thin translucency (leaves, cloth): approximate with subsurface.
        material.subsurface = std::clamp(dt.at("diffuseTransmissionFactor").as_float(0.0f), 0.0f, 1.0f);
        const JsonValue& c = dt.at("diffuseTransmissionColorFactor");
        if (c.is_array() && c.array_value.size() >= 3) {
            material.subsurface_color = {c.at(0).as_float(1.0f), c.at(1).as_float(1.0f), c.at(2).as_float(1.0f)};
        }
    }
    // KHR_texture_transform on the base color texture drives the UV transform.
    if (pbr.is_object()) {
        if (const JsonValue& tt = pbr.at("baseColorTexture").at("extensions").at("KHR_texture_transform"); tt.is_object()) {
            const JsonValue& sc = tt.at("scale");
            if (sc.is_array() && sc.array_value.size() >= 2) {
                material.uv_scale_x = sc.at(0).as_float(1.0f);
                material.uv_scale_y = sc.at(1).as_float(1.0f);
            }
            const JsonValue& of = tt.at("offset");
            if (of.is_array() && of.array_value.size() >= 2) {
                material.uv_offset_x = of.at(0).as_float(0.0f);
                material.uv_offset_y = of.at(1).as_float(0.0f);
            }
        }
    }
    return material;
}

ImportedPart build_gltf_primitive(const GltfDocument& document, const JsonValue& mesh_json, const JsonValue& primitive, int primitive_index, const Mat4& transform) {
    if (primitive.at("mode").as_int(4) != 4) {
        throw std::runtime_error("only triangle-list primitives are supported");
    }
    if (primitive.at("extensions").find("KHR_draco_mesh_compression") != nullptr || document.json.at("extensionsRequired").is_array()) {
        for (const JsonValue& required : document.json.at("extensionsRequired").array_value) {
            const std::string name = required.as_string();
            if (name == "KHR_draco_mesh_compression" || name == "EXT_meshopt_compression") {
                throw std::runtime_error("compressed meshes (" + name + ") are not supported");
            }
        }
    }

    const JsonValue& attributes = primitive.at("attributes");
    const int position_accessor = attributes.at("POSITION").as_int(-1);
    if (position_accessor < 0) {
        throw std::runtime_error("primitive has no POSITION attribute");
    }

    const AttributeReader positions = accessor_reader(document, position_accessor);
    const int normal_accessor = attributes.at("NORMAL").as_int(-1);
    const int uv_accessor = attributes.at("TEXCOORD_0").as_int(-1);
    const int color_accessor = attributes.at("COLOR_0").as_int(-1);
    const AttributeReader normals = normal_accessor >= 0 ? accessor_reader(document, normal_accessor) : AttributeReader{};
    const AttributeReader uvs = uv_accessor >= 0 ? accessor_reader(document, uv_accessor) : AttributeReader{};
    const AttributeReader colors = color_accessor >= 0 ? accessor_reader(document, color_accessor) : AttributeReader{};

    ImportedPart part;
    part.name = mesh_json.at("name").as_string("mesh") + ":" + std::to_string(primitive_index);
    part.material = read_gltf_material(document, primitive.at("material").as_int(-1));
    part.vertices.reserve(static_cast<std::size_t>(positions.count));

    // Normals need the inverse-transpose under non-uniform node scale.
    const Mat4 normal_transform = normal_matrix(transform);
    for (int i = 0; i < positions.count; ++i) {
        Vertex vertex;
        vertex.position = transform_point(transform, read_vec3(positions, i));
        vertex.normal = normalize(transform_vector(normal_transform, read_vec3(normals, i, {0.0f, 1.0f, 0.0f})));
        vertex.uv = read_vec2(uvs, i);
        vertex.color = read_vec3(colors, i, {1.0f, 1.0f, 1.0f});
        part.vertices.push_back(vertex);
    }

    part.indices = read_indices(document, primitive, positions.count);
    // A mirroring node transform (negative determinant) flips triangle
    // winding; swap two corners so front faces stay front faces.
    const Vec3 x_axis{transform.m[0], transform.m[1], transform.m[2]};
    const Vec3 y_axis{transform.m[4], transform.m[5], transform.m[6]};
    const Vec3 z_axis{transform.m[8], transform.m[9], transform.m[10]};
    if (dot(cross(x_axis, y_axis), z_axis) < 0.0f) {
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            std::swap(part.indices[i + 1], part.indices[i + 2]);
        }
    }
    if (normal_accessor < 0) {
        // No normals in the file: derive flat-ish normals from the faces.
        std::vector<Vec3> accumulated(part.vertices.size(), Vec3{});
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            const Vec3 a = part.vertices[part.indices[i]].position;
            const Vec3 b = part.vertices[part.indices[i + 1]].position;
            const Vec3 c = part.vertices[part.indices[i + 2]].position;
            const Vec3 face = cross(b - a, c - a);
            for (int k = 0; k < 3; ++k) {
                accumulated[part.indices[i + static_cast<std::size_t>(k)]] += face;
            }
        }
        for (std::size_t i = 0; i < part.vertices.size(); ++i) {
            const Vec3 n = normalize(accumulated[i]);
            part.vertices[i].normal = length(n) > 0.5f ? n : Vec3{0.0f, 1.0f, 0.0f};
        }
    }
    generate_tangents(part.vertices, part.indices);
    return part;
}

void attach_gltf_skin(const GltfDocument& document, const JsonValue& skin_json, const JsonValue& primitive, ImportedPart& part) {
    const JsonValue& attributes = primitive.at("attributes");
    const int joints_accessor = attributes.at("JOINTS_0").as_int(-1);
    const int weights_accessor = attributes.at("WEIGHTS_0").as_int(-1);
    if (joints_accessor < 0 || weights_accessor < 0) {
        return;
    }
    const AttributeReader joints = accessor_reader(document, joints_accessor);
    const AttributeReader weights = accessor_reader(document, weights_accessor);
    const JsonValue& joint_nodes = skin_json.at("joints");
    if (!joint_nodes.is_array() || joint_nodes.array_value.empty() || joints.count < static_cast<int>(part.vertices.size())) {
        return;
    }
    const int joint_count = static_cast<int>(joint_nodes.array_value.size());
    for (const JsonValue& node : joint_nodes.array_value) {
        const int index = node.as_int(-1);
        part.skin_joints.push_back(index >= 0 && index < static_cast<int>(document.node_joint.size()) ? document.node_joint[static_cast<std::size_t>(index)] : 0);
    }
    const int ibm_accessor = skin_json.at("inverseBindMatrices").as_int(-1);
    const AttributeReader ibm = ibm_accessor >= 0 ? accessor_reader(document, ibm_accessor) : AttributeReader{};
    for (int j = 0; j < joint_count; ++j) {
        Mat4 m = Mat4::identity();
        const std::vector<float> values = read_element(ibm, j);
        if (values.size() == 16) {
            std::copy(values.begin(), values.end(), m.m.begin());
        }
        part.inverse_bind.push_back(m);
    }
    part.skin.resize(part.vertices.size());
    for (std::size_t v = 0; v < part.vertices.size(); ++v) {
        const std::vector<float> j = read_element(joints, static_cast<int>(v));
        const std::vector<float> w = read_element(weights, static_cast<int>(v));
        SkinInfluence influence;
        float total = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            const int joint = k < j.size() ? static_cast<int>(j[k]) : 0;
            influence.joints[k] = static_cast<std::uint16_t>(std::clamp(joint, 0, joint_count - 1));
            influence.weights[k] = k < w.size() ? std::max(w[k], 0.0f) : 0.0f;
            total += influence.weights[k];
        }
        if (total <= 1e-6f) {
            influence.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        } else {
            for (float& weight : influence.weights) {
                weight /= total;
            }
        }
        part.skin[v] = influence;
    }
}

// Every node becomes a joint (parents first), so animation channels can
// target any node; rest pose = the node's TRS.
std::shared_ptr<Skeleton> build_gltf_skeleton(GltfDocument& document, const std::vector<int>& roots) {
    const JsonValue& nodes = document.json.at("nodes");
    const int node_count = static_cast<int>(nodes.size());
    auto skeleton = std::make_shared<Skeleton>();
    document.node_joint.assign(static_cast<std::size_t>(node_count), -1);
    std::function<void(int, int, int)> visit = [&](int node_index, int parent_joint, int depth) {
        if (node_index < 0 || node_index >= node_count || depth > 128 || document.node_joint[static_cast<std::size_t>(node_index)] >= 0) {
            return;
        }
        const JsonValue& node = nodes.at(static_cast<std::size_t>(node_index));
        Joint joint;
        joint.name = node.at("name").as_string("node" + std::to_string(node_index));
        joint.parent = parent_joint;
        const Mat4 local = gltf_node_transform(node);
        joint.rest.translation = {local.m[12], local.m[13], local.m[14]};
        const Vec3 sx{local.m[0], local.m[1], local.m[2]};
        const Vec3 sy{local.m[4], local.m[5], local.m[6]};
        const Vec3 sz{local.m[8], local.m[9], local.m[10]};
        joint.rest.scale = {length(sx), length(sy), length(sz)};
        Mat4 rotation = Mat4::identity();
        for (int c = 0; c < 3; ++c) {
            const float len = std::max(c == 0 ? joint.rest.scale.x : (c == 1 ? joint.rest.scale.y : joint.rest.scale.z), 1e-8f);
            for (int r = 0; r < 3; ++r) {
                rotation.m[static_cast<std::size_t>(c * 4 + r)] = local.m[static_cast<std::size_t>(c * 4 + r)] / len;
            }
        }
        joint.rest.rotation = quat_from_matrix(rotation);
        const int index = static_cast<int>(skeleton->joints.size());
        skeleton->joints.push_back(joint);
        document.node_joint[static_cast<std::size_t>(node_index)] = index;
        for (const JsonValue& child : node.at("children").array_value) {
            visit(child.as_int(-1), index, depth + 1);
        }
    };
    for (int root : roots) {
        visit(root, -1, 0);
    }
    for (int i = 0; i < node_count; ++i) {
        visit(i, -1, 0); // nodes outside the scene
    }

    // Animations.
    for (const JsonValue& animation : document.json.at("animations").array_value) {
        AnimationClip clip;
        clip.name = animation.at("name").as_string("clip" + std::to_string(skeleton->clips.size()));
        const JsonValue& samplers = animation.at("samplers");
        for (const JsonValue& channel_json : animation.at("channels").array_value) {
            const JsonValue& target = channel_json.at("target");
            const int node = target.at("node").as_int(-1);
            const std::string path = target.at("path").as_string();
            const int sampler_index = channel_json.at("sampler").as_int(-1);
            if (node < 0 || node >= node_count || sampler_index < 0 || sampler_index >= static_cast<int>(samplers.size())) {
                continue;
            }
            AnimationChannel channel;
            channel.joint = document.node_joint[static_cast<std::size_t>(node)];
            if (path == "translation") {
                channel.path = AnimationPath::Translation;
            } else if (path == "rotation") {
                channel.path = AnimationPath::Rotation;
            } else if (path == "scale") {
                channel.path = AnimationPath::Scale;
            } else {
                continue; // morph weights
            }
            const JsonValue& sampler = samplers.at(static_cast<std::size_t>(sampler_index));
            const std::string interpolation = sampler.at("interpolation").as_string("LINEAR");
            channel.interpolation = interpolation == "STEP" ? Interpolation::Step : (interpolation == "CUBICSPLINE" ? Interpolation::CubicSpline : Interpolation::Linear);
            const AttributeReader input = accessor_reader(document, sampler.at("input").as_int(-1));
            const AttributeReader output = accessor_reader(document, sampler.at("output").as_int(-1));
            for (int k = 0; k < input.count; ++k) {
                const std::vector<float> t = read_element(input, k);
                channel.times.push_back(t.empty() ? 0.0f : t[0]);
            }
            for (int k = 0; k < output.count; ++k) {
                const std::vector<float> v = read_element(output, k);
                channel.values.insert(channel.values.end(), v.begin(), v.end());
            }
            if (channel.times.empty() || channel.values.empty()) {
                continue;
            }
            clip.duration = std::max(clip.duration, channel.times.back());
            clip.channels.push_back(std::move(channel));
        }
        if (!clip.channels.empty()) {
            skeleton->clips.push_back(std::move(clip));
        }
    }
    return skeleton;
}

void collect_gltf_node(const GltfDocument& document, int node_index, const Mat4& parent_transform, std::vector<ImportedPart>& parts, int depth) {
    const JsonValue& nodes = document.json.at("nodes");
    if (depth > 64 || !nodes.is_array() || node_index < 0 || node_index >= static_cast<int>(nodes.array_value.size())) {
        return;
    }

    const JsonValue& node = nodes.at(static_cast<std::size_t>(node_index));
    const Mat4 world = parent_transform * gltf_node_transform(node);
    const int mesh_index = node.at("mesh").as_int(-1);
    // Skinned meshes ignore their node transform: vertices are in bind
    // space and the joints place them (glTF 2.0, 3.7.3).
    const int skin_index = document.node_joint.empty() ? -1 : node.at("skin").as_int(-1);
    const JsonValue& skins = document.json.at("skins");
    const bool skinned = skin_index >= 0 && skins.is_array() && skin_index < static_cast<int>(skins.array_value.size());
    const JsonValue& meshes = document.json.at("meshes");
    if (mesh_index >= 0 && meshes.is_array() && mesh_index < static_cast<int>(meshes.array_value.size())) {
        const JsonValue& mesh_json = meshes.at(static_cast<std::size_t>(mesh_index));
        const JsonValue& primitives = mesh_json.at("primitives");
        if (primitives.is_array()) {
            for (std::size_t i = 0; i < primitives.array_value.size(); ++i) {
                try {
                    ImportedPart part = build_gltf_primitive(document, mesh_json, primitives.at(i), static_cast<int>(i), skinned ? Mat4::identity() : world);
                    if (skinned) {
                        attach_gltf_skin(document, skins.at(static_cast<std::size_t>(skin_index)), primitives.at(i), part);
                    }
                    if (!part.indices.empty()) {
                        parts.push_back(std::move(part));
                    }
                } catch (const std::exception& error) {
                    log_warning(std::format("glTF mesh '{}' primitive {} skipped: {}", mesh_json.at("name").as_string("?"), i, error.what()));
                }
            }
        }
    }

    const JsonValue& children = node.at("children");
    if (children.is_array()) {
        for (const JsonValue& child : children.array_value) {
            collect_gltf_node(document, child.as_int(-1), world, parts, depth + 1);
        }
    }
}

std::vector<int> gltf_scene_roots(const GltfDocument& document) {
    std::vector<int> roots;
    const JsonValue& scenes = document.json.at("scenes");
    if (!scenes.is_array() || scenes.array_value.empty()) {
        // No scene list: treat every node that isn't someone's child as a root.
        const JsonValue& nodes = document.json.at("nodes");
        std::vector<bool> is_child(nodes.size(), false);
        for (const JsonValue& node : nodes.array_value) {
            for (const JsonValue& child : node.at("children").array_value) {
                const int index = child.as_int(-1);
                if (index >= 0 && index < static_cast<int>(is_child.size())) {
                    is_child[static_cast<std::size_t>(index)] = true;
                }
            }
        }
        for (std::size_t i = 0; i < is_child.size(); ++i) {
            if (!is_child[i]) {
                roots.push_back(static_cast<int>(i));
            }
        }
        return roots;
    }
    const int scene_index = std::clamp(document.json.at("scene").as_int(0), 0, static_cast<int>(scenes.array_value.size()) - 1);
    for (const JsonValue& node : scenes.at(static_cast<std::size_t>(scene_index)).at("nodes").array_value) {
        roots.push_back(node.as_int(-1));
    }
    return roots;
}

void resolve_gltf_buffers_and_images(GltfDocument& document, const std::filesystem::path& path, const std::vector<unsigned char>& glb_bin_chunk = {}) {
    const std::filesystem::path base_dir = path.parent_path();
    for (const JsonValue& buffer : document.json.at("buffers").array_value) {
        const std::string uri = buffer.at("uri").as_string();
        if (uri.empty()) {
            document.buffers.push_back(glb_bin_chunk);
        } else if (uri.rfind("data:", 0) == 0) {
            document.buffers.push_back(decode_data_uri(uri));
        } else {
            document.buffers.push_back(read_binary_file(base_dir / path_from_utf8(decode_uri_path(uri))));
        }
    }

    const JsonValue& images = document.json.at("images");
    document.image_paths.resize(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        const JsonValue& image = images.at(i);
        const std::string uri = image.at("uri").as_string();
        if (!uri.empty()) {
            if (uri.rfind("data:", 0) == 0) {
                const std::vector<unsigned char> bytes = decode_data_uri(uri);
                const std::string mime = uri.find("image/jpeg") != std::string::npos ? "image/jpeg" : "image/png";
                const std::filesystem::path extracted = extracted_image_path(path, static_cast<int>(i), mime);
                write_extracted_image(extracted, bytes.data(), bytes.size());
                document.image_paths[i] = to_utf8(extracted);
            } else {
                document.image_paths[i] = to_utf8((base_dir / path_from_utf8(decode_uri_path(uri))).lexically_normal());
            }
            continue;
        }

        const int buffer_view_index = image.at("bufferView").as_int(-1);
        if (buffer_view_index < 0) {
            continue;
        }
        const JsonValue& buffer_view = document.json.at("bufferViews").at(static_cast<std::size_t>(buffer_view_index));
        const int buffer_index = buffer_view.at("buffer").as_int(-1);
        if (buffer_index < 0 || buffer_index >= static_cast<int>(document.buffers.size())) {
            continue;
        }
        const int byte_offset = buffer_view.at("byteOffset").as_int();
        const int byte_length = buffer_view.at("byteLength").as_int();
        const auto& buffer = document.buffers[static_cast<std::size_t>(buffer_index)];
        if (byte_offset < 0 || byte_length <= 0 || static_cast<std::size_t>(byte_offset) + static_cast<std::size_t>(byte_length) > buffer.size()) {
            continue;
        }
        const std::filesystem::path extracted = extracted_image_path(path, static_cast<int>(i), image.at("mimeType").as_string("image/png"));
        write_extracted_image(extracted, buffer.data() + byte_offset, static_cast<std::size_t>(byte_length));
        document.image_paths[i] = to_utf8(extracted);
    }
}

std::string lower_extension(const std::filesystem::path& path) {
    std::string ext = to_utf8(path.extension());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

GltfDocument load_gltf_document(const std::filesystem::path& path) {
    GltfDocument document;
    if (lower_extension(path) == ".glb") {
        const std::vector<unsigned char> bytes = read_binary_file(path);
        if (bytes.size() < 20 || read_u32_le(bytes, 0) != 0x46546C67 || read_u32_le(bytes, 4) != 2) {
            throw std::runtime_error("invalid or unsupported GLB file (only glTF 2.0)");
        }

        std::size_t offset = 12;
        std::string json_text;
        std::vector<unsigned char> bin_chunk;
        while (offset + 8 <= bytes.size()) {
            const std::uint32_t chunk_length = read_u32_le(bytes, offset);
            const std::uint32_t chunk_type = read_u32_le(bytes, offset + 4);
            offset += 8;
            if (offset + chunk_length > bytes.size()) {
                throw std::runtime_error("invalid GLB chunk length");
            }
            if (chunk_type == 0x4E4F534A) {
                json_text.assign(reinterpret_cast<const char*>(bytes.data() + offset), chunk_length);
            } else if (chunk_type == 0x004E4942) {
                bin_chunk.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk_length));
            }
            offset += chunk_length;
        }
        document.json = parse_json(json_text);
        resolve_gltf_buffers_and_images(document, path, bin_chunk);
    } else {
        document.json = parse_json(read_text_file(path));
        resolve_gltf_buffers_and_images(document, path);
    }
    return document;
}

ImportedModel load_gltf(const std::filesystem::path& path) {
    GltfDocument document = load_gltf_document(path);
    ImportedModel imported;
    const std::vector<int> roots = gltf_scene_roots(document);
    if (document.json.at("skins").size() > 0 || document.json.at("animations").size() > 0) {
        imported.skeleton = build_gltf_skeleton(document, roots);
        log_info(std::format("'{}': {} joints, {} animation clip(s)", to_utf8(path.filename()), imported.skeleton->joints.size(), imported.skeleton->clips.size()));
    }
    for (int root : roots) {
        collect_gltf_node(document, root, Mat4::identity(), imported.parts, 0);
    }
    if (imported.parts.empty()) {
        throw std::runtime_error("no triangle meshes found in glTF file");
    }
    return imported;
}

struct ObjVertexKey {
    int position;
    int uv;
    int normal;
    bool operator<(const ObjVertexKey& other) const {
        if (position != other.position) return position < other.position;
        if (uv != other.uv) return uv < other.uv;
        return normal < other.normal;
    }
};

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
    // Per part: (position, uv, normal) index triple -> output vertex, so
    // shared corners are emitted once instead of once per triangle.
    std::map<std::string, std::map<ObjVertexKey, std::uint32_t>> vertex_maps;
    std::string current_material = "default";
    std::size_t line_number = 0;

    std::string line;
    while (std::getline(file, line)) {
        ++line_number;
        std::stringstream stream(line);
        std::string tag;
        stream >> tag;
        try {
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
                std::getline(stream >> std::ws, mtl_name);
                while (!mtl_name.empty() && (mtl_name.back() == '\r' || mtl_name.back() == ' ')) {
                    mtl_name.pop_back();
                }
                auto loaded = load_mtl_file(path.parent_path() / path_from_utf8(mtl_name));
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
                auto& vertex_map = vertex_maps[part.name];
                std::vector<ObjVertexKey> keys;
                for (const ObjIndex& corner : face) {
                    const int pi = resolve_index(corner.position, static_cast<int>(positions.size()));
                    if (pi < 0 || pi >= static_cast<int>(positions.size())) {
                        throw std::runtime_error("face references an invalid vertex");
                    }
                    const int ti = resolve_index(corner.uv, static_cast<int>(uvs.size()));
                    const int ni = resolve_index(corner.normal, static_cast<int>(normals.size()));
                    keys.push_back({pi, ti >= 0 && ti < static_cast<int>(uvs.size()) ? ti : -1, ni >= 0 && ni < static_cast<int>(normals.size()) ? ni : -1});
                }
                const Vec3 face_normal = normalize(cross(positions[static_cast<std::size_t>(keys[1].position)] - positions[static_cast<std::size_t>(keys[0].position)],
                    positions[static_cast<std::size_t>(keys[2].position)] - positions[static_cast<std::size_t>(keys[0].position)]));

                auto emit = [&](const ObjVertexKey& key) -> std::uint32_t {
                    // Corners without an explicit normal take the face normal,
                    // so they can't be shared across faces.
                    if (key.normal >= 0) {
                        auto it = vertex_map.find(key);
                        if (it != vertex_map.end()) {
                            return it->second;
                        }
                    }
                    Vertex vertex;
                    vertex.position = positions[static_cast<std::size_t>(key.position)];
                    vertex.normal = key.normal >= 0 ? normals[static_cast<std::size_t>(key.normal)] : face_normal;
                    vertex.uv = key.uv >= 0 ? uvs[static_cast<std::size_t>(key.uv)] : Vec2{};
                    const auto index = static_cast<std::uint32_t>(part.vertices.size());
                    part.vertices.push_back(vertex);
                    if (key.normal >= 0) {
                        vertex_map[key] = index;
                    }
                    return index;
                };
                for (std::size_t i = 1; i + 1 < keys.size(); ++i) {
                    part.indices.push_back(emit(keys[0]));
                    part.indices.push_back(emit(keys[i]));
                    part.indices.push_back(emit(keys[i + 1]));
                }
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(std::format("OBJ line {}: {}", line_number, error.what()));
        }
    }

    ImportedModel imported;
    for (ObjPartBuilder& builder : builders) {
        if (builder.vertices.empty()) {
            continue;
        }
        ImportedPart part;
        part.name = builder.name;
        part.material = builder.material;
        part.vertices = std::move(builder.vertices);
        part.indices = std::move(builder.indices);
        generate_tangents(part.vertices, part.indices);
        imported.parts.push_back(std::move(part));
    }
    if (imported.parts.empty()) {
        throw std::runtime_error("no faces found in OBJ file");
    }
    return imported;
}

} // namespace

ImportedModel import_model_file(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    if (ext == ".obj") {
        return load_obj(path);
    }
    if (ext == ".gltf" || ext == ".glb") {
        return load_gltf(path);
    }
    throw std::runtime_error("unsupported import format; use .obj, .gltf, or .glb");
}

bool is_supported_model_file(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    return ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

const char* supported_import_extensions() {
    return ".obj, .gltf, .glb";
}

} // namespace Hollow
