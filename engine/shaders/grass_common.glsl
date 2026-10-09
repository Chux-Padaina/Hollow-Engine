// Procedural grass blades shared by grass_cull.comp and grass.vert.
// Blade buffer layout (RendererScene.cpp): two vec4 per blade
//   position (actor space) + rotation, height + width + tint + phase.
#pragma once

layout(std430, binding = SSBO_SCRATCH0) readonly buffer GrassBlades {
    vec4 grass_blades[];
};

uniform mat4 u_model;
uniform float u_distance; // draw distance (m)
uniform float u_wind;
uniform uint u_offset;    // this terrain's region in the visible list

// Fraction of blades kept at a distance; the rest are culled and the
// survivors widen to keep the coverage.
float grass_keep(float d) {
    return mix(1.0, 0.25, smoothstep(0.15, 1.0, d / u_distance));
}
