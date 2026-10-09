// Central owner of loaded assets. Models are imported once per file path
// and cached, so importing (or undo-restoring, or loading a scene with) the
// same .glb twice shares the same MeshAssets and therefore the same GPU
// buffers. Built-in primitives (cube/plane/sphere) are also MeshAssets, so
// the renderer has a single code path for every mesh.
#pragma once

#include "Hollow/Assets/MeshAsset.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hollow {

enum class BuiltinMesh {
    Cube,
    Plane,
    Sphere,
};

const char* builtin_mesh_name(BuiltinMesh mesh);

struct ModelPart {
    std::string name;
    MeshHandle mesh;
    Material material; // default material for the part, as imported
};

struct ModelAsset {
    std::string path; // UTF-8, absolute
    std::vector<ModelPart> parts;
    Aabb bounds;
};

using ModelHandle = std::shared_ptr<const ModelAsset>;

class AssetManager {
public:
    MeshHandle builtin_mesh(BuiltinMesh mesh);
    // nullptr for an unknown name.
    MeshHandle builtin_mesh(const std::string& name);

    // Imports (or returns the cached) model at `path`. Throws
    // std::runtime_error if the file can't be imported.
    ModelHandle load_model(const std::filesystem::path& path);
    // Mesh for part `index` of the model at `path`; nullptr (and a logged
    // warning) if the model fails to load or has no such part.
    MeshHandle model_mesh(const std::filesystem::path& path, int index);

    // Re-import a model from disk on next load (e.g. after it changed).
    void forget_model(const std::filesystem::path& path);
    std::size_t model_count() const { return models_.size(); }

private:
    std::unordered_map<int, MeshHandle> builtins_;
    std::unordered_map<std::string, ModelHandle> models_;
};

// Parses a MeshAsset::source string back into its parts. Returns false for
// a malformed source.
bool parse_model_source(const std::string& source, std::string& path_utf8, int& part);

} // namespace Hollow
