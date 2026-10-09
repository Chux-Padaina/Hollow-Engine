// Final composite: bloom (+ lens dirt), chromatic aberration, vignette,
// the grading/tonemapping LUT, film grain, sRGB encoding and dithering.
#include "frame.glsl"
#include "tonemappers.glsl"

in vec2 v_uv;
layout(binding = 0) uniform sampler2D t_color;
layout(binding = 1) uniform sampler2D t_bloom;
layout(binding = 2) uniform sampler3D t_lut;
layout(binding = 3) uniform sampler2D t_dirt;
layout(location = 0) out vec4 o_color;
uniform float u_bloom;
uniform float u_dirt;
uniform float u_vignette;
uniform float u_chromatic;
uniform float u_grain;
uniform int u_has_bloom;
uniform float u_bloom_scale; // 1 / bloom levels (the upsample chain sums them)

void main() {
    vec2 uv = v_uv;
    vec3 color;
    if (u_chromatic > 0.0) {
        vec2 dir = uv - 0.5;
        float amount = u_chromatic * 0.012 * dot(dir, dir) * 4.0;
        color.r = textureLod(t_color, uv - dir * amount, 0.0).r;
        color.g = textureLod(t_color, uv, 0.0).g;
        color.b = textureLod(t_color, uv + dir * amount, 0.0).b;
    } else {
        color = textureLod(t_color, uv, 0.0).rgb;
    }
    int debug = u_flags.x;
    if (debug != DEBUG_FINAL) {
        // Debug buffers are shown as-is.
        o_color = vec4(linear_to_srgb(saturate(color)), 1.0);
        return;
    }
    if (u_has_bloom != 0) {
        vec3 bloom = textureLod(t_bloom, uv, 0.0).rgb * u_bloom_scale;
        color = mix(color, bloom, u_bloom);
        if (u_dirt > 0.0) {
            color += bloom * textureLod(t_dirt, uv, 0.0).rgb * u_dirt * u_bloom * 8.0;
        }
    }
    if (u_vignette > 0.0) {
        vec2 d = (uv - 0.5) * vec2(u_output_size.x / u_output_size.y, 1.0);
        float v = 1.0 - u_vignette * smoothstep(0.25, 1.1, length(d) * 1.2);
        color *= v;
    }
    // Grading + tonemapping LUT (half-texel addressing).
    vec3 encoded = lut_encode(color);
    vec3 size = vec3(textureSize(t_lut, 0));
    vec3 display = textureLod(t_lut, encoded * ((size - 1.0) / size) + 0.5 / size, 0.0).rgb;
    if (u_grain > 0.0) {
        float n = hash_float(uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + uint(u_flags.w) * 26699u) - 0.5;
        float lum = luminance(display);
        display += n * u_grain * 0.12 * (1.0 - lum) * sqrt(lum + 0.02);
    }
    vec3 encoded_out = linear_to_srgb(saturate(display));
    // Triangular dither: removes banding in skies and gradients.
    float r1 = interleaved_gradient_noise(gl_FragCoord.xy, float(u_flags.w));
    float r2 = interleaved_gradient_noise(gl_FragCoord.xy + 37.0, float(u_flags.w));
    encoded_out += (r1 + r2 - 1.0) / 255.0;
    o_color = vec4(encoded_out, 1.0);
}
