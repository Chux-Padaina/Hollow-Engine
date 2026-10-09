// CPU-side asset data shared between the scene, importer and renderer:
// Vertex, Material (glTF metallic-roughness plus the common KHR material
// extensions), and MeshAsset (with LODs and optional skinning data).
//
// A MeshAsset is immutable once created and is shared by every actor that
// uses it through a MeshHandle (shared_ptr). The renderer keys its GPU
// buffers by MeshAsset::id, so ten actors using the same imported model
// share one vertex/index range in the renderer's mesh pool.
#pragma once

#include "Hollow/Core/Math.hpp"
#include "Hollow/Core/Properties.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Hollow {

enum class AlphaMode {
    Opaque,
    Mask,
    Blend,
};

struct Material {
    // Core metallic-roughness
    Vec3 base_color{0.75f, 0.72f, 0.62f};
    float base_color_alpha = 1.0f;
    float metallic = 0.0f;
    float roughness = 0.55f;
    float normal_strength = 1.0f;
    Vec3 emissive_factor{0.0f, 0.0f, 0.0f};
    // Multiplies emissive_factor (KHR_materials_emissive_strength). Emissive
    // is exposure-relative unless emissive_nits is set.
    float emissive_strength = 1.0f;
    bool emissive_nits = false;
    float occlusion_strength = 1.0f;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    bool double_sided = false;
    bool unlit = false;
    float specular = 1.0f; // KHR_materials_specular factor
    float ior = 1.5f;

    // Clearcoat (car paint, lacquer)
    float clearcoat = 0.0f;
    float clearcoat_roughness = 0.03f;
    // Sheen (cloth, velvet)
    Vec3 sheen_color{0.0f, 0.0f, 0.0f};
    float sheen_roughness = 0.3f;
    // Transmission + volume (glass, liquids)
    float transmission = 0.0f;
    float thickness = 0.0f; // meters
    Vec3 attenuation_color{1.0f, 1.0f, 1.0f};
    float attenuation_distance = 0.0f; // 0 = no absorption
    // Anisotropy (brushed metal, hair)
    float anisotropy = 0.0f;           // -1..1
    float anisotropy_rotation = 0.0f;  // radians
    // Subsurface approximation (skin, wax, leaves)
    float subsurface = 0.0f;
    Vec3 subsurface_color{1.0f, 0.35f, 0.25f};
    // Parallax occlusion mapping (needs a height texture)
    float parallax_scale = 0.0f;
    // UV transform
    float uv_scale_x = 1.0f;
    float uv_scale_y = 1.0f;
    float uv_offset_x = 0.0f;
    float uv_offset_y = 0.0f;

    // UTF-8 file paths; empty means "no texture".
    std::string base_color_texture_path;
    std::string metallic_roughness_texture_path; // G roughness, B metallic
    std::string normal_texture_path;
    std::string emissive_texture_path;
    std::string occlusion_texture_path;
    std::string height_texture_path;
    std::string clearcoat_texture_path;    // R factor, G roughness
    std::string sheen_texture_path;        // RGB color, A roughness
    std::string transmission_texture_path; // R

    bool uses_extensions() const;
};

// Inspector + serialization description (see Core/Properties.hpp).
void describe(Material& material, PropertyVisitor& v);

struct Vertex {
    Vec3 position{};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec3 tangent{1.0f, 0.0f, 0.0f};
    float tangent_sign = 1.0f;
    Vec2 uv{};
    Vec3 color{1.0f, 1.0f, 1.0f};
};

// A level of detail: a range of MeshAsset::indices drawn instead of the
// full mesh once the mesh covers less than `screen_size` of the screen
// height. LOD 0 is the full mesh.
struct MeshLod {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    float screen_size = 0.0f;
    float error = 0.0f; // simplification error (fraction of the mesh size)
};

// Per-vertex skinning influences (up to 4 joints).
struct SkinInfluence {
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};
};

struct Skeleton;

struct MeshAsset {
    std::uint64_t id = 0;      // unique for the lifetime of the process
    std::string source;        // "builtin:cube" or "model:<utf8 path>#<part>"
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices; // LOD 0 first, then the other LODs
    std::vector<MeshLod> lods;          // at least one (filled by make_mesh_asset)
    Aabb bounds;               // local space
    bool radial_outline = false;

    // Skinning (empty for static meshes). `skin_joints[i]` is the skeleton
    // joint driving skin joint i, `inverse_bind[i]` its inverse bind matrix.
    std::vector<SkinInfluence> skin;
    std::vector<int> skin_joints;
    std::vector<Mat4> inverse_bind;
    std::shared_ptr<const Skeleton> skeleton;

    bool skinned() const { return !skin.empty() && skeleton != nullptr; }
    std::size_t triangle_count(std::size_t lod = 0) const;
};

using MeshHandle = std::shared_ptr<const MeshAsset>;

struct MeshBuildOptions {
    bool generate_lods = true;
    std::size_t lod_min_triangles = 600; // smaller meshes keep one LOD
};

// Fills in id, bounds and (optionally) LODs and wraps the asset in a handle.
MeshHandle make_mesh_asset(MeshAsset asset, const MeshBuildOptions& options = {});
// Recomputes per-vertex tangents from positions/UVs (used by the importer
// and the built-in primitives).
void generate_tangents(std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices);

} // namespace Hollow
