// Cubemap face/texel <-> direction (OpenGL face order +X -X +Y -Y +Z -Z).
#pragma once

vec3 cube_direction(int face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;
    vec3 d;
    switch (face) {
    case 0: d = vec3(1.0, -st.y, -st.x); break;
    case 1: d = vec3(-1.0, -st.y, st.x); break;
    case 2: d = vec3(st.x, 1.0, st.y); break;
    case 3: d = vec3(st.x, -1.0, -st.y); break;
    case 4: d = vec3(st.x, -st.y, 1.0); break;
    default: d = vec3(-st.x, -st.y, -1.0); break;
    }
    return normalize(d);
}
