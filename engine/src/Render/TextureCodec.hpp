// CPU texture processing for the renderer's texture manager: mip chain
// generation, block compression (BC1/BC3/BC4/BC5) and readers for
// pre-compressed .dds / .ktx2 files and the engine's own on-disk cache
// format (.htex).
#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Hollow::texcodec {

enum class Usage {
    Color,  // sRGB color (+alpha)
    Linear, // linear RGB(A) data, e.g. metallic/roughness
    Normal, // tangent-space normal (only XY kept when compressed)
    Single, // one channel (occlusion, height, transmission)
    Dual,   // two channels (clearcoat factor/roughness)
};

struct MipLevel {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> data;
};

// A texture ready to upload: either uncompressed RGBA8 or a compressed
// GL format, with every mip level.
struct TextureData {
    GLenum internal_format = GL_RGBA8; // sized GL format
    bool compressed = false;
    std::vector<MipLevel> mips;
    std::size_t bytes() const;
};

// RGBA8 image -> full mip chain (box filter, in linear space for sRGB).
TextureData build_mips(int width, int height, std::vector<std::uint8_t> rgba, Usage usage);
// Compresses every level (BC1/BC3 color, BC4 single, BC5 normal/dual).
TextureData compress(const TextureData& source, Usage usage);
GLenum compressed_format_for(Usage usage, bool has_alpha);

// .dds (DX10 or legacy DXT1/DXT5/ATI1/ATI2/BC7 headers) and .ktx2
// (uncompressed RGBA8 or BCn, no supercompression). Throws on
// unsupported content.
TextureData load_dds(const std::filesystem::path& path, bool srgb);
TextureData load_ktx2(const std::filesystem::path& path, bool srgb);

// Engine cache (.htex): a header (source size + timestamp) plus mips.
bool read_cache(const std::filesystem::path& path, std::uint64_t source_size, std::int64_t source_time, TextureData& out);
void write_cache(const std::filesystem::path& path, std::uint64_t source_size, std::int64_t source_time, const TextureData& data);

} // namespace Hollow::texcodec
