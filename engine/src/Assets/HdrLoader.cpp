// Implementation of load_radiance_hdr() from HdrLoader.hpp: parses the
// Radiance header/resolution line, then decodes each scanline (new RLE,
// legacy RLE, or flat) into RGBE and converts it to clamped linear float
// RGB via rgbe_to_linear().

#include "Hollow/Assets/HdrLoader.hpp"
#include "Hollow/Core/FileUtil.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Hollow {
namespace {

bool read_line(std::istream& in, std::string& line) {
    if (!std::getline(in, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

// Decodes one RGBE (Radiance shared-exponent) texel into linear float RGB.
// Standard formula from Greg Ward's reference `rgbe2float`: mantissa bytes
// are scaled by 2^(exponent-(128+8)); an exponent of 0 encodes black.
void rgbe_to_linear(unsigned char r, unsigned char g, unsigned char b, unsigned char e, float& out_r, float& out_g, float& out_b) {
    if (e == 0) {
        out_r = out_g = out_b = 0.0f;
        return;
    }
    const float f = std::ldexp(1.0f, static_cast<int>(e) - (128 + 8));
    // Clamp to just under the largest value a 16-bit float can hold (65504)
    // — every IBL bake target this decodes into (the equirect source
    // texture, and every cubemap baked from it) uses GL_RGBA16F. A
    // real-world outdoor HDRI often has a directly-visible sun disc whose
    // decoded radiance (r/g/b * 2^(e-136), computed here in full float32
    // range) can run into the tens of thousands or higher — nowhere near a
    // float32 overflow, but well past what a half-float texture can store.
    // Uploaded as-is, those texels silently become +Infinity on the GPU,
    // and Infinity then poisons every downstream sum that touches it: the
    // irradiance convolution and the GGX specular prefilter both average
    // many texture samples into each output texel, so one overflowing
    // pixel in the source panorama can spread into widespread NaN/Inf
    // "boxy" corruption across the baked cubemaps (Reinhard tonemapping,
    // x / (x + 1), turns an Infinity fragment into Inf / Inf = NaN, which
    // is exactly the kind of undefined-looking blotch this was producing).
    // Clamping once here, upstream of every GPU upload, is simpler and more
    // robust than guarding every shader that might later sample it.
    constexpr float kMaxSafeRadiance = 65000.0f;
    out_r = std::min(r * f, kMaxSafeRadiance);
    out_g = std::min(g * f, kMaxSafeRadiance);
    out_b = std::min(b * f, kMaxSafeRadiance);
}

} // namespace

HdrImage load_radiance_hdr(const std::string& path) {
    std::ifstream file(path_from_utf8(path), std::ios::binary); // path is UTF-8
    if (!file) {
        throw std::runtime_error("could not open HDR file: " + path);
    }

    std::string first_line;
    if (!read_line(file, first_line) || first_line.size() < 2 || first_line[0] != '#' || first_line[1] != '?') {
        throw std::runtime_error("not a Radiance HDR file (missing '#?' magic): " + path);
    }

    // Header fields (FORMAT=, EXPOSURE=, comments, ...) run until a blank
    // line; we don't need any of them for RGBE data, just to skip past them.
    std::string header_line;
    bool found_blank_line = false;
    while (read_line(file, header_line)) {
        if (header_line.empty()) {
            found_blank_line = true;
            break;
        }
    }
    if (!found_blank_line) {
        throw std::runtime_error("HDR file has no header/data separator: " + path);
    }

    std::string resolution_line;
    if (!read_line(file, resolution_line)) {
        throw std::runtime_error("HDR file missing resolution line: " + path);
    }

    std::istringstream resolution_stream(resolution_line);
    std::string y_sign;
    std::string x_sign;
    int height = 0;
    int width = 0;
    resolution_stream >> y_sign >> height >> x_sign >> width;
    if (!resolution_stream || y_sign != "-Y" || x_sign != "+X" || height <= 0 || width <= 0) {
        throw std::runtime_error("unsupported HDR orientation/resolution (only top-down '-Y H +X W' is supported): " + path);
    }

    HdrImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);

    std::vector<unsigned char> scanline(static_cast<std::size_t>(width) * 4);

    for (int y = 0; y < height; ++y) {
        unsigned char header[4];
        file.read(reinterpret_cast<char*>(header), 4);
        if (!file) {
            throw std::runtime_error("unexpected end of HDR pixel data: " + path);
        }

        const bool is_new_rle = width >= 8 && width < 32768 && header[0] == 2 && header[1] == 2
            && ((static_cast<int>(header[2]) << 8 | static_cast<int>(header[3])) == width);

        if (is_new_rle) {
            for (int channel = 0; channel < 4; ++channel) {
                int x = 0;
                while (x < width) {
                    unsigned char count = 0;
                    file.read(reinterpret_cast<char*>(&count), 1);
                    if (!file) {
                        throw std::runtime_error("truncated HDR scanline: " + path);
                    }
                    if (count > 128) {
                        const int run_length = count - 128;
                        unsigned char value = 0;
                        file.read(reinterpret_cast<char*>(&value), 1);
                        if (!file || x + run_length > width) {
                            throw std::runtime_error("corrupt HDR RLE run: " + path);
                        }
                        for (int i = 0; i < run_length; ++i) {
                            scanline[static_cast<std::size_t>(x + i) * 4 + static_cast<std::size_t>(channel)] = value;
                        }
                        x += run_length;
                    } else {
                        const int literal_count = count;
                        if (x + literal_count > width) {
                            throw std::runtime_error("corrupt HDR RLE literal run: " + path);
                        }
                        for (int i = 0; i < literal_count; ++i) {
                            unsigned char value = 0;
                            file.read(reinterpret_cast<char*>(&value), 1);
                            if (!file) {
                                throw std::runtime_error("truncated HDR scanline: " + path);
                            }
                            scanline[static_cast<std::size_t>(x + i) * 4 + static_cast<std::size_t>(channel)] = value;
                        }
                        x += literal_count;
                    }
                }
            }
        } else {
            // Legacy flat/old-RLE scanline; `header` already holds pixel 0.
            scanline[0] = header[0];
            scanline[1] = header[1];
            scanline[2] = header[2];
            scanline[3] = header[3];
            int x = 1;
            int shift = 0;
            while (x < width) {
                unsigned char rgbe[4];
                file.read(reinterpret_cast<char*>(rgbe), 4);
                if (!file) {
                    throw std::runtime_error("truncated legacy HDR scanline: " + path);
                }
                if (rgbe[0] == 1 && rgbe[1] == 1 && rgbe[2] == 1) {
                    const int count = static_cast<int>(rgbe[3]) << shift;
                    if (x + count > width) {
                        throw std::runtime_error("corrupt legacy HDR RLE run: " + path);
                    }
                    for (int i = 0; i < count; ++i) {
                        for (int c = 0; c < 4; ++c) {
                            scanline[static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(c)] = scanline[static_cast<std::size_t>(x - 1) * 4 + static_cast<std::size_t>(c)];
                        }
                        ++x;
                    }
                    shift += 8;
                } else {
                    scanline[static_cast<std::size_t>(x) * 4 + 0] = rgbe[0];
                    scanline[static_cast<std::size_t>(x) * 4 + 1] = rgbe[1];
                    scanline[static_cast<std::size_t>(x) * 4 + 2] = rgbe[2];
                    scanline[static_cast<std::size_t>(x) * 4 + 3] = rgbe[3];
                    ++x;
                    shift = 0;
                }
            }
        }

        const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3;
        for (int x = 0; x < width; ++x) {
            const unsigned char r = scanline[static_cast<std::size_t>(x) * 4 + 0];
            const unsigned char g = scanline[static_cast<std::size_t>(x) * 4 + 1];
            const unsigned char b = scanline[static_cast<std::size_t>(x) * 4 + 2];
            const unsigned char e = scanline[static_cast<std::size_t>(x) * 4 + 3];
            float fr = 0.0f;
            float fg = 0.0f;
            float fb = 0.0f;
            rgbe_to_linear(r, g, b, e, fr, fg, fb);
            image.pixels[row_offset + static_cast<std::size_t>(x) * 3 + 0] = fr;
            image.pixels[row_offset + static_cast<std::size_t>(x) * 3 + 1] = fg;
            image.pixels[row_offset + static_cast<std::size_t>(x) * 3 + 2] = fb;
        }
    }

    return image;
}

} // namespace Hollow
