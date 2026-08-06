#pragma once

#include <filesystem>
#include <vector>

namespace Forge {

struct TextureImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

TextureImage load_texture_image(const std::filesystem::path& path);

} // namespace Forge
