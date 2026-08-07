// Loader for Radiance RGBE (.hdr) environment maps used for image-based
// lighting (IBL). Decodes into a linear float RGB buffer consumed by the
// IBL bake step (irradiance convolution / GGX specular prefilter) before
// upload to the GPU.

#pragma once

#include <string>
#include <vector>

namespace Hollow {

// A decoded Radiance (.hdr / .pic) image: linear, tightly-packed RGB float
// triples, top row first, left-to-right — ready to upload directly as a
// GL_FLOAT/GL_RGB texture. `pixels.size() == width * height * 3`.
struct HdrImage {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;
};

// Parses a Radiance RGBE (.hdr) file: the standard format used by
// environment/HDRI downloads (Poly Haven, HDRI Haven, etc.). Supports both
// the common "new" per-scanline RLE encoding and legacy flat/old-RLE
// scanlines. Throws std::runtime_error with a description if the file can't
// be opened or doesn't parse as a valid Radiance HDR image.
//
// Only the standard `-Y <height> +X <width>` (top-down, left-to-right)
// resolution/orientation is supported, which covers the overwhelming
// majority of real-world HDRI files; anything else throws rather than
// silently loading a flipped/rotated image.
HdrImage load_radiance_hdr(const std::string& path);

} // namespace Hollow
