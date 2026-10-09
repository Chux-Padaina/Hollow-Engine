// Mesh simplification for automatic LODs: quadric-error-metric edge
// collapses (Garland & Heckbert) that only ever move a vertex onto one of
// its neighbors, so every LOD reuses the original vertex buffer and only
// needs its own index range. UV seams and open borders are locked so
// textures and silhouettes hold up.
#pragma once

#include "Hollow/Assets/MeshAsset.hpp"

#include <cstdint>
#include <vector>

namespace Hollow {

struct SimplifyResult {
    std::vector<std::uint32_t> indices;
    float error = 0.0f; // RMS quadric error relative to the mesh's size
};

// Collapses edges until at most `target_triangles` remain or the next
// collapse would exceed `max_error` (relative to the bounding radius).
SimplifyResult simplify_mesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices, std::size_t target_triangles, float max_error = 0.05f);

// Builds up to 3 extra LODs (50%, 25%, 12.5% of the triangles) and appends
// them to mesh.indices / mesh.lods.
void generate_lods(MeshAsset& mesh);

} // namespace Hollow
