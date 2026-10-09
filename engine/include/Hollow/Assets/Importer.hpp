// Model importer: loads a glTF/GLB or OBJ file into raw mesh parts (one per
// glTF primitive / OBJ material group). Positions keep the file's own units
// and pivot, so modular kits still line up; AssetManager wraps the result in
// shared MeshAssets. The glTF node hierarchy is flattened, with node
// transforms baked into the vertices of static meshes. Skinned glTF meshes
// keep their bind-space vertices plus joint influences, and the whole node
// hierarchy becomes a Skeleton carrying the file's animation clips. Morph
// targets are not imported.
#pragma once

#include "Hollow/Assets/Animation.hpp"
#include "Hollow/Assets/MeshAsset.hpp"

#include <memory>

#include <filesystem>
#include <string>
#include <vector>

namespace Hollow {

struct ImportedPart {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    Material material;
    // Skinning (empty for static parts); see MeshAsset.
    std::vector<SkinInfluence> skin;
    std::vector<int> skin_joints;
    std::vector<Mat4> inverse_bind;
};

struct ImportedModel {
    std::vector<ImportedPart> parts;
    std::shared_ptr<Skeleton> skeleton; // set when the file has skins or animations
};

// Throws std::runtime_error if the file can't be read or has no triangles.
// Recoverable per-primitive problems are logged as warnings and skipped.
ImportedModel import_model_file(const std::filesystem::path& path);
bool is_supported_model_file(const std::filesystem::path& path);
const char* supported_import_extensions();

} // namespace Hollow
