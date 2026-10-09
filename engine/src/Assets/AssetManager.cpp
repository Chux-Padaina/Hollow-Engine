// Implementation of AssetManager: built-in primitive generation and the
// per-path model cache.
#include "Hollow/Assets/AssetManager.hpp"

#include "Hollow/Assets/Importer.hpp"
#include "Hollow/Core/FileUtil.hpp"
#include "Hollow/Core/Log.hpp"

#include <format>
#include <stdexcept>

namespace Hollow {
namespace {

std::string cache_key(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        absolute = std::filesystem::absolute(path, ec);
    }
    std::string key = to_utf8(absolute.lexically_normal());
    // Windows paths are case-insensitive.
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

MeshAsset create_cube() {
    MeshAsset mesh;
    mesh.name = "Cube";
    mesh.source = "builtin:cube";
    mesh.radial_outline = true;
    struct Face {
        Vec3 normal;
        Vec3 corners[4];
    };
    const Face faces[6] = {
        {{0, 0, 1}, {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}}},
        {{0, 0, -1}, {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}}},
        {{-1, 0, 0}, {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}}},
        {{1, 0, 0}, {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}},
        {{0, 1, 0}, {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}}},
        {{0, -1, 0}, {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}}},
    };
    const Vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int i = 0; i < 4; ++i) {
            Vertex vertex;
            vertex.position = face.corners[i];
            vertex.normal = face.normal;
            vertex.uv = uvs[i];
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    generate_tangents(mesh.vertices, mesh.indices);
    return mesh;
}

MeshAsset create_plane() {
    MeshAsset mesh;
    mesh.name = "Plane";
    mesh.source = "builtin:plane";
    const Vec3 corners[4] = {{-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f}};
    const Vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i) {
        Vertex vertex;
        vertex.position = corners[i];
        vertex.normal = {0.0f, 1.0f, 0.0f};
        vertex.uv = uvs[i];
        vertex.tangent = {1.0f, 0.0f, 0.0f};
        mesh.vertices.push_back(vertex);
    }
    // Counter-clockwise seen from +Y.
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

MeshAsset create_sphere() {
    MeshAsset mesh;
    mesh.name = "Sphere";
    mesh.source = "builtin:sphere";
    mesh.radial_outline = true;
    constexpr int rings = 24;
    constexpr int segments = 40;
    for (int y = 0; y <= rings; ++y) {
        const float v = static_cast<float>(y) / rings;
        const float phi = (v - 0.5f) * kPi;
        for (int x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / segments;
            const float theta = u * kPi * 2.0f;
            Vertex vertex;
            vertex.normal = {std::cos(phi) * std::cos(theta), std::sin(phi), std::cos(phi) * std::sin(theta)};
            vertex.position = vertex.normal * 0.5f;
            vertex.uv = {u, 1.0f - v};
            vertex.tangent = normalize(Vec3{-std::sin(theta), 0.0f, std::cos(theta)});
            mesh.vertices.push_back(vertex);
        }
    }
    for (int y = 0; y < rings; ++y) {
        for (int x = 0; x < segments; ++x) {
            const auto a = static_cast<std::uint32_t>(y * (segments + 1) + x);
            const std::uint32_t b = a + 1;
            const auto c = static_cast<std::uint32_t>((y + 1) * (segments + 1) + x);
            const std::uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

} // namespace

const char* builtin_mesh_name(BuiltinMesh mesh) {
    switch (mesh) {
    case BuiltinMesh::Cube:
        return "cube";
    case BuiltinMesh::Plane:
        return "plane";
    case BuiltinMesh::Sphere:
        return "sphere";
    }
    return "cube";
}

MeshHandle AssetManager::builtin_mesh(BuiltinMesh mesh) {
    const int key = static_cast<int>(mesh);
    auto it = builtins_.find(key);
    if (it != builtins_.end()) {
        return it->second;
    }
    MeshAsset asset;
    switch (mesh) {
    case BuiltinMesh::Cube:
        asset = create_cube();
        break;
    case BuiltinMesh::Plane:
        asset = create_plane();
        break;
    case BuiltinMesh::Sphere:
        asset = create_sphere();
        break;
    }
    MeshHandle handle = make_mesh_asset(std::move(asset));
    builtins_[key] = handle;
    return handle;
}

MeshHandle AssetManager::builtin_mesh(const std::string& name) {
    for (BuiltinMesh mesh : {BuiltinMesh::Cube, BuiltinMesh::Plane, BuiltinMesh::Sphere}) {
        if (name == builtin_mesh_name(mesh)) {
            return builtin_mesh(mesh);
        }
    }
    return nullptr;
}

ModelHandle AssetManager::load_model(const std::filesystem::path& path) {
    const std::string key = cache_key(path);
    auto it = models_.find(key);
    if (it != models_.end()) {
        return it->second;
    }

    // Import through the absolute path so texture paths recorded in the
    // materials don't depend on the working directory.
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec).lexically_normal();
    ImportedModel imported = import_model_file(absolute);
    auto model = std::make_shared<ModelAsset>();
    model->path = to_utf8(absolute);
    model->bounds = Aabb::empty();
    for (std::size_t i = 0; i < imported.parts.size(); ++i) {
        ImportedPart& part = imported.parts[i];
        MeshAsset asset;
        asset.name = part.name;
        asset.source = "model:" + model->path + "#" + std::to_string(i);
        asset.vertices = std::move(part.vertices);
        asset.indices = std::move(part.indices);
        if (!part.skin.empty() && imported.skeleton) {
            asset.skin = std::move(part.skin);
            asset.skin_joints = std::move(part.skin_joints);
            asset.inverse_bind = std::move(part.inverse_bind);
            asset.skeleton = imported.skeleton;
        }
        MeshHandle mesh = make_mesh_asset(std::move(asset));
        model->bounds.expand(mesh->bounds);
        model->parts.push_back({part.name, mesh, part.material});
    }
    if (!model->bounds.valid()) {
        model->bounds = {};
    }
    log_info(std::format("Imported '{}' ({} part(s))", to_utf8(path.filename()), model->parts.size()));
    models_[key] = model;
    return model;
}

MeshHandle AssetManager::model_mesh(const std::filesystem::path& path, int index) {
    try {
        ModelHandle model = load_model(path);
        if (index < 0 || index >= static_cast<int>(model->parts.size())) {
            log_warning(std::format("Model '{}' has no part {}", to_utf8(path), index));
            return nullptr;
        }
        return model->parts[static_cast<std::size_t>(index)].mesh;
    } catch (const std::exception& error) {
        log_warning(std::format("Could not load model '{}': {}", to_utf8(path), error.what()));
        return nullptr;
    }
}

void AssetManager::forget_model(const std::filesystem::path& path) {
    models_.erase(cache_key(path));
}

bool parse_model_source(const std::string& source, std::string& path_utf8, int& part) {
    constexpr std::string_view prefix = "model:";
    if (source.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::size_t hash = source.rfind('#');
    if (hash == std::string::npos || hash <= prefix.size()) {
        return false;
    }
    path_utf8 = source.substr(prefix.size(), hash - prefix.size());
    try {
        part = std::stoi(source.substr(hash + 1));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

} // namespace Hollow
