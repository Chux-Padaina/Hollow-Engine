// MeshAsset helpers: id assignment, bounds, LODs, tangent generation, and
// the Material property description.
#include "Hollow/Assets/MeshAsset.hpp"

#include "Hollow/Assets/MeshSimplify.hpp"

#include <array>
#include <atomic>

namespace Hollow {
namespace {

std::atomic<std::uint64_t> g_next_mesh_id{1};

Vec3 fallback_tangent(Vec3 normal) {
    const Vec3 up = std::abs(normal.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    return normalize(cross(up, normal));
}

} // namespace

MeshHandle make_mesh_asset(MeshAsset asset, const MeshBuildOptions& options) {
    asset.id = g_next_mesh_id.fetch_add(1);
    asset.bounds = Aabb::empty();
    for (const Vertex& vertex : asset.vertices) {
        asset.bounds.expand(vertex.position);
    }
    if (!asset.bounds.valid()) {
        asset.bounds = {};
    }
    if (asset.skinned()) {
        // Animation moves vertices outside the bind pose: pad the bounds
        // so culling stays conservative.
        const Vec3 pad = (asset.bounds.max - asset.bounds.min) * 0.5f;
        asset.bounds.min -= pad;
        asset.bounds.max += pad;
    }
    if (asset.lods.empty()) {
        asset.lods.push_back({0, static_cast<std::uint32_t>(asset.indices.size()), 1.0f, 0.0f});
        // Skinned meshes deform, so a static simplification would be wrong
        // in motion; they keep one LOD.
        if (options.generate_lods && !asset.skinned() && asset.indices.size() / 3 >= options.lod_min_triangles) {
            generate_lods(asset);
        }
    }
    return std::make_shared<const MeshAsset>(std::move(asset));
}

std::size_t MeshAsset::triangle_count(std::size_t lod) const {
    if (lod < lods.size()) {
        return lods[lod].index_count / 3;
    }
    return indices.size() / 3;
}

bool Material::uses_extensions() const {
    return clearcoat > 0.0f || max_component(sheen_color) > 0.0f || transmission > 0.0f || anisotropy != 0.0f || subsurface > 0.0f || parallax_scale > 0.0f;
}

namespace {
constexpr std::array<const char*, 3> kAlphaModeNames{"opaque", "mask", "blend"};
} // namespace

void describe(Material& m, PropertyVisitor& v) {
    v.color("base_color", "Base color", m.base_color);
    v.number("alpha", "Opacity", m.base_color_alpha, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("metallic", "Metallic", m.metallic, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.number("roughness", "Roughness", m.roughness, {0.02f, 1.0f, 0.01f, "%.2f"});
    v.number("normal_strength", "Normal strength", m.normal_strength, {0.0f, 3.0f, 0.01f, "%.2f"});
    v.number("occlusion_strength", "Occlusion", m.occlusion_strength, {0.0f, 1.0f, 0.01f, "%.2f"});
    v.color("emissive", "Emissive", m.emissive_factor, true);
    v.number("emissive_strength", "Emissive strength", m.emissive_strength, {0.0f, 1000.0f, 0.05f, "%.2f", true, true});
    v.boolean("emissive_nits", "Emissive in nits", m.emissive_nits, "Physical luminance (cd/m^2) instead of exposure-relative brightness.");
    v.enumeration("alpha_mode", "Alpha mode", m.alpha_mode, kAlphaModeNames);
    if (v.show(m.alpha_mode == AlphaMode::Mask)) {
        v.number("alpha_cutoff", "Alpha cutoff", m.alpha_cutoff, {0.0f, 1.0f, 0.01f, "%.2f"});
    }
    v.boolean("double_sided", "Double sided", m.double_sided);
    v.boolean("unlit", "Unlit", m.unlit);
    v.number("specular", "Specular", m.specular, {0.0f, 2.0f, 0.01f, "%.2f"});
    v.number("ior", "IOR", m.ior, {1.0f, 3.0f, 0.005f, "%.3f"});
    if (PropertyGroup g{v, nullptr, "Clearcoat"}) {
        v.number("clearcoat", "Clearcoat", m.clearcoat, {0.0f, 1.0f, 0.01f, "%.2f"});
        if (v.show(m.clearcoat > 0.0f)) {
            v.number("clearcoat_roughness", "Coat roughness", m.clearcoat_roughness, {0.02f, 1.0f, 0.01f, "%.2f"});
        }
    }
    if (PropertyGroup g{v, nullptr, "Sheen"}) {
        v.color("sheen_color", "Sheen color", m.sheen_color);
        if (v.show(max_component(m.sheen_color) > 0.0f)) {
            v.number("sheen_roughness", "Sheen roughness", m.sheen_roughness, {0.05f, 1.0f, 0.01f, "%.2f"});
        }
    }
    if (PropertyGroup g{v, nullptr, "Transmission"}) {
        v.number("transmission", "Transmission", m.transmission, {0.0f, 1.0f, 0.01f, "%.2f"}, "Glass-like refraction (needs Alpha mode Blend to render in the transparent pass).");
        if (v.show(m.transmission > 0.0f)) {
            v.number("thickness", "Thickness", m.thickness, {0.0f, 2.0f, 0.005f, "%.3f m"});
            v.color("attenuation_color", "Absorption color", m.attenuation_color);
            v.number("attenuation_distance", "Absorption distance", m.attenuation_distance, {0.0f, 10.0f, 0.01f, "%.2f m"});
        }
    }
    if (PropertyGroup g{v, nullptr, "Anisotropy"}) {
        v.number("anisotropy", "Anisotropy", m.anisotropy, {-1.0f, 1.0f, 0.01f, "%.2f"});
        if (v.show(m.anisotropy != 0.0f)) {
            v.angle("anisotropy_rotation_degrees", "Rotation", m.anisotropy_rotation, -180.0f, 180.0f);
        }
    }
    if (PropertyGroup g{v, nullptr, "Subsurface"}) {
        v.number("subsurface", "Subsurface", m.subsurface, {0.0f, 1.0f, 0.01f, "%.2f"});
        if (v.show(m.subsurface > 0.0f)) {
            v.color("subsurface_color", "Scatter color", m.subsurface_color);
        }
    }
    if (PropertyGroup g{v, nullptr, "UV & parallax"}) {
        v.number("uv_scale_x", "UV scale X", m.uv_scale_x, {0.01f, 100.0f, 0.01f, "%.2f", false});
        v.number("uv_scale_y", "UV scale Y", m.uv_scale_y, {0.01f, 100.0f, 0.01f, "%.2f", false});
        v.number("uv_offset_x", "UV offset X", m.uv_offset_x, {-10.0f, 10.0f, 0.01f, "%.2f", false});
        v.number("uv_offset_y", "UV offset Y", m.uv_offset_y, {-10.0f, 10.0f, 0.01f, "%.2f", false});
        v.number("parallax_scale", "Parallax depth", m.parallax_scale, {0.0f, 0.2f, 0.001f, "%.3f"}, "Parallax occlusion mapping depth (needs a height texture).");
    }
    if (PropertyGroup g{v, "textures", "Textures"}) {
        v.path("base_color", "Base color", m.base_color_texture_path, "Images");
        v.path("metallic_roughness", "Metal/rough", m.metallic_roughness_texture_path, "Images");
        v.path("normal", "Normal", m.normal_texture_path, "Images");
        v.path("emissive", "Emissive", m.emissive_texture_path, "Images");
        v.path("occlusion", "Occlusion", m.occlusion_texture_path, "Images");
        v.path("height", "Height", m.height_texture_path, "Images");
        v.path("clearcoat", "Clearcoat", m.clearcoat_texture_path, "Images");
        v.path("sheen", "Sheen", m.sheen_texture_path, "Images");
        v.path("transmission", "Transmission", m.transmission_texture_path, "Images");
    }
}

void generate_tangents(std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    std::vector<Vec3> tangents(vertices.size(), Vec3{});
    std::vector<Vec3> bitangents(vertices.size(), Vec3{});

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t i0 = indices[i];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
        const Vertex& v0 = vertices[i0];
        const Vertex& v1 = vertices[i1];
        const Vertex& v2 = vertices[i2];
        const Vec3 edge1 = v1.position - v0.position;
        const Vec3 edge2 = v2.position - v0.position;
        const Vec2 duv1{v1.uv.x - v0.uv.x, v1.uv.y - v0.uv.y};
        const Vec2 duv2{v2.uv.x - v0.uv.x, v2.uv.y - v0.uv.y};
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) <= 0.000001f) {
            continue;
        }
        const float r = 1.0f / determinant;
        const Vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * r;
        const Vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;
        for (std::uint32_t index : {i0, i1, i2}) {
            tangents[index] += tangent;
            bitangents[index] += bitangent;
        }
    }

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        Vertex& vertex = vertices[i];
        const Vec3 normal = normalize(vertex.normal);
        Vec3 tangent = normalize(tangents[i] - normal * dot(normal, tangents[i]));
        if (length(tangent) <= 0.0001f) {
            tangent = fallback_tangent(normal);
        }
        vertex.tangent = tangent;
        vertex.tangent_sign = dot(cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
    }
}

} // namespace Hollow
