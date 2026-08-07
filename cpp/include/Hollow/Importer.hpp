// Model importer: loads a glTF/GLB or OBJ file from disk and converts it
// into MeshPart data (defined in Scene.hpp) that the editor can hand to
// Scene::create_mesh_actor() to add the imported model as a scene actor.

#pragma once

#include "Hollow/Scene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace Hollow {

struct ImportedModel {
    std::vector<MeshPart> parts;
};

ImportedModel import_model_file(const std::filesystem::path& path);
std::string supported_import_extensions();

} // namespace Hollow
