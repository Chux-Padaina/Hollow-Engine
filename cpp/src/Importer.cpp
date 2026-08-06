#include "Forge/Importer.hpp"
#include "Forge/Json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace Forge {
namespace {

struct GltfDocument {
    JsonValue json;
    std::vector<std::vector<unsigned char>> buffers;
    std::vector<std::string> image_paths;
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

Vec3 transform_point(const Mat4& matrix, Vec3 p) {
    return {
        p.x * matrix.m[0] + p.y * matrix.m[4] + p.z * matrix.m[8] + matrix.m[12],
        p.x * matrix.m[1] + p.y * matrix.m[5] + p.z * matrix.m[9] + matrix.m[13],
        p.x * matrix.m[2] + p.y * matrix.m[6] + p.z * matrix.m[10] + matrix.m[14],
    };
}

Vec3 transform_vector(const Mat4& matrix, Vec3 v) {
    return normalize({
        v.x * matrix.m[0] + v.y * matrix.m[4] + v.z * matrix.m[8],
        v.x * matrix.m[1] + v.y * matrix.m[5] + v.z * matrix.m[9],
        v.x * matrix.m[2] + v.y * matrix.m[6] + v.z * matrix.m[10],
    });
}

Vec3 safe_tangent_for_normal(Vec3 normal) {
    const Vec3 up = std::abs(normal.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    return normalize(cross(up, normal));
}

void generate_tangents(MeshPart& part) {
    std::vector<Vec3> tangents(part.vertices.size(), {});
    std::vector<Vec3> bitangents(part.vertices.size(), {});

    for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
        const std::uint32_t i0 = part.indices[i];
        const std::uint32_t i1 = part.indices[i + 1];
        const std::uint32_t i2 = part.indices[i + 2];
        if (i0 >= part.vertices.size() || i1 >= part.vertices.size() || i2 >= part.vertices.size()) {
            continue;
        }

        const Vertex& v0 = part.vertices[i0];
        const Vertex& v1 = part.vertices[i1];
        const Vertex& v2 = part.vertices[i2];
        const Vec3 edge1 = v1.position - v0.position;
        const Vec3 edge2 = v2.position - v0.position;
        const Vec2 duv1{v1.uv.x - v0.uv.x, v1.uv.y - v0.uv.y};
        const Vec2 duv2{v2.uv.x - v0.uv.x, v2.uv.y - v0.uv.y};
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) <= 0.000001f) {
            continue;
        }

        const float r = 1.0f / determinant;
        const Vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * r;
        const Vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;
        tangents[i0] += tangent;
        tangents[i1] += tangent;
        tangents[i2] += tangent;
        bitangents[i0] += bitangent;
        bitangents[i1] += bitangent;
        bitangents[i2] += bitangent;
    }

    for (std::size_t i = 0; i < part.vertices.size(); ++i) {
        Vertex& vertex = part.vertices[i];
        const Vec3 normal = normalize(vertex.normal);
        Vec3 tangent = tangents[i];
        tangent = normalize(tangent - normal * dot(normal, tangent));
        if (length(tangent) <= 0.0001f) {
            tangent = safe_tangent_for_normal(normal);
        }
        const float sign = dot(cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = tangent;
        vertex.tangent_sign = sign;
    }
}

Mat4 gltf_node_transform(const JsonValue& node) {
    if (const JsonValue* matrix = node.find("matrix"); matrix != nullptr && matrix->is_array() && matrix->array_value.size() == 16) {
        Mat4 out;
        for (std::size_t i = 0; i < 16; ++i) {
            out.m[i] = matrix->at(i).as_float();
        }
        return out;
    }

    Transform transform;
    if (const JsonValue* translation = node.find("translation"); translation != nullptr && translation->is_array() && translation->array_value.size() >= 3) {
        transform.translation = {translation->at(0).as_float(), translation->at(1).as_float(), translation->at(2).as_float()};
    }
    if (const JsonValue* scale = node.find("scale"); scale != nullptr && scale->is_array() && scale->array_value.size() >= 3) {
        transform.scale = {scale->at(0).as_float(1.0f), scale->at(1).as_float(1.0f), scale->at(2).as_float(1.0f)};
    }
    if (const JsonValue* rotation = node.find("rotation"); rotation != nullptr && rotation->is_array() && rotation->array_value.size() >= 4) {
        const float x = rotation->at(0).as_float();
        const float y = rotation->at(1).as_float();
        const float z = rotation->at(2).as_float();
        const float w = rotation->at(3).as_float(1.0f);
        const float sinr_cosp = 2.0f * (w * x + y * z);
        const float cosr_cosp = 1.0f - 2.0f * (x * x + y * y);
        transform.rotation.x = std::atan2(sinr_cosp, cosr_cosp);
        const float sinp = 2.0f * (w * y - z * x);
        transform.rotation.y = std::abs(sinp) >= 1.0f ? std::copysign(std::numbers::pi_v<float> / 2.0f, sinp) : std::asin(sinp);
        const float siny_cosp = 2.0f * (w * z + x * y);
        const float cosy_cosp = 1.0f - 2.0f * (y * y + z * z);
        transform.rotation.z = std::atan2(siny_cosp, cosy_cosp);
    }
    return transform.matrix();
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

std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open binary file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open text file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
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
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<unsigned char> decode_data_uri(const std::string& uri) {
    const std::size_t comma = uri.find(',');
    if (comma == std::string::npos || uri.find(";base64") == std::string::npos) {
        throw std::runtime_error("only base64 data URIs are supported");
    }

    std::vector<unsigned char> out;
    int value = 0;
    int bits = -8;
    for (char c : uri.substr(comma + 1)) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == '=') {
            continue;
        }
        const int decoded = base64_value(c);
        if (decoded < 0) {
            continue;
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
            bits -= 8;
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

std::filesystem::path extracted_image_path(const std::filesystem::path& model_path, int index, const std::string& mime_type) {
    std::string ext = ".img";
    if (mime_type == "image/png") {
        ext = ".png";
    } else if (mime_type == "image/jpeg") {
        ext = ".jpg";
    } else if (mime_type == "image/webp") {
        ext = ".webp";
    }
    return std::filesystem::temp_directory_path() / ("forge_gltf_" + model_path.stem().string() + "_" + std::to_string(index) + ext);
}

AttributeReader accessor_reader(const GltfDocument& document, int accessor_index) {
    const JsonValue& accessors = document.json.at("accessors");
    if (!accessors.is_array() || accessor_index < 0 || accessor_index >= static_cast<int>(accessors.array_value.size())) {
        throw std::runtime_error("glTF accessor index out of range");
    }
    const JsonValue& accessor = accessors.at(static_cast<std::size_t>(accessor_index));
    const int buffer_view_index = accessor.at("bufferView").as_int(-1);
    if (buffer_view_index < 0) {
        throw std::runtime_error("glTF accessor has no bufferView");
    }

    const JsonValue& buffer_views = document.json.at("bufferViews");
    const JsonValue& buffer_view = buffer_views.at(static_cast<std::size_t>(buffer_view_index));
    const int buffer_index = buffer_view.at("buffer").as_int();
    if (buffer_index < 0 || buffer_index >= static_cast<int>(document.buffers.size())) {
        throw std::runtime_error("glTF buffer index out of range");
    }

    const int component_type = accessor.at("componentType").as_int();
    const int component_count = type_component_count(accessor.at("type").as_string());
    const int byte_offset = buffer_view.at("byteOffset").as_int() + accessor.at("byteOffset").as_int();
    const int packed_stride = component_size(component_type) * component_count;
    const int stride = buffer_view.at("byteStride").as_int(packed_stride);
    const int count = accessor.at("count").as_int();
    const auto& buffer = document.buffers[static_cast<std::size_t>(buffer_index)];
    if (byte_offset < 0 || byte_offset + stride * std::max(0, count - 1) + packed_stride > static_cast<int>(buffer.size())) {
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
    if (reader.data == nullptr || index < 0 || index >= reader.count || reader.component_count < 3) {
        return fallback;
    }
    const unsigned char* data = reader.data + index * reader.stride;
    const int size = component_size(reader.component_type);
    return {
        read_component(data, reader.component_type, reader.normalized),
        read_component(data + size, reader.component_type, reader.normalized),
        read_component(data + size * 2, reader.component_type, reader.normalized),
    };
}

Vec2 read_vec2(const AttributeReader& reader, int index, Vec2 fallback = {}) {
    if (reader.data == nullptr || index < 0 || index >= reader.count || reader.component_count < 2) {
        return fallback;
    }
    const unsigned char* data = reader.data + index * reader.stride;
    const int size = component_size(reader.component_type);
    return {
        read_component(data, reader.component_type, reader.normalized),
        read_component(data + size, reader.component_type, reader.normalized),
    };
}

std::vector<std::uint32_t> read_indices(const GltfDocument& document, const JsonValue& primitive, int vertex_count) {
    if (const JsonValue* indices = primitive.find("indices")) {
        const AttributeReader reader = accessor_reader(document, indices->as_int());
        std::vector<std::uint32_t> out;
        out.reserve(static_cast<std::size_t>(reader.count));
        const int size = component_size(reader.component_type);
        for (int i = 0; i < reader.count; ++i) {
            out.push_back(read_index_component(reader.data + i * reader.stride, reader.component_type));
        }
        (void)size;
        return out;
    }

    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>(vertex_count));
    for (int i = 0; i < vertex_count; ++i) {
        out.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
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
    const JsonValue& materials = document.json.at("materials");
    if (!materials.is_array() || material_index < 0 || material_index >= static_cast<int>(materials.array_value.size())) {
        return material;
    }

    const JsonValue& material_json = materials.at(static_cast<std::size_t>(material_index));
    const JsonValue& pbr = material_json.at("pbrMetallicRoughness");
    if (pbr.is_object()) {
        const JsonValue& factor = pbr.at("baseColorFactor");
        if (factor.is_array() && factor.array_value.size() >= 3) {
            material.base_color = {factor.at(0).as_float(1.0f), factor.at(1).as_float(1.0f), factor.at(2).as_float(1.0f)};
        }
        material.metallic = std::clamp(pbr.at("metallicFactor").as_float(1.0f), 0.0f, 1.0f);
        material.roughness = std::clamp(pbr.at("roughnessFactor").as_float(1.0f), 0.04f, 1.0f);
        material.base_color_texture_path = texture_image_path(document, pbr.at("baseColorTexture"));
        material.metallic_roughness_texture_path = texture_image_path(document, pbr.at("metallicRoughnessTexture"));
    }
    const JsonValue& normal_texture = material_json.at("normalTexture");
    material.normal_texture_path = texture_image_path(document, normal_texture);
    material.normal_strength = normal_texture.at("scale").as_float(1.0f);
    material.emissive_texture_path = texture_image_path(document, material_json.at("emissiveTexture"));
    return material;
}

MeshPart build_gltf_primitive(const GltfDocument& document, const JsonValue& mesh_json, const JsonValue& primitive, int primitive_index, const Mat4& transform) {
    if (primitive.at("mode").as_int(4) != 4) {
        throw std::runtime_error("only glTF triangle primitives are supported");
    }

    const JsonValue& attributes = primitive.at("attributes");
    const int position_accessor = attributes.at("POSITION").as_int(-1);
    if (position_accessor < 0) {
        throw std::runtime_error("glTF primitive has no POSITION attribute");
    }

    const AttributeReader positions = accessor_reader(document, position_accessor);
    const int normal_accessor = attributes.at("NORMAL").as_int(-1);
    const int uv_accessor = attributes.at("TEXCOORD_0").as_int(-1);
    const int color_accessor = attributes.at("COLOR_0").as_int(-1);
    const AttributeReader normals = normal_accessor >= 0 ? accessor_reader(document, normal_accessor) : AttributeReader{};
    const AttributeReader uvs = uv_accessor >= 0 ? accessor_reader(document, uv_accessor) : AttributeReader{};
    const AttributeReader colors = color_accessor >= 0 ? accessor_reader(document, color_accessor) : AttributeReader{};

    MeshPart part;
    part.name = mesh_json.at("name").as_string("glTF mesh") + ":" + std::to_string(primitive_index);
    part.primitive = MeshPrimitive::Custom;
    part.material = read_gltf_material(document, primitive.at("material").as_int(-1));
    part.vertices.reserve(static_cast<std::size_t>(positions.count));

    for (int i = 0; i < positions.count; ++i) {
        Vertex vertex;
        vertex.position = transform_point(transform, read_vec3(positions, i));
        vertex.normal = transform_vector(transform, read_vec3(normals, i, {0.0f, 1.0f, 0.0f}));
        vertex.uv = read_vec2(uvs, i);
        vertex.color = read_vec3(colors, i, {1.0f, 1.0f, 1.0f});
        part.vertices.push_back(vertex);
    }

    part.indices = read_indices(document, primitive, positions.count);
    generate_tangents(part);
    float radius = 0.001f;
    for (const Vertex& vertex : part.vertices) {
        radius = std::max(radius, length(vertex.position));
    }
    part.bounds_radius = radius;
    return part;
}

void collect_gltf_node(const GltfDocument& document, int node_index, const Mat4& parent_transform, std::vector<MeshPart>& parts) {
    const JsonValue& nodes = document.json.at("nodes");
    if (!nodes.is_array() || node_index < 0 || node_index >= static_cast<int>(nodes.array_value.size())) {
        return;
    }

    const JsonValue& node = nodes.at(static_cast<std::size_t>(node_index));
    const Mat4 world = parent_transform * gltf_node_transform(node);
    const int mesh_index = node.at("mesh").as_int(-1);
    const JsonValue& meshes = document.json.at("meshes");
    if (mesh_index >= 0 && meshes.is_array() && mesh_index < static_cast<int>(meshes.array_value.size())) {
        const JsonValue& mesh_json = meshes.at(static_cast<std::size_t>(mesh_index));
        const JsonValue& primitives = mesh_json.at("primitives");
        if (primitives.is_array()) {
            for (std::size_t i = 0; i < primitives.array_value.size(); ++i) {
                try {
                    parts.push_back(build_gltf_primitive(document, mesh_json, primitives.at(i), static_cast<int>(i), world));
                } catch (const std::exception&) {
                }
            }
        }
    }

    const JsonValue& children = node.at("children");
    if (children.is_array()) {
        for (const JsonValue& child : children.array_value) {
            collect_gltf_node(document, child.as_int(-1), world, parts);
        }
    }
}

std::vector<int> gltf_scene_roots(const GltfDocument& document) {
    std::vector<int> roots;
    const JsonValue& scenes = document.json.at("scenes");
    int scene_index = document.json.at("scene").as_int(0);
    if (!scenes.is_array() || scenes.array_value.empty()) {
        return roots;
    }
    scene_index = std::clamp(scene_index, 0, static_cast<int>(scenes.array_value.size()) - 1);
    const JsonValue& nodes = scenes.at(static_cast<std::size_t>(scene_index)).at("nodes");
    if (nodes.is_array()) {
        for (const JsonValue& node : nodes.array_value) {
            roots.push_back(node.as_int(-1));
        }
    }
    return roots;
}

void resolve_gltf_buffers_and_images(GltfDocument& document, const std::filesystem::path& path, const std::vector<unsigned char>& glb_bin_chunk = {}) {
    const std::filesystem::path base_dir = path.parent_path();
    const JsonValue& buffers = document.json.at("buffers");
    if (buffers.is_array()) {
        for (std::size_t i = 0; i < buffers.array_value.size(); ++i) {
            const JsonValue& buffer = buffers.at(i);
            const std::string uri = buffer.at("uri").as_string();
            if (uri.empty()) {
                if (!glb_bin_chunk.empty()) {
                    document.buffers.push_back(glb_bin_chunk);
                }
            } else if (uri.rfind("data:", 0) == 0) {
                document.buffers.push_back(decode_data_uri(uri));
            } else {
                document.buffers.push_back(read_binary_file(base_dir / std::filesystem::path(decode_uri_path(uri))));
            }
        }
    }

    const JsonValue& images = document.json.at("images");
    if (!images.is_array()) {
        return;
    }
    document.image_paths.resize(images.array_value.size());
    for (std::size_t i = 0; i < images.array_value.size(); ++i) {
        const JsonValue& image = images.at(i);
        const std::string uri = image.at("uri").as_string();
        if (!uri.empty()) {
            if (uri.rfind("data:", 0) == 0) {
                const std::vector<unsigned char> bytes = decode_data_uri(uri);
                const std::filesystem::path extracted = extracted_image_path(path, static_cast<int>(i), uri.find("image/jpeg") != std::string::npos ? "image/jpeg" : "image/png");
                std::ofstream out(extracted, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                document.image_paths[i] = extracted.string();
            } else {
                document.image_paths[i] = (base_dir / std::filesystem::path(decode_uri_path(uri))).lexically_normal().string();
            }
            continue;
        }

        const int buffer_view_index = image.at("bufferView").as_int(-1);
        if (buffer_view_index < 0) {
            continue;
        }
        const JsonValue& buffer_view = document.json.at("bufferViews").at(static_cast<std::size_t>(buffer_view_index));
        const int buffer_index = buffer_view.at("buffer").as_int();
        if (buffer_index < 0 || buffer_index >= static_cast<int>(document.buffers.size())) {
            continue;
        }
        const int byte_offset = buffer_view.at("byteOffset").as_int();
        const int byte_length = buffer_view.at("byteLength").as_int();
        const auto& buffer = document.buffers[static_cast<std::size_t>(buffer_index)];
        if (byte_offset < 0 || byte_length <= 0 || byte_offset + byte_length > static_cast<int>(buffer.size())) {
            continue;
        }

        const std::filesystem::path extracted = extracted_image_path(path, static_cast<int>(i), image.at("mimeType").as_string("image/png"));
        std::ofstream out(extracted, std::ios::binary);
        out.write(reinterpret_cast<const char*>(buffer.data() + byte_offset), byte_length);
        document.image_paths[i] = extracted.string();
    }
}

GltfDocument load_gltf_document(const std::filesystem::path& path) {
    GltfDocument document;
    const std::string ext = path.extension().string();
    if (ext == ".glb" || ext == ".GLB") {
        const std::vector<unsigned char> bytes = read_binary_file(path);
        if (bytes.size() < 20 || read_u32_le(bytes, 0) != 0x46546C67 || read_u32_le(bytes, 4) != 2) {
            throw std::runtime_error("invalid or unsupported GLB file");
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
    for (int root : gltf_scene_roots(document)) {
        collect_gltf_node(document, root, Mat4::identity(), imported.parts);
    }
    if (imported.parts.empty()) {
        throw std::runtime_error("no triangle meshes found in glTF file");
    }
    normalize_parts(imported.parts);
    return imported;
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
        generate_tangents(part);
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
    if (ext == ".gltf" || ext == ".GLTF" || ext == ".glb" || ext == ".GLB") {
        return load_gltf(path);
    }
    throw std::runtime_error("unsupported import format; use .obj, .gltf, or .glb");
}

std::string supported_import_extensions() {
    return ".obj, .gltf, .glb";
}

} // namespace Forge
