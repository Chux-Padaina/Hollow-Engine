// Implementation of TextureCodec.hpp.
#include "Render/TextureCodec.hpp"

#include "Hollow/Core/FileUtil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace Hollow::texcodec {

std::size_t TextureData::bytes() const {
    std::size_t total = 0;
    for (const MipLevel& mip : mips) {
        total += mip.data.size();
    }
    return total;
}

// --- Mips --------------------------------------------------------------------

namespace {

std::array<float, 256> make_srgb_table() {
    std::array<float, 256> table{};
    for (int i = 0; i < 256; ++i) {
        const float c = static_cast<float>(i) / 255.0f;
        table[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return table;
}

const std::array<float, 256>& srgb_table() {
    static const std::array<float, 256> table = make_srgb_table();
    return table;
}

std::uint8_t linear_to_srgb8(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
}

std::uint8_t to8(float c) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
}

MipLevel downsample(const MipLevel& src, Usage usage) {
    MipLevel dst;
    dst.width = std::max(1, src.width / 2);
    dst.height = std::max(1, src.height / 2);
    dst.data.resize(static_cast<std::size_t>(dst.width) * static_cast<std::size_t>(dst.height) * 4);
    const auto& table = srgb_table();
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            float sum[4] = {0, 0, 0, 0};
            int count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, src.width - 1);
                    const int sy = std::min(y * 2 + dy, src.height - 1);
                    const std::uint8_t* p = &src.data[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(src.width) + static_cast<std::size_t>(sx)) * 4];
                    for (int c = 0; c < 4; ++c) {
                        float value;
                        if (usage == Usage::Color && c < 3) {
                            value = table[p[c]];
                        } else if (usage == Usage::Normal && c < 3) {
                            value = static_cast<float>(p[c]) / 127.5f - 1.0f;
                        } else {
                            value = static_cast<float>(p[c]) / 255.0f;
                        }
                        sum[c] += value;
                    }
                    ++count;
                }
            }
            std::uint8_t* out = &dst.data[(static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.width) + static_cast<std::size_t>(x)) * 4];
            const float inv = 1.0f / static_cast<float>(count);
            if (usage == Usage::Normal) {
                float n[3] = {sum[0] * inv, sum[1] * inv, sum[2] * inv};
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int c = 0; c < 3; ++c) {
                    const float v = len > 1e-6f ? n[c] / len : (c == 2 ? 1.0f : 0.0f);
                    out[c] = to8(v * 0.5f + 0.5f);
                }
                out[3] = to8(sum[3] * inv);
            } else {
                for (int c = 0; c < 4; ++c) {
                    out[c] = usage == Usage::Color && c < 3 ? linear_to_srgb8(sum[c] * inv) : to8(sum[c] * inv);
                }
            }
        }
    }
    return dst;
}

} // namespace

TextureData build_mips(int width, int height, std::vector<std::uint8_t> rgba, Usage usage) {
    TextureData data;
    data.internal_format = usage == Usage::Color ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    MipLevel base;
    base.width = width;
    base.height = height;
    base.data = std::move(rgba);
    data.mips.push_back(std::move(base));
    while (data.mips.back().width > 1 || data.mips.back().height > 1) {
        data.mips.push_back(downsample(data.mips.back(), usage));
    }
    return data;
}

// --- Block compression ----------------------------------------------------------

namespace {

std::uint16_t pack565(const float c[3]) {
    const int r = std::clamp(static_cast<int>(std::lround(c[0] * 31.0f / 255.0f)), 0, 31);
    const int g = std::clamp(static_cast<int>(std::lround(c[1] * 63.0f / 255.0f)), 0, 63);
    const int b = std::clamp(static_cast<int>(std::lround(c[2] * 31.0f / 255.0f)), 0, 31);
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

void unpack565(std::uint16_t v, float out[3]) {
    const int r = (v >> 11) & 31;
    const int g = (v >> 5) & 63;
    const int b = v & 31;
    out[0] = static_cast<float>((r << 3) | (r >> 2));
    out[1] = static_cast<float>((g << 2) | (g >> 4));
    out[2] = static_cast<float>((b << 3) | (b >> 2));
}

// 16 RGB pixels -> 8-byte BC1 block (always 4-color mode).
void encode_bc1(const float pixels[16][3], std::uint8_t* out) {
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            mean[c] += pixels[i][c] / 16.0f;
        }
    }
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        const float r = pixels[i][0] - mean[0], g = pixels[i][1] - mean[1], b = pixels[i][2] - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b;
        cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    float axis[3] = {1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 6; ++it) {
        const float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        const float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        const float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-6f) {
            break;
        }
        axis[0] = x / len;
        axis[1] = y / len;
        axis[2] = z / len;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; ++i) {
        const float t = (pixels[i][0] - mean[0]) * axis[0] + (pixels[i][1] - mean[1]) * axis[1] + (pixels[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    // Inset the endpoints slightly (reduces error at the extremes).
    const float inset = (hi - lo) / 32.0f;
    lo += inset;
    hi -= inset;
    float e0[3], e1[3];
    for (int c = 0; c < 3; ++c) {
        e0[c] = std::clamp(mean[c] + axis[c] * hi, 0.0f, 255.0f);
        e1[c] = std::clamp(mean[c] + axis[c] * lo, 0.0f, 255.0f);
    }
    std::uint16_t c0 = pack565(e0);
    std::uint16_t c1 = pack565(e1);
    if (c0 < c1) {
        std::swap(c0, c1);
    }
    std::uint32_t indices = 0;
    if (c0 != c1) {
        float palette[4][3];
        unpack565(c0, palette[0]);
        unpack565(c1, palette[1]);
        for (int c = 0; c < 3; ++c) {
            palette[2][c] = (2.0f * palette[0][c] + palette[1][c]) / 3.0f;
            palette[3][c] = (palette[0][c] + 2.0f * palette[1][c]) / 3.0f;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 0;
            float best_d = 1e30f;
            for (int p = 0; p < 4; ++p) {
                const float dr = pixels[i][0] - palette[p][0];
                const float dg = pixels[i][1] - palette[p][1];
                const float db = pixels[i][2] - palette[p][2];
                const float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
                if (d < best_d) {
                    best_d = d;
                    best = p;
                }
            }
            indices |= static_cast<std::uint32_t>(best) << (i * 2);
        }
    }
    out[0] = static_cast<std::uint8_t>(c0 & 0xFF);
    out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1 & 0xFF);
    out[3] = static_cast<std::uint8_t>(c1 >> 8);
    std::memcpy(out + 4, &indices, 4);
}

// 16 values -> 8-byte BC4 block (8-value mode).
void encode_bc4(const float values[16], std::uint8_t* out) {
    float lo = 255.0f, hi = 0.0f;
    for (int i = 0; i < 16; ++i) {
        lo = std::min(lo, values[i]);
        hi = std::max(hi, values[i]);
    }
    const int a0 = static_cast<int>(std::lround(hi));
    const int a1 = static_cast<int>(std::lround(lo));
    out[0] = static_cast<std::uint8_t>(a0);
    out[1] = static_cast<std::uint8_t>(a1);
    std::uint64_t bits = 0;
    if (a0 != a1) {
        float palette[8];
        palette[0] = static_cast<float>(a0);
        palette[1] = static_cast<float>(a1);
        for (int i = 1; i <= 6; ++i) {
            palette[i + 1] = (static_cast<float>(7 - i) * static_cast<float>(a0) + static_cast<float>(i) * static_cast<float>(a1)) / 7.0f;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 0;
            float best_d = 1e30f;
            for (int p = 0; p < 8; ++p) {
                const float d = std::abs(values[i] - palette[p]);
                if (d < best_d) {
                    best_d = d;
                    best = p;
                }
            }
            bits |= static_cast<std::uint64_t>(best) << (i * 3);
        }
    }
    for (int i = 0; i < 6; ++i) {
        out[2 + i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF);
    }
}

} // namespace

GLenum compressed_format_for(Usage usage, bool has_alpha) {
    switch (usage) {
    case Usage::Color: return has_alpha ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
    case Usage::Linear: return has_alpha ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    case Usage::Normal:
    case Usage::Dual: return GL_COMPRESSED_RG_RGTC2;
    case Usage::Single: return GL_COMPRESSED_RED_RGTC1;
    }
    return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
}

TextureData compress(const TextureData& source, Usage usage) {
    if (source.compressed || source.mips.empty()) {
        return source;
    }
    bool has_alpha = false;
    if (usage == Usage::Color || usage == Usage::Linear) {
        const auto& base = source.mips.front().data;
        for (std::size_t i = 3; i < base.size(); i += 4) {
            if (base[i] < 250) {
                has_alpha = true;
                break;
            }
        }
    }
    TextureData out;
    out.compressed = true;
    out.internal_format = compressed_format_for(usage, has_alpha);
    const bool bc1 = out.internal_format == GL_COMPRESSED_SRGB_S3TC_DXT1_EXT || out.internal_format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    const bool bc3 = out.internal_format == GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT || out.internal_format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    const bool bc5 = out.internal_format == GL_COMPRESSED_RG_RGTC2;
    const std::size_t block_bytes = (bc3 || bc5) ? 16 : 8;
    for (const MipLevel& level : source.mips) {
        MipLevel mip;
        mip.width = level.width;
        mip.height = level.height;
        const int bw = (level.width + 3) / 4;
        const int bh = (level.height + 3) / 4;
        mip.data.resize(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * block_bytes);
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                float rgb[16][3];
                float alpha[16];
                float red[16];
                float green[16];
                for (int i = 0; i < 16; ++i) {
                    const int x = std::min(bx * 4 + (i & 3), level.width - 1);
                    const int y = std::min(by * 4 + (i >> 2), level.height - 1);
                    const std::uint8_t* p = &level.data[(static_cast<std::size_t>(y) * static_cast<std::size_t>(level.width) + static_cast<std::size_t>(x)) * 4];
                    rgb[i][0] = p[0];
                    rgb[i][1] = p[1];
                    rgb[i][2] = p[2];
                    alpha[i] = p[3];
                    red[i] = p[0];
                    green[i] = p[1];
                }
                std::uint8_t* block = &mip.data[(static_cast<std::size_t>(by) * static_cast<std::size_t>(bw) + static_cast<std::size_t>(bx)) * block_bytes];
                if (bc1) {
                    encode_bc1(rgb, block);
                } else if (bc3) {
                    encode_bc4(alpha, block);
                    encode_bc1(rgb, block + 8);
                } else if (bc5) {
                    encode_bc4(red, block);
                    encode_bc4(green, block + 8);
                } else {
                    encode_bc4(red, block);
                }
            }
        }
        out.mips.push_back(std::move(mip));
    }
    return out;
}

// --- DDS / KTX2 -------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("can't open " + to_utf8(path));
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::uint32_t u32(const std::vector<std::uint8_t>& d, std::size_t o) {
    if (o + 4 > d.size()) {
        throw std::runtime_error("truncated texture file");
    }
    std::uint32_t v;
    std::memcpy(&v, &d[o], 4);
    return v;
}

std::uint64_t u64(const std::vector<std::uint8_t>& d, std::size_t o) {
    if (o + 8 > d.size()) {
        throw std::runtime_error("truncated texture file");
    }
    std::uint64_t v;
    std::memcpy(&v, &d[o], 8);
    return v;
}

std::size_t level_size(GLenum format, bool compressed, int w, int h) {
    if (!compressed) {
        return static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    }
    const bool small_blocks = format == GL_COMPRESSED_SRGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_RED_RGTC1;
    return static_cast<std::size_t>((w + 3) / 4) * static_cast<std::size_t>((h + 3) / 4) * (small_blocks ? 8u : 16u);
}

} // namespace

TextureData load_dds(const std::filesystem::path& path, bool srgb) {
    const std::vector<std::uint8_t> d = read_all(path);
    if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) {
        throw std::runtime_error("not a DDS file");
    }
    const int height = static_cast<int>(u32(d, 12));
    const int width = static_cast<int>(u32(d, 16));
    const int mip_count = std::max(1, static_cast<int>(u32(d, 28)));
    const std::uint32_t fourcc = u32(d, 84);
    auto cc = [](const char* s) { return static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) | (static_cast<std::uint32_t>(s[2]) << 16) | (static_cast<std::uint32_t>(s[3]) << 24); };
    TextureData out;
    out.compressed = true;
    std::size_t offset = 128;
    if (fourcc == cc("DXT1")) {
        out.internal_format = srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    } else if (fourcc == cc("DXT5")) {
        out.internal_format = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    } else if (fourcc == cc("ATI1") || fourcc == cc("BC4U")) {
        out.internal_format = GL_COMPRESSED_RED_RGTC1;
    } else if (fourcc == cc("ATI2") || fourcc == cc("BC5U")) {
        out.internal_format = GL_COMPRESSED_RG_RGTC2;
    } else if (fourcc == cc("DX10")) {
        const std::uint32_t dxgi = u32(d, 128);
        offset = 148;
        switch (dxgi) {
        case 71: out.internal_format = GL_COMPRESSED_RGB_S3TC_DXT1_EXT; break;
        case 72: out.internal_format = GL_COMPRESSED_SRGB_S3TC_DXT1_EXT; break;
        case 77: out.internal_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
        case 78: out.internal_format = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT; break;
        case 80: out.internal_format = GL_COMPRESSED_RED_RGTC1; break;
        case 83: out.internal_format = GL_COMPRESSED_RG_RGTC2; break;
        case 98: out.internal_format = srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM; break;
        case 99: out.internal_format = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM; break;
        case 28: out.internal_format = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8; out.compressed = false; break;
        case 29: out.internal_format = GL_SRGB8_ALPHA8; out.compressed = false; break;
        default: throw std::runtime_error("unsupported DXGI format in DDS: " + std::to_string(dxgi));
        }
    } else {
        throw std::runtime_error("unsupported DDS pixel format");
    }
    int w = width;
    int h = height;
    for (int level = 0; level < mip_count; ++level) {
        const std::size_t size = level_size(out.internal_format, out.compressed, w, h);
        if (offset + size > d.size()) {
            break;
        }
        MipLevel mip;
        mip.width = w;
        mip.height = h;
        mip.data.assign(d.begin() + static_cast<std::ptrdiff_t>(offset), d.begin() + static_cast<std::ptrdiff_t>(offset + size));
        out.mips.push_back(std::move(mip));
        offset += size;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    if (out.mips.empty()) {
        throw std::runtime_error("DDS has no image data");
    }
    return out;
}

TextureData load_ktx2(const std::filesystem::path& path, bool srgb) {
    const std::vector<std::uint8_t> d = read_all(path);
    static const std::uint8_t kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (d.size() < 80 || std::memcmp(d.data(), kId, 12) != 0) {
        throw std::runtime_error("not a KTX2 file");
    }
    const std::uint32_t vk_format = u32(d, 12);
    const int width = static_cast<int>(u32(d, 20));
    const int height = static_cast<int>(std::max(1u, u32(d, 24)));
    const std::uint32_t face_count = u32(d, 36);
    const int level_count = static_cast<int>(std::max(1u, u32(d, 40)));
    const std::uint32_t supercompression = u32(d, 44);
    if (supercompression != 0) {
        throw std::runtime_error("supercompressed KTX2 (Basis/Zstd) isn't supported");
    }
    if (face_count > 1) {
        throw std::runtime_error("KTX2 cubemaps aren't supported as material textures");
    }
    TextureData out;
    out.compressed = true;
    switch (vk_format) {
    case 131: case 133: out.internal_format = GL_COMPRESSED_RGB_S3TC_DXT1_EXT; break;
    case 132: case 134: out.internal_format = GL_COMPRESSED_SRGB_S3TC_DXT1_EXT; break;
    case 137: out.internal_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
    case 138: out.internal_format = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT; break;
    case 139: out.internal_format = GL_COMPRESSED_RED_RGTC1; break;
    case 141: out.internal_format = GL_COMPRESSED_RG_RGTC2; break;
    case 145: out.internal_format = srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM; break;
    case 146: out.internal_format = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM; break;
    case 37: out.internal_format = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8; out.compressed = false; break;
    case 43: out.internal_format = GL_SRGB8_ALPHA8; out.compressed = false; break;
    default: throw std::runtime_error("unsupported KTX2 vkFormat " + std::to_string(vk_format));
    }
    const std::size_t level_index = 80;
    for (int level = 0; level < level_count; ++level) {
        const std::size_t entry = level_index + static_cast<std::size_t>(level) * 24;
        const std::uint64_t offset = u64(d, entry);
        const std::uint64_t length = u64(d, entry + 8);
        if (offset + length > d.size()) {
            throw std::runtime_error("truncated KTX2 level");
        }
        MipLevel mip;
        mip.width = std::max(1, width >> level);
        mip.height = std::max(1, height >> level);
        mip.data.assign(d.begin() + static_cast<std::ptrdiff_t>(offset), d.begin() + static_cast<std::ptrdiff_t>(offset + length));
        out.mips.push_back(std::move(mip));
    }
    return out;
}

// --- Cache ----------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kCacheMagic = 0x58455448; // "HTEX"
constexpr std::uint32_t kCacheVersion = 2;
} // namespace

bool read_cache(const std::filesystem::path& path, std::uint64_t source_size, std::int64_t source_time, TextureData& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::uint32_t header[5];
    std::uint64_t size = 0;
    std::int64_t time = 0;
    file.read(reinterpret_cast<char*>(header), sizeof(std::uint32_t) * 2);
    file.read(reinterpret_cast<char*>(&size), 8);
    file.read(reinterpret_cast<char*>(&time), 8);
    file.read(reinterpret_cast<char*>(header + 2), sizeof(std::uint32_t) * 3);
    if (!file || header[0] != kCacheMagic || header[1] != kCacheVersion || size != source_size || time != source_time) {
        return false;
    }
    out.internal_format = header[2];
    out.compressed = header[3] != 0;
    out.mips.clear();
    for (std::uint32_t i = 0; i < header[4] && i < 20; ++i) {
        std::uint32_t dims[3];
        file.read(reinterpret_cast<char*>(dims), sizeof(dims));
        if (!file || dims[2] > (1u << 28)) {
            return false;
        }
        MipLevel mip;
        mip.width = static_cast<int>(dims[0]);
        mip.height = static_cast<int>(dims[1]);
        mip.data.resize(dims[2]);
        file.read(reinterpret_cast<char*>(mip.data.data()), static_cast<std::streamsize>(dims[2]));
        if (!file) {
            return false;
        }
        out.mips.push_back(std::move(mip));
    }
    return !out.mips.empty();
}

void write_cache(const std::filesystem::path& path, std::uint64_t source_size, std::int64_t source_time, const TextureData& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return;
        }
        const std::uint32_t head[2] = {kCacheMagic, kCacheVersion};
        file.write(reinterpret_cast<const char*>(head), sizeof(head));
        file.write(reinterpret_cast<const char*>(&source_size), 8);
        file.write(reinterpret_cast<const char*>(&source_time), 8);
        const std::uint32_t info[3] = {data.internal_format, data.compressed ? 1u : 0u, static_cast<std::uint32_t>(data.mips.size())};
        file.write(reinterpret_cast<const char*>(info), sizeof(info));
        for (const MipLevel& mip : data.mips) {
            const std::uint32_t dims[3] = {static_cast<std::uint32_t>(mip.width), static_cast<std::uint32_t>(mip.height), static_cast<std::uint32_t>(mip.data.size())};
            file.write(reinterpret_cast<const char*>(dims), sizeof(dims));
            file.write(reinterpret_cast<const char*>(mip.data.data()), static_cast<std::streamsize>(mip.data.size()));
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
    }
}

} // namespace Hollow::texcodec
