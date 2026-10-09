// Scene extraction: turns the Scene into GPU data each frame. Meshes live
// in a shared pool (one vertex + index buffer); every visible mesh part
// becomes an instance; instances are sorted into batches (same category,
// cull mode and texture set) so each batch is one multi-draw-indirect
// call. Also gathers lights (physical units -> GPU), reflection probes,
// irradiance volumes, decals and terrain chunks.
#include "Render/RendererImpl.hpp"

#include "Hollow/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace Hollow {

using texcodec::Usage;

namespace {

Vec4 v4(Vec3 v, float w) {
    return {v.x, v.y, v.z, w};
}

constexpr std::uint32_t kInitialVertices = 1u << 18;
constexpr std::uint32_t kInitialIndices = 1u << 20;

std::uint64_t instance_key(ActorId actor, std::uint32_t part, std::uint32_t kind = 0) {
    return (static_cast<std::uint64_t>(kind) << 56) | (static_cast<std::uint64_t>(actor) << 24) | part;
}

void collect_active(const Scene& scene, ActorId id, std::vector<const Actor*>& out) {
    const Actor* actor = scene.find(id);
    if (actor == nullptr || !actor->active) {
        return; // an inactive actor hides its whole subtree
    }
    out.push_back(actor);
    for (ActorId child : actor->children()) {
        collect_active(scene, child, out);
    }
}

// Approximate linear RGB of a black-body temperature (normalized luminance).
Vec3 kelvin_rgb(float kelvin) {
    const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    float r, g, b;
    r = t <= 66.0f ? 1.0f : std::clamp(1.29293618606f * std::pow(t - 60.0f, -0.1332047592f), 0.0f, 1.0f);
    g = t <= 66.0f ? std::clamp(0.39008157876f * std::log(t) - 0.63184144378f, 0.0f, 1.0f) : std::clamp(1.12989086089f * std::pow(t - 60.0f, -0.0755148492f), 0.0f, 1.0f);
    b = t >= 66.0f ? 1.0f : (t <= 19.0f ? 0.0f : std::clamp(0.54320678911f * std::log(t - 10.0f) - 1.19625408914f, 0.0f, 1.0f));
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    Vec3 c{lin(r), lin(g), lin(b)};
    const float lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    return lum > 1e-4f ? c / lum : Vec3{1.0f, 1.0f, 1.0f};
}

} // namespace

// --- Range allocator ------------------------------------------------------------------

void RangeAllocator::reset(std::uint32_t capacity) {
    free_.clear();
    free_.push_back({0, capacity});
    capacity_ = capacity;
    used_ = 0;
}

bool RangeAllocator::allocate(std::uint32_t count, Range& out) {
    if (count == 0) {
        out = {0, 0};
        return true;
    }
    for (std::size_t i = 0; i < free_.size(); ++i) {
        Range& range = free_[i];
        if (range.count >= count) {
            out = {range.offset, count};
            range.offset += count;
            range.count -= count;
            if (range.count == 0) {
                free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            used_ += count;
            return true;
        }
    }
    return false;
}

void RangeAllocator::free(Range range) {
    if (range.count == 0) {
        return;
    }
    used_ -= std::min(used_, range.count);
    const auto it = std::lower_bound(free_.begin(), free_.end(), range, [](const Range& a, const Range& b) { return a.offset < b.offset; });
    const auto inserted = free_.insert(it, range);
    // Merge with neighbors.
    std::size_t index = static_cast<std::size_t>(inserted - free_.begin());
    if (index + 1 < free_.size() && free_[index].offset + free_[index].count == free_[index + 1].offset) {
        free_[index].count += free_[index + 1].count;
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(index + 1));
    }
    if (index > 0 && free_[index - 1].offset + free_[index - 1].count == free_[index].offset) {
        free_[index - 1].count += free_[index].count;
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void RangeAllocator::grow(std::uint32_t new_capacity) {
    if (new_capacity <= capacity_) {
        return;
    }
    free({capacity_, new_capacity - capacity_});
    used_ += new_capacity - capacity_; // free() subtracted it
    capacity_ = new_capacity;
}

// --- Mesh pool ---------------------------------------------------------------------------

void Renderer::Impl::init_mesh_pool() {
    pool.vertices = gfx::Buffer(static_cast<std::size_t>(kInitialVertices) * sizeof(Vertex), GL_DYNAMIC_STORAGE_BIT, nullptr, "MeshPool.Vertices");
    pool.indices = gfx::Buffer(static_cast<std::size_t>(kInitialIndices) * sizeof(std::uint32_t), GL_DYNAMIC_STORAGE_BIT, nullptr, "MeshPool.Indices");
    pool.infos = gfx::Buffer(sizeof(gpu::MeshInfo) * 256, GL_DYNAMIC_STORAGE_BIT, nullptr, "MeshPool.Infos");
    pool.vertex_alloc.reset(kInitialVertices);
    pool.index_alloc.reset(kInitialIndices);
    ensure_draw_ids(4096);

    glCreateVertexArrays(1, &pool.vao);
    const GLuint vao = pool.vao;
    struct Attribute {
        GLuint location;
        GLint size;
        GLuint offset;
    };
    const Attribute attributes[] = {
        {0, 3, static_cast<GLuint>(offsetof(Vertex, position))},
        {1, 3, static_cast<GLuint>(offsetof(Vertex, normal))},
        {2, 3, static_cast<GLuint>(offsetof(Vertex, tangent))},
        {3, 1, static_cast<GLuint>(offsetof(Vertex, tangent_sign))},
        {4, 2, static_cast<GLuint>(offsetof(Vertex, uv))},
        {5, 3, static_cast<GLuint>(offsetof(Vertex, color))},
    };
    for (const Attribute& a : attributes) {
        glEnableVertexArrayAttrib(vao, a.location);
        glVertexArrayAttribFormat(vao, a.location, a.size, GL_FLOAT, GL_FALSE, a.offset);
        glVertexArrayAttribBinding(vao, a.location, 0);
    }
    glEnableVertexArrayAttrib(vao, 6);
    glVertexArrayAttribIFormat(vao, 6, 1, GL_UNSIGNED_INT, 0);
    glVertexArrayAttribBinding(vao, 6, 1);
    glVertexArrayBindingDivisor(vao, 1, 1);
    glVertexArrayVertexBuffer(vao, 0, pool.vertices.id(), 0, sizeof(Vertex));
    glVertexArrayVertexBuffer(vao, 1, pool.draw_ids.id(), 0, sizeof(std::uint32_t));
    glVertexArrayElementBuffer(vao, pool.indices.id());
    gfx::set_label(GL_VERTEX_ARRAY, vao, "MeshPool.VAO");
}

void Renderer::Impl::destroy_mesh_pool() {
    if (pool.vao != 0) {
        glDeleteVertexArrays(1, &pool.vao);
        pool.vao = 0;
    }
    pool = MeshPool{};
    pooled_meshes.clear();
    skinned_regions.clear();
    skin_offsets.clear();
}

void Renderer::Impl::ensure_draw_ids(std::size_t count) {
    if (count <= pool.draw_id_capacity && pool.draw_ids) {
        return;
    }
    std::size_t capacity = std::max<std::size_t>(pool.draw_id_capacity, 4096);
    while (capacity < count) {
        capacity *= 2;
    }
    std::vector<std::uint32_t> ids(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        ids[i] = static_cast<std::uint32_t>(i);
    }
    pool.draw_ids = gfx::Buffer(capacity * sizeof(std::uint32_t), 0, ids.data(), "MeshPool.DrawIds");
    pool.draw_id_capacity = capacity;
    if (pool.vao != 0) {
        glVertexArrayVertexBuffer(pool.vao, 1, pool.draw_ids.id(), 0, sizeof(std::uint32_t));
    }
}

bool Renderer::Impl::allocate_vertices(std::uint32_t count, RangeAllocator::Range& out) {
    while (!pool.vertex_alloc.allocate(count, out)) {
        const std::uint32_t capacity = std::max(pool.vertex_alloc.capacity() * 2, pool.vertex_alloc.capacity() + count);
        pool.vertices.grow_preserve(static_cast<std::size_t>(capacity) * sizeof(Vertex), "MeshPool.Vertices");
        pool.vertex_alloc.grow(capacity);
        glVertexArrayVertexBuffer(pool.vao, 0, pool.vertices.id(), 0, sizeof(Vertex));
    }
    return true;
}

PooledMesh& Renderer::Impl::ensure_mesh(const MeshAsset& mesh) {
    auto it = pooled_meshes.find(mesh.id);
    if (it != pooled_meshes.end()) {
        it->second.last_used = frame_index;
        return it->second;
    }
    PooledMesh pooled;
    const auto vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto index_count = static_cast<std::uint32_t>(mesh.indices.size());
    allocate_vertices(vertex_count, pooled.vertex_range);
    while (!pool.index_alloc.allocate(index_count, pooled.index_range)) {
        const std::uint32_t capacity = std::max(pool.index_alloc.capacity() * 2, pool.index_alloc.capacity() + index_count);
        pool.indices.grow_preserve(static_cast<std::size_t>(capacity) * sizeof(std::uint32_t), "MeshPool.Indices");
        pool.index_alloc.grow(capacity);
        glVertexArrayElementBuffer(pool.vao, pool.indices.id());
    }
    pool.vertices.upload(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex), static_cast<std::size_t>(pooled.vertex_range.offset) * sizeof(Vertex));
    pool.indices.upload(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t), static_cast<std::size_t>(pooled.index_range.offset) * sizeof(std::uint32_t));

    gpu::MeshInfo info;
    const std::size_t lod_levels = std::clamp<std::size_t>(mesh.lods.size(), 1, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t source = std::min(i, lod_levels - 1);
        const MeshLod lod = mesh.lods.empty() ? MeshLod{0, index_count, 1.0f, 0.0f} : mesh.lods[source];
        info.lod_first[i] = pooled.index_range.offset + lod.first_index;
        info.lod_count[i] = lod.index_count;
        info.lod_error[i] = i == 0 ? 1e9f : lod.screen_size;
    }
    const Vec3 center = mesh.bounds.center();
    info.local_bounds = v4(center, length(mesh.bounds.extents()));
    info.base_vertex = static_cast<std::int32_t>(pooled.vertex_range.offset);
    info.lod_levels = static_cast<std::uint32_t>(lod_levels);
    info.vertex_count = vertex_count;
    if (!pool.free_slots.empty()) {
        pooled.slot = pool.free_slots.back();
        pool.free_slots.pop_back();
        pool.info_shadow[pooled.slot] = info;
    } else {
        pooled.slot = static_cast<std::uint32_t>(pool.info_shadow.size());
        pool.info_shadow.push_back(info);
    }
    pool.infos_dirty = true;
    pooled.last_used = frame_index;
    pooled.triangles_lod0 = mesh.lods.empty() ? index_count / 3 : mesh.lods[0].index_count / 3;
    return pooled_meshes.emplace(mesh.id, pooled).first->second;
}

void Renderer::Impl::upload_mesh_infos() {
    if (!pool.infos_dirty) {
        return;
    }
    const std::size_t bytes = pool.info_shadow.size() * sizeof(gpu::MeshInfo);
    if (bytes > pool.infos.size()) {
        pool.infos = gfx::Buffer(bytes * 2, GL_DYNAMIC_STORAGE_BIT, nullptr, "MeshPool.Infos");
    }
    pool.infos.upload(pool.info_shadow.data(), bytes);
    pool.infos_dirty = false;
}

void Renderer::Impl::evict_unused_meshes() {
    for (auto it = pooled_meshes.begin(); it != pooled_meshes.end();) {
        if (frame_index - it->second.last_used > 300) {
            pool.vertex_alloc.free(it->second.vertex_range);
            pool.index_alloc.free(it->second.index_range);
            pool.free_slots.push_back(it->second.slot);
            it = pooled_meshes.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = skinned_regions.begin(); it != skinned_regions.end();) {
        if (frame_index - it->second.last_used > 300) {
            pool.vertex_alloc.free(it->second.regions[0]);
            pool.vertex_alloc.free(it->second.regions[1]);
            it = skinned_regions.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Materials -------------------------------------------------------------------------------

std::uint32_t Renderer::Impl::texture_set_index(const TextureSet& set) {
    for (std::size_t i = 0; i < texture_sets.size(); ++i) {
        if (texture_sets[i] == set) {
            return static_cast<std::uint32_t>(i);
        }
    }
    texture_sets.push_back(set);
    return static_cast<std::uint32_t>(texture_sets.size() - 1);
}

std::uint32_t Renderer::Impl::material_index(const Material& m, std::uint32_t extra_flags, TextureSet& set) {
    using namespace gpu::material_flag;
    gpu::Material g;
    g.base_color = v4(m.base_color, m.base_color_alpha);
    g.emissive = v4(m.emissive_factor * m.emissive_strength, m.emissive_nits ? 0.0f : 1.0f);
    g.pbr = {m.metallic, m.roughness, m.normal_strength, m.occlusion_strength};
    g.alpha_clearcoat = {m.alpha_cutoff, m.clearcoat, m.clearcoat_roughness, m.specular};
    g.sheen = v4(m.sheen_color, m.sheen_roughness);
    g.transmission = {m.transmission, m.thickness, std::max(m.ior, 1.0f), 0.0f};
    g.attenuation = v4(m.attenuation_color, m.attenuation_distance);
    g.anisotropy = {m.anisotropy, m.anisotropy_rotation, m.subsurface, m.parallax_scale};
    g.subsurface = v4(m.subsurface_color, 0.0f);
    g.uv = {m.uv_scale_x, m.uv_scale_y, m.uv_offset_x, m.uv_offset_y};
    std::uint32_t flags = extra_flags;
    if (m.alpha_mode == AlphaMode::Mask) flags |= kAlphaMask;
    if (m.alpha_mode == AlphaMode::Blend) flags |= kAlphaBlend;
    if (m.double_sided) flags |= kDoubleSided;
    if (m.unlit) flags |= kUnlit;
    if (m.clearcoat > 0.0f) flags |= kClearcoat;
    if (max_component(m.sheen_color) > 0.0f) flags |= kSheen;
    if (m.transmission > 0.0f) flags |= kTransmission;
    if (m.anisotropy != 0.0f) flags |= kAnisotropy;
    if (m.subsurface > 0.0f) flags |= kSubsurface;
    if (m.parallax_scale > 0.0f && !m.height_texture_path.empty()) flags |= kParallax;
    g.flags = flags;

    struct Slot {
        const std::string* path;
        Usage usage;
    };
    const Slot slots[gpu::binding::kMaterialTextureCount] = {
        {&m.base_color_texture_path, Usage::Color},
        {&m.metallic_roughness_texture_path, Usage::Linear},
        {&m.normal_texture_path, Usage::Normal},
        {&m.emissive_texture_path, Usage::Color},
        {&m.occlusion_texture_path, Usage::Single},
        {&m.height_texture_path, Usage::Single},
        {&m.clearcoat_texture_path, Usage::Dual},
        {&m.sheen_texture_path, Usage::Color},
        {&m.transmission_texture_path, Usage::Single},
    };
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < gpu::binding::kMaterialTextureCount; ++i) {
        if (slots[i].path->empty()) {
            set.ids[i] = textures.default_texture(slots[i].usage);
            continue;
        }
        const GLuint id = textures.get(*slots[i].path, slots[i].usage);
        set.ids[i] = id;
        if (id != textures.default_texture(slots[i].usage) && id != 0) {
            mask |= 1u << i;
        } else if (i == 0) {
            // Base color still loading: keep the factor only.
        }
    }
    g.textures = mask;
    const auto index = static_cast<std::uint32_t>(materials.size());
    materials.push_back(g);
    return index;
}

// --- Terrain -----------------------------------------------------------------------------------

TerrainCache& Renderer::Impl::terrain_for(const Actor& actor, const TerrainComponent& terrain) {
    TerrainCache& cache = terrains[actor.id()];
    cache.last_used = frame_index;
    // Rebuild when any property changes (cheap hash over the fields that
    // shape the mesh).
    std::uint32_t hash = 2166136261u;
    auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 16777619u;
        }
    };
    mix(&terrain.size, sizeof(float));
    mix(&terrain.resolution, sizeof(int));
    mix(&terrain.height, sizeof(float));
    mix(&terrain.seed, sizeof(int));
    mix(&terrain.frequency, sizeof(float));
    mix(&terrain.octaves, sizeof(int));
    mix(&terrain.ridges, sizeof(float));
    mix(&terrain.flatten, sizeof(float));
    mix(&terrain.basin_depth, sizeof(float));
    mix(&terrain.grass_min_height, sizeof(float));
    mix(&terrain.basin_radius, sizeof(float));
    mix(&terrain.basin_x, sizeof(float));
    mix(&terrain.basin_z, sizeof(float));
    mix(&terrain.grass_density, sizeof(float));
    mix(&terrain.grass_height, sizeof(float));
    if (cache.revision != hash) {
        build_terrain(cache, terrain);
        cache.revision = hash;
    }
    return cache;
}

void Renderer::Impl::build_terrain(TerrainCache& cache, const TerrainComponent& terrain) {
    cache.chunks.clear();
    cache.chunk_offsets.clear();
    const int resolution = std::clamp(terrain.resolution, 33, 1025);
    const int cells = resolution - 1;
    const int chunk_cells = 32;
    const int chunks_per_side = std::max(1, (cells + chunk_cells - 1) / chunk_cells);
    const float size = std::max(terrain.size, 1.0f);
    const float cell = size / static_cast<float>(cells);
    const float half = size * 0.5f;
    cache.size = size;

    // Heights on the full grid (+1 border for normals).
    const int grid = resolution + 2;
    std::vector<float> heights(static_cast<std::size_t>(grid) * static_cast<std::size_t>(grid));
    for (int z = 0; z < grid; ++z) {
        for (int x = 0; x < grid; ++x) {
            const float wx = -half + static_cast<float>(x - 1) * cell;
            const float wz = -half + static_cast<float>(z - 1) * cell;
            heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(grid) + static_cast<std::size_t>(x)] = terrain.sample_height(wx, wz);
        }
    }
    auto h = [&](int x, int z) {
        x = std::clamp(x + 1, 0, grid - 1);
        z = std::clamp(z + 1, 0, grid - 1);
        return heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(grid) + static_cast<std::size_t>(x)];
    };

    for (int cz = 0; cz < chunks_per_side; ++cz) {
        for (int cx = 0; cx < chunks_per_side; ++cx) {
            const int x0 = cx * chunk_cells;
            const int z0 = cz * chunk_cells;
            const int nx = std::min(chunk_cells, cells - x0);
            const int nz = std::min(chunk_cells, cells - z0);
            if (nx <= 0 || nz <= 0) {
                continue;
            }
            MeshAsset mesh;
            mesh.name = "Terrain chunk";
            mesh.source = "terrain";
            const int vx = nx + 1;
            const int vz = nz + 1;
            for (int z = 0; z < vz; ++z) {
                for (int x = 0; x < vx; ++x) {
                    const int gx = x0 + x;
                    const int gz = z0 + z;
                    Vertex v;
                    v.position = {-half + static_cast<float>(gx) * cell, h(gx, gz), -half + static_cast<float>(gz) * cell};
                    const float dx = h(gx + 1, gz) - h(gx - 1, gz);
                    const float dz = h(gx, gz + 1) - h(gx, gz - 1);
                    v.normal = normalize(Vec3{-dx, 2.0f * cell, -dz});
                    v.tangent = normalize(Vec3{2.0f * cell, dx, 0.0f});
                    v.tangent_sign = 1.0f;
                    v.uv = {v.position.x * 0.25f, v.position.z * 0.25f};
                    mesh.vertices.push_back(v);
                }
            }
            // Skirt: a copy of the border pushed down hides LOD cracks.
            const std::uint32_t skirt_base = static_cast<std::uint32_t>(mesh.vertices.size());
            std::vector<std::uint32_t> border;
            for (int x = 0; x < vx; ++x) border.push_back(static_cast<std::uint32_t>(x));
            for (int z = 1; z < vz; ++z) border.push_back(static_cast<std::uint32_t>(z * vx + (vx - 1)));
            for (int x = vx - 2; x >= 0; --x) border.push_back(static_cast<std::uint32_t>((vz - 1) * vx + x));
            for (int z = vz - 2; z >= 1; --z) border.push_back(static_cast<std::uint32_t>(z * vx));
            for (std::uint32_t b : border) {
                Vertex v = mesh.vertices[b];
                v.position.y -= cell * 1.5f;
                mesh.vertices.push_back(v);
            }

            // LODs by grid stride (1, 2, 4, 8).
            const float screen_sizes[4] = {1.0f, 0.18f, 0.08f, 0.035f};
            for (int lod = 0; lod < 4; ++lod) {
                const int stride = 1 << lod;
                if (lod > 0 && (nx % stride != 0 || nz % stride != 0)) {
                    break;
                }
                MeshLod entry;
                entry.first_index = static_cast<std::uint32_t>(mesh.indices.size());
                for (int z = 0; z < nz; z += stride) {
                    for (int x = 0; x < nx; x += stride) {
                        const std::uint32_t i00 = static_cast<std::uint32_t>(z * vx + x);
                        const std::uint32_t i10 = static_cast<std::uint32_t>(z * vx + x + stride);
                        const std::uint32_t i01 = static_cast<std::uint32_t>((z + stride) * vx + x);
                        const std::uint32_t i11 = static_cast<std::uint32_t>((z + stride) * vx + x + stride);
                        mesh.indices.insert(mesh.indices.end(), {i00, i01, i10, i10, i01, i11});
                    }
                }
                // Skirt quads along the border at this stride.
                for (std::size_t b = 0; b < border.size(); b += static_cast<std::size_t>(stride)) {
                    const std::size_t next = (b + static_cast<std::size_t>(stride)) % border.size();
                    const std::uint32_t top0 = border[b];
                    const std::uint32_t top1 = border[next];
                    const std::uint32_t bottom0 = skirt_base + static_cast<std::uint32_t>(b);
                    const std::uint32_t bottom1 = skirt_base + static_cast<std::uint32_t>(next);
                    mesh.indices.insert(mesh.indices.end(), {top0, bottom0, top1, top1, bottom0, bottom1});
                }
                entry.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - entry.first_index;
                entry.screen_size = screen_sizes[lod];
                mesh.lods.push_back(entry);
            }
            MeshBuildOptions options;
            options.generate_lods = false;
            cache.chunks.push_back(make_mesh_asset(std::move(mesh), options));
        }
    }

    // Grass blades: jittered points where the ground is flat enough.
    cache.grass_count = 0;
    if (terrain.grass_density > 0.0f) {
        struct Blade {
            Vec4 position_rotation; // xyz, rotation
            Vec4 scale_tint;        // height, width, tint, phase
        };
        std::vector<Blade> blades;
        const float spacing = 1.0f / std::sqrt(terrain.grass_density);
        const int count = static_cast<int>(size / spacing);
        std::uint32_t rng = static_cast<std::uint32_t>(terrain.seed) * 747796405u + 2891336453u;
        auto next = [&rng] {
            rng = rng * 747796405u + 2891336453u;
            std::uint32_t word = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u;
            return static_cast<float>((word >> 22u) ^ word) / 4294967296.0f;
        };
        const std::size_t max_blades = 400000;
        for (int z = 0; z < count && blades.size() < max_blades; ++z) {
            for (int x = 0; x < count && blades.size() < max_blades; ++x) {
                const float px = -half + (static_cast<float>(x) + next()) * spacing;
                const float pz = -half + (static_cast<float>(z) + next()) * spacing;
                const float height = terrain.sample_height(px, pz);
                const float dx = terrain.sample_height(px + 0.5f, pz) - terrain.sample_height(px - 0.5f, pz);
                const float dz = terrain.sample_height(px, pz + 0.5f) - terrain.sample_height(px, pz - 0.5f);
                const float slope = std::sqrt(dx * dx + dz * dz);
                if (slope > 0.6f || height < terrain.grass_min_height || height > terrain.height * 0.55f) {
                    continue;
                }
                Blade blade;
                blade.position_rotation = {px, height, pz, next() * 6.2831853f};
                blade.scale_tint = {terrain.grass_height * (0.6f + next() * 0.8f), 0.04f + next() * 0.03f, next(), next() * 6.2831853f};
                blades.push_back(blade);
            }
        }
        if (!blades.empty()) {
            cache.grass = gfx::Buffer(blades.size() * sizeof(Blade), 0, blades.data(), "Terrain.Grass");
            cache.grass_count = static_cast<std::uint32_t>(blades.size());
        }
    }
    log_info(std::format("Terrain built: {} chunks, {} grass blades", cache.chunks.size(), cache.grass_count));
}

// --- Extraction --------------------------------------------------------------------------------

void Renderer::Impl::extract_scene() {
    FrameState& f = frame;
    const Scene& scene = *f.scene;
    const RenderView& view = *f.view;
    const RenderPipelineSettings& settings = *f.settings;

    std::vector<const Actor*> actors;
    actors.reserve(scene.actor_count());
    for (ActorId root : scene.root_actors()) {
        collect_active(scene, root, actors);
    }

    materials.clear();
    texture_sets.clear();
    joint_palette.clear();
    skin_dispatches.clear();
    f.any_skinned = false;
    instances.clear();
    instance_info.clear();
    batches.clear();

    struct Pending {
        gpu::Instance instance;
        InstanceInfo info;
        Batch batch_key;
        float depth = 0.0f;
    };
    std::vector<Pending> pending;
    pending.reserve(actors.size() * 2);

    auto add_instance = [&](const Actor& actor, const MeshHandle& mesh_handle, const Material& material, std::uint32_t part, std::uint64_t key, const Mat4& model,
                            bool cast_shadows, std::uint32_t extra_instance_flags, std::uint32_t extra_material_flags) {
        if (!mesh_handle || mesh_handle->indices.empty()) {
            return;
        }
        const MeshAsset& mesh = *mesh_handle;
        PooledMesh& pooled = ensure_mesh(mesh);
        Pending p;
        TextureSet set;
        p.instance.material = material_index(material, extra_material_flags, set);
        const gpu::Material& gm = materials[p.instance.material];
        p.instance.model = model;
        const auto prev = previous_models.find(key);
        p.instance.prev_model = prev != previous_models.end() ? prev->second : model;
        const Mat4 nm = normal_matrix(model);
        p.instance.normal0 = {nm.m[0], nm.m[1], nm.m[2], 0.0f};
        p.instance.normal1 = {nm.m[4], nm.m[5], nm.m[6], 0.0f};
        p.instance.normal2 = {nm.m[8], nm.m[9], nm.m[10], 0.0f};
        const Aabb world_bounds = transform_aabb(mesh.bounds, model);
        const Vec3 center = world_bounds.center();
        p.instance.bounds = v4(center, std::max(length(world_bounds.extents()), 0.001f));
        p.instance.mesh = pooled.slot;
        p.instance.actor = actor.id();
        p.instance.base_vertex = static_cast<std::int32_t>(pooled.vertex_range.offset);
        p.instance.prev_base_vertex = p.instance.base_vertex;
        bool skinned_instance = false;
        if (mesh.skinned()) {
            const auto* animator = actor.find_component<AnimatorComponent>();
            if (animator != nullptr && animator->skeleton == mesh.skeleton && !animator->joint_matrices.empty()) {
                setup_skinned_instance(key, mesh, pooled, *animator, p.instance);
                skinned_instance = true;
                f.any_skinned = true;
            }
        }
        std::uint32_t flags = extra_instance_flags;
        if (cast_shadows) flags |= gpu::instance_flag::kCastShadows;
        if (actor.id() == view.selected) flags |= gpu::instance_flag::kSelected;
        if (material.double_sided) flags |= gpu::instance_flag::kDoubleSided;
        if (skinned_instance) flags |= gpu::instance_flag::kSkinned;
        p.instance.flags = flags;
        const auto prev_slot = previous_slots.find(key);
        p.instance.prev_slot = prev_slot != previous_slots.end() ? prev_slot->second : 0xFFFFFFFFu;

        p.info.key = key;
        p.info.mesh = &mesh;
        p.info.mesh_slot = pooled.slot;
        p.info.actor = actor.id();
        p.info.lod_count = static_cast<std::uint32_t>(std::max<std::size_t>(mesh.lods.size(), 1));
        p.depth = dot(center - f.camera_position, f.camera_forward);
        p.info.view_depth = p.depth;

        Batch key_batch;
        key_batch.double_sided = material.double_sided;
        key_batch.texture_set = texture_set_index(set);
        key_batch.water = (gm.flags & gpu::material_flag::kWater) != 0;
        if (key_batch.water || material.transmission > 0.0f) {
            key_batch.category = BatchCategory::Refractive;
            f.has_refractive = true;
            f.has_water = f.has_water || key_batch.water;
        } else if (material.alpha_mode == AlphaMode::Blend) {
            key_batch.category = BatchCategory::Transparent;
            f.has_transparent = true;
        } else if (material.alpha_mode == AlphaMode::Mask) {
            key_batch.category = BatchCategory::Masked;
        } else {
            key_batch.category = BatchCategory::Opaque;
        }
        p.batch_key = key_batch;
        pending.push_back(std::move(p));
        (void)part;
    };

    for (const Actor* actor_ptr : actors) {
        const Actor& actor = *actor_ptr;
        const Mat4& world = actor.world_matrix();
        if (const auto* mesh = actor.find_component<StaticMeshComponent>(); mesh != nullptr && mesh->visible) {
            for (std::uint32_t i = 0; i < mesh->parts.size(); ++i) {
                const MeshPart& part = mesh->parts[i];
                // Clear (transmissive) parts let light through: no opaque shadow.
                add_instance(actor, part.mesh, part.material, i, instance_key(actor.id(), i), world, mesh->cast_shadows && part.material.transmission < 0.5f, 0, 0);
            }
        }
        if (const auto* terrain = actor.find_component<TerrainComponent>()) {
            TerrainCache& cache = terrain_for(actor, *terrain);
            cache.material.base_color = {1.0f, 1.0f, 1.0f};
            cache.material.roughness = 1.0f;
            cache.material.metallic = 0.0f;
            for (std::uint32_t i = 0; i < cache.chunks.size(); ++i) {
                add_instance(actor, cache.chunks[i], cache.material, i, instance_key(actor.id(), i, 1), world, true, gpu::instance_flag::kTerrain,
                    gpu::material_flag::kTerrain);
            }
        }
        if (const auto* water = actor.find_component<WaterComponent>()) {
            if (!water_grid) {
                MeshAsset grid;
                grid.name = "Water grid";
                grid.source = "water";
                const int n = 128;
                for (int z = 0; z <= n; ++z) {
                    for (int x = 0; x <= n; ++x) {
                        Vertex v;
                        v.position = {static_cast<float>(x) / n - 0.5f, 0.0f, static_cast<float>(z) / n - 0.5f};
                        v.normal = {0.0f, 1.0f, 0.0f};
                        v.tangent = {1.0f, 0.0f, 0.0f};
                        v.uv = {static_cast<float>(x) / n, static_cast<float>(z) / n};
                        grid.vertices.push_back(v);
                    }
                }
                for (int z = 0; z < n; ++z) {
                    for (int x = 0; x < n; ++x) {
                        const std::uint32_t i00 = static_cast<std::uint32_t>(z * (n + 1) + x);
                        const std::uint32_t i10 = i00 + 1;
                        const std::uint32_t i01 = i00 + static_cast<std::uint32_t>(n + 1);
                        const std::uint32_t i11 = i01 + 1;
                        grid.indices.insert(grid.indices.end(), {i00, i01, i10, i10, i01, i11});
                    }
                }
                MeshBuildOptions options;
                options.generate_lods = false;
                water_grid = make_mesh_asset(std::move(grid), options);
            }
            Material material;
            material.base_color = water->shallow_color;
            material.roughness = water->roughness;
            material.metallic = 0.0f;
            material.transmission = 1.0f;
            material.attenuation_color = water->deep_color;
            material.attenuation_distance = water->clarity;
            material.sheen_color = water->deep_color; // packed for the water shader
            material.thickness = water->foam;
            material.anisotropy = water->wave_height;
            material.anisotropy_rotation = water->wind_direction;
            material.subsurface = water->steepness;
            material.parallax_scale = water->wavelength;
            material.clearcoat = water->wave_speed;
            material.ior = 1.333f;
            const Mat4 model = world * Mat4::scale({water->size_x, 1.0f, water->size_z});
            add_instance(actor, water_grid, material, 0, instance_key(actor.id(), 0, 2), model, false, gpu::instance_flag::kNoMotion, gpu::material_flag::kWater);
        }
    }

    // Sort into batches: category, cull mode, texture set; transparent
    // back to front (sorted mode needs it; WBOIT doesn't care).
    const bool sorted_transparency = settings.transparency.mode == TransparencyMode::Sorted;
    std::stable_sort(pending.begin(), pending.end(), [sorted_transparency](const Pending& a, const Pending& b) {
        if (a.batch_key.category != b.batch_key.category) {
            return a.batch_key.category < b.batch_key.category;
        }
        if (a.batch_key.category == BatchCategory::Transparent && sorted_transparency) {
            return a.depth > b.depth;
        }
        if (a.batch_key.water != b.batch_key.water) {
            return a.batch_key.water < b.batch_key.water;
        }
        if (a.batch_key.double_sided != b.batch_key.double_sided) {
            return a.batch_key.double_sided < b.batch_key.double_sided;
        }
        if (a.batch_key.texture_set != b.batch_key.texture_set) {
            return a.batch_key.texture_set < b.batch_key.texture_set;
        }
        return a.depth < b.depth; // front to back within a batch
    });

    previous_slots.clear();
    std::unordered_map<std::uint64_t, Mat4> models;
    models.reserve(pending.size());
    instances.reserve(pending.size());
    instance_info.reserve(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        Pending& p = pending[i];
        const auto slot = static_cast<std::uint32_t>(i);
        const bool separate = p.batch_key.category == BatchCategory::Transparent && sorted_transparency;
        if (batches.empty() || separate || batches.back().category != p.batch_key.category || batches.back().double_sided != p.batch_key.double_sided ||
            batches.back().texture_set != p.batch_key.texture_set || batches.back().water != p.batch_key.water) {
            Batch batch = p.batch_key;
            batch.first = slot;
            batch.count = 0;
            batches.push_back(batch);
        }
        batches.back().count++;
        p.instance.batch = static_cast<std::uint32_t>(batches.size() - 1);
        instances.push_back(p.instance);
        instance_info.push_back(p.info);
        previous_slots[p.info.key] = slot;
        models[p.info.key] = p.instance.model;
    }
    previous_models = std::move(models);
    f.instance_count = static_cast<std::uint32_t>(instances.size());
    stats.instances = static_cast<int>(instances.size());
    stats.batches = static_cast<int>(batches.size());

    extract_lights();
    extract_probes_and_volumes();
    extract_decals();
    extract_effects(actors);
    upload_mesh_infos();
}

void Renderer::Impl::extract_lights() {
    FrameState& f = frame;
    const Scene& scene = *f.scene;
    std::vector<const Actor*> actors;
    for (ActorId root : scene.root_actors()) {
        collect_active(scene, root, actors);
    }
    lights.clear();
    std::vector<gpu::Light> local;
    const int max_lights = std::max(f.settings->performance.max_lights, 1);
    for (const Actor* actor : actors) {
        const auto* light = actor->find_component<LightComponent>();
        if (light == nullptr || !light->enabled || light->intensity <= 0.0f) {
            continue;
        }
        const Mat4& world = actor->world_matrix();
        Vec3 color = light->color;
        if (light->use_temperature) {
            color = component_mul(color, kelvin_rgb(light->temperature));
        }
        gpu::Light g;
        const Vec3 position = world.translation_part();
        const Vec3 direction = normalize(transform_vector(world, {0.0f, -1.0f, 0.0f}));
        g.position_range = v4(position, std::max(light->range, 0.01f));
        g.spot_shadow = {0.0f, 1.0f, -1.0f, light->volumetric};
        switch (light->light_kind) {
        case LightKind::Directional: {
            g.direction_type = v4(direction, 0.0f);
            const float angular_radius = light->source_size * 0.5f * kDegToRad;
            g.color = v4(color * light->intensity, angular_radius);
            if (f.sun_light_index < 0 && light->cast_shadows) {
                f.sun_light_index = static_cast<int>(lights.size());
                f.sun_direction = -direction;
                f.sun_illuminance = color * light->intensity;
                f.sun_angular_radius = angular_radius;
            } else if (f.sun_light_index < 0 && lights.empty()) {
                // No shadow caster: the first directional light still drives the sky.
                f.sun_direction = -direction;
                f.sun_illuminance = color * light->intensity;
                f.sun_angular_radius = angular_radius;
            }
            lights.push_back(g);
            break;
        }
        case LightKind::Point:
        case LightKind::Spot: {
            const bool spot = light->light_kind == LightKind::Spot;
            g.direction_type = v4(direction, spot ? 2.0f : 1.0f);
            const float candela = light->intensity / (4.0f * kPi);
            g.color = v4(color * candela, std::max(light->source_size, 0.0f));
            if (spot) {
                const float outer = std::clamp(light->spot_angle_radians * 0.5f, 0.01f, 1.55f);
                const float inner = outer * (1.0f - std::clamp(light->spot_softness, 0.01f, 1.0f));
                const float cos_outer = std::cos(outer);
                const float cos_inner = std::cos(inner);
                const float scale = 1.0f / std::max(cos_inner - cos_outer, 1e-4f);
                g.spot_shadow.x = scale;
                g.spot_shadow.y = -cos_outer * scale;
            }
            // Shadow casting is decided in allocate_atlas (z = -1 here).
            g.spot_shadow.z = light->cast_shadows ? -2.0f : -1.0f;
            if (static_cast<int>(local.size()) < max_lights) {
                local.push_back(g);
            }
            break;
        }
        }
    }
    f.directional_count = static_cast<int>(lights.size());
    lights.insert(lights.end(), local.begin(), local.end());
    f.light_count = static_cast<int>(lights.size());
    stats.lights = f.light_count;
}

void Renderer::Impl::extract_probes_and_volumes() {
    FrameState& f = frame;
    const Scene& scene = *f.scene;
    std::vector<const Actor*> actors;
    for (ActorId root : scene.root_actors()) {
        collect_active(scene, root, actors);
    }
    probes.clear();
    volumes.clear();
    std::vector<ActorId> seen_probes;
    std::vector<ActorId> seen_volumes;
    for (const Actor* actor : actors) {
        const Vec3 position = actor->world_position();
        if (const auto* probe = actor->find_component<ReflectionProbeComponent>()) {
            seen_probes.push_back(actor->id());
            auto it = std::find_if(probe_records.begin(), probe_records.end(), [&](const ProbeRecord& r) { return r.actor == actor->id(); });
            if (it == probe_records.end()) {
                // Assign a free layer (1..kMaxProbes).
                int layer = -1;
                for (int candidate = 1; candidate <= kMaxProbes; ++candidate) {
                    if (std::none_of(probe_records.begin(), probe_records.end(), [candidate](const ProbeRecord& r) { return r.layer == candidate; })) {
                        layer = candidate;
                        break;
                    }
                }
                if (layer < 0) {
                    continue; // out of probe slots
                }
                ProbeRecord record;
                record.actor = actor->id();
                record.layer = layer;
                probe_records.push_back(record);
                it = probe_records.end() - 1;
            }
            // Auto re-bake when the probe moves or is edited.
            if (!it->baked || length(it->baked_position - position) > 0.01f || it->revision != probe->revision) {
                const bool queued = std::any_of(bake_queue.begin(), bake_queue.end(), [&](const BakeJob& j) { return j.kind == BakeJob::Kind::Probe && j.actor == actor->id(); });
                if (!queued) {
                    bake_queue.push_back({BakeJob::Kind::Probe, actor->id(), 0, 0});
                    bake_total += 6;
                }
                it->revision = probe->revision;
                it->baked_position = position;
            }
            if (it->baked) {
                gpu::Probe g;
                g.position = v4(position, static_cast<float>(it->layer));
                g.box_min = v4(position - probe->extents, probe->blend_distance);
                g.box_max = v4(position + probe->extents, probe->intensity);
                g.params = {probe->box_projection ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
                probes.push_back(g);
            }
        }
        if (const auto* volume = actor->find_component<IrradianceVolumeComponent>()) {
            seen_volumes.push_back(actor->id());
            auto it = std::find_if(volume_records.begin(), volume_records.end(), [&](const VolumeRecord& r) { return r.actor == actor->id(); });
            if (it == volume_records.end()) {
                VolumeRecord record;
                record.actor = actor->id();
                volume_records.push_back(record);
                it = volume_records.end() - 1;
            }
            const bool resized = it->nx != volume->probes_x || it->ny != volume->probes_y || it->nz != volume->probes_z;
            if (!it->baked || resized || length(it->baked_position - position) > 0.01f || it->revision != volume->revision) {
                const bool queued = std::any_of(bake_queue.begin(), bake_queue.end(), [&](const BakeJob& j) { return j.kind == BakeJob::Kind::Volume && j.actor == actor->id(); });
                if (!queued) {
                    bake_queue.push_back({BakeJob::Kind::Volume, actor->id(), 0, 0});
                    bake_total += volume->probes_x * volume->probes_y * volume->probes_z * std::max(volume->bounces, 1);
                }
                it->revision = volume->revision;
                it->baked_position = position;
                if (resized) {
                    it->baked = false;
                }
            }
            if (it->baked) {
                gpu::Volume g;
                g.box_min = v4(position - volume->extents, static_cast<float>(it->z_offset));
                g.box_max = v4(position + volume->extents, volume->intensity);
                g.resolution = {static_cast<float>(it->nx), static_cast<float>(it->ny), static_cast<float>(it->nz), static_cast<float>(volume_atlas_depth)};
                g.params = {volume->normal_bias, volume->blend_distance, 0.0f, 0.0f}; // box_max.w is the intensity
                volumes.push_back(g);
            }
        }
    }
    // Forget records of deleted actors.
    probe_records.erase(std::remove_if(probe_records.begin(), probe_records.end(),
                            [&](const ProbeRecord& r) { return std::find(seen_probes.begin(), seen_probes.end(), r.actor) == seen_probes.end(); }),
        probe_records.end());
    volume_records.erase(std::remove_if(volume_records.begin(), volume_records.end(),
                             [&](const VolumeRecord& r) { return std::find(seen_volumes.begin(), seen_volumes.end(), r.actor) == seen_volumes.end(); }),
        volume_records.end());
    if (bake_requested) {
        bake_requested = false;
        for (ProbeRecord& r : probe_records) {
            r.baked = r.baked; // keep showing the old data while re-baking
            if (std::none_of(bake_queue.begin(), bake_queue.end(), [&](const BakeJob& j) { return j.actor == r.actor; })) {
                bake_queue.push_back({BakeJob::Kind::Probe, r.actor, 0, 0});
                bake_total += 6;
            }
        }
        for (VolumeRecord& r : volume_records) {
            if (std::none_of(bake_queue.begin(), bake_queue.end(), [&](const BakeJob& j) { return j.actor == r.actor && j.kind == BakeJob::Kind::Volume; })) {
                bake_queue.push_back({BakeJob::Kind::Volume, r.actor, 0, 0});
                if (const Actor* actor = scene.find(r.actor)) {
                    if (const auto* volume = actor->find_component<IrradianceVolumeComponent>()) {
                        bake_total += volume->probes_x * volume->probes_y * volume->probes_z * std::max(volume->bounces, 1);
                    }
                }
            }
        }
    }
    if (bake_queue.empty()) {
        bake_total = 0;
        bake_done = 0;
    }
}

void Renderer::Impl::extract_decals() {
    FrameState& f = frame;
    const Scene& scene = *f.scene;
    std::vector<const Actor*> actors;
    for (ActorId root : scene.root_actors()) {
        collect_active(scene, root, actors);
    }
    decals.clear();
    auto layer_for = [this](const std::string& path, bool normal) -> int {
        if (path.empty()) {
            if (normal) {
                return -1;
            }
            // No texture: a built-in procedural splat (soft, ragged disc).
            const auto it = decal_layers.find("builtin:splat");
            if (it != decal_layers.end()) {
                return it->second;
            }
            if (decal_layer_count >= kMaxDecalLayers) {
                return -1;
            }
            const int layer = decal_layer_count++;
            const int n = kDecalLayerSize;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(n * n * 4));
            auto hash = [](int x, int y) {
                std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
            };
            auto value_noise = [&hash](float x, float y) {
                const int xi = static_cast<int>(std::floor(x));
                const int yi = static_cast<int>(std::floor(y));
                const float fx = x - static_cast<float>(xi);
                const float fy = y - static_cast<float>(yi);
                const float u = fx * fx * (3.0f - 2.0f * fx);
                const float v = fy * fy * (3.0f - 2.0f * fy);
                const float a = hash(xi, yi) + (hash(xi + 1, yi) - hash(xi, yi)) * u;
                const float b = hash(xi, yi + 1) + (hash(xi + 1, yi + 1) - hash(xi, yi + 1)) * u;
                return a + (b - a) * v;
            };
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    const float px = (static_cast<float>(x) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
                    const float py = (static_cast<float>(y) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
                    float noise = 0.0f;
                    float amplitude = 0.5f;
                    float frequency = 4.0f;
                    for (int o = 0; o < 5; ++o) {
                        noise += value_noise((px + 1.0f) * frequency, (py + 1.0f) * frequency) * amplitude;
                        amplitude *= 0.5f;
                        frequency *= 2.0f;
                    }
                    const float r = std::sqrt(px * px + py * py) + (noise - 0.5f) * 0.45f;
                    const float alpha = std::clamp((0.85f - r) * 4.0f, 0.0f, 1.0f) * (0.75f + 0.25f * noise);
                    const auto shade = static_cast<std::uint8_t>(std::clamp(200.0f + noise * 55.0f, 0.0f, 255.0f));
                    std::uint8_t* p = &pixels[static_cast<std::size_t>((y * n + x) * 4)];
                    p[0] = p[1] = p[2] = shade;
                    p[3] = static_cast<std::uint8_t>(alpha * 255.0f);
                }
            }
            glTextureSubImage3D(decal_array.id(), 0, 0, 0, layer, n, n, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glGenerateTextureMipmap(decal_array.id());
            decal_layers["builtin:splat"] = layer;
            return layer;
        }
        const std::string key = path + (normal ? "|n" : "|c");
        const auto it = decal_layers.find(key);
        if (it != decal_layers.end()) {
            return it->second;
        }
        if (decal_layer_count >= kMaxDecalLayers) {
            return -1;
        }
        const GLuint source = textures.load_now(path, normal ? Usage::Normal : Usage::Color);
        if (source == 0) {
            decal_layers[key] = -1;
            return -1;
        }
        const int layer = decal_layer_count++;
        // Resample into the array layer with a fullscreen blit pass.
        const ProgramId program = shaders.graphics("fullscreen.vert", "blit.frag");
        const GLuint fbo = framebuffers.get(std::array<gfx::Attachment, 1>{gfx::Attachment{decal_array.id(), 0, layer}}, {});
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kDecalLayerSize, kDecalLayerSize);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        shaders.use(program);
        glUniform1i(shaders.uniform(program, "u_mode"), normal ? 0 : 1);
        gfx::bind_texture(0, source, gfx::SamplerKind::LinearMipClamp);
        gfx::draw_fullscreen_triangle();
        glGenerateTextureMipmap(decal_array.id());
        glDeleteTextures(1, &source);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        decal_layers[key] = layer;
        return layer;
    };
    for (const Actor* actor : actors) {
        const auto* decal = actor->find_component<DecalComponent>();
        if (decal == nullptr || decal->opacity <= 0.0f) {
            continue;
        }
        const int albedo = layer_for(decal->albedo_texture_path, false);
        if (albedo < 0) {
            continue;
        }
        const int normal = layer_for(decal->normal_texture_path, true);
        const Mat4& world = actor->world_matrix();
        gpu::Decal g;
        g.world_to_decal = inverse(world);
        g.color = v4(decal->color, decal->opacity);
        g.params = {static_cast<float>(albedo), static_cast<float>(normal), decal->normal_strength, decal->override_roughness ? decal->roughness : -1.0f};
        g.params2 = {decal->angle_fade, -1.0f, 0.0f, 0.0f};
        const Aabb bounds = transform_aabb({{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}, world);
        g.bounds = v4(bounds.center(), length(bounds.extents()));
        decals.push_back(g);
    }
}

// --- Upload ----------------------------------------------------------------------------------

void Renderer::Impl::upload_scene_buffers() {
    FrameState& f = frame;
    ensure_draw_ids(std::max<std::size_t>(instances.size(), 1));
    auto upload = [](gfx::Buffer& buffer, const void* data, std::size_t bytes, const char* label) {
        const std::size_t size = std::max<std::size_t>(bytes, 64);
        if (size > buffer.size()) {
            buffer.reserve(size, GL_DYNAMIC_STORAGE_BIT, label);
        }
        if (bytes > 0) {
            buffer.upload(data, bytes);
        }
    };
    upload(instance_buffer, instances.data(), instances.size() * sizeof(gpu::Instance), "Instances");
    upload(material_buffer, materials.data(), materials.size() * sizeof(gpu::Material), "Materials");
    upload(light_buffer, lights.data(), lights.size() * sizeof(gpu::Light), "Lights");
    upload(shadow_view_buffer, shadow_view_data.data(), shadow_view_data.size() * sizeof(gpu::ShadowView), "ShadowViews");
    upload(probe_buffer, probes.data(), probes.size() * sizeof(gpu::Probe), "Probes");
    upload(volume_buffer, volumes.data(), volumes.size() * sizeof(gpu::Volume), "Volumes");
    upload(decal_buffer, decals.data(), decals.size() * sizeof(gpu::Decal), "Decals");

    // Commands: main union / phase 1 / phase 2, then one block per shadow view.
    f.command_stride = std::max<std::uint32_t>(f.instance_count, 1);
    f.main_union_offset = 0;
    f.main_phase1_offset = f.command_stride;
    f.main_phase2_offset = f.command_stride * 2;
    for (std::size_t i = 0; i < f.shadow_views.size(); ++i) {
        f.shadow_views[i].command_offset = f.command_stride * static_cast<std::uint32_t>(3 + i);
    }
    f.view_count = static_cast<std::uint32_t>(3 + f.shadow_views.size());
    const std::size_t command_bytes = static_cast<std::size_t>(f.command_stride) * f.view_count * sizeof(gpu::DrawCommand);
    if (command_bytes > command_buffer.size()) {
        command_buffer.reserve(command_bytes, GL_DYNAMIC_STORAGE_BIT, "DrawCommands");
    }
    const std::size_t visibility_bytes = std::max<std::size_t>(instances.size(), 1) * sizeof(std::uint32_t);
    for (gfx::Buffer& buffer : visibility) {
        if (visibility_bytes > buffer.size()) {
            // Keep both the same size; contents restart (everything is
            // treated as newly visible for a frame).
            buffer.reserve(visibility_bytes * 2, GL_DYNAMIC_STORAGE_BIT, "Visibility");
            buffer.clear_u32(0);
        }
    }
    if (visibility_bytes > instance_lods.size()) {
        instance_lods.reserve(visibility_bytes * 2, GL_DYNAMIC_STORAGE_BIT, "InstanceLods");
        instance_lods.clear_u32(0);
    }
    const std::size_t clusters = static_cast<std::size_t>(16 * 16) * kClusterSlices;
    const std::size_t grid_bytes = clusters * sizeof(std::uint32_t) * 4;
    if (grid_bytes > cluster_grid.size()) {
        cluster_grid.reserve(grid_bytes, GL_DYNAMIC_STORAGE_BIT, "ClusterGrid");
    }
    const std::size_t item_bytes = (clusters * kClusterItemsPerCluster + 1) * sizeof(std::uint32_t);
    if (item_bytes > cluster_items.size()) {
        cluster_items.reserve(item_bytes, GL_DYNAMIC_STORAGE_BIT, "ClusterItems");
    }

    if (!f.gpu_driven) {
        // CPU culling path: fill the same command layout on the CPU.
        cpu_commands.assign(static_cast<std::size_t>(f.command_stride) * f.view_count, gpu::DrawCommand{});
        cpu_cull(f.main_union_offset, f.frustum, false);
        for (const ShadowViewInfo& view : f.shadow_views) {
            cpu_cull(view.command_offset, view.frustum, true);
        }
        command_buffer.upload(cpu_commands.data(), cpu_commands.size() * sizeof(gpu::DrawCommand));
    }
}

} // namespace Hollow
