#pragma once
struct Particle {
    vec4 position_age;  // xyz, age (s)
    vec4 velocity_life; // xyz, lifetime (s)
    vec4 extra;         // x random, y random
};
layout(std430, binding = SSBO_SCRATCH0) buffer ParticleBuffer {
    Particle particles[];
};
