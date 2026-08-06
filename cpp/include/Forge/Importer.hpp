#pragma once

#include "Forge/Scene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace Forge {

struct ImportedModel {
    std::vector<MeshPart> parts;
};

ImportedModel import_model_file(const std::filesystem::path& path);
std::string supported_import_extensions();

} // namespace Forge
