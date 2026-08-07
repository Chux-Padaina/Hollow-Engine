// Loads a PNG/JPEG (or any WIC-supported) image file into a CPU-side RGBA8
// pixel buffer ready for GPU upload. Windows-only: implemented on top of
// the Windows Imaging Component (WIC). Used when importing textured
// assets, e.g. by the glTF importer.

#pragma once

#include <filesystem>
#include <vector>

namespace Hollow {

struct TextureImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

TextureImage load_texture_image(const std::filesystem::path& path);

} // namespace Hollow
