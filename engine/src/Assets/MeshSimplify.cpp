// Implementation of MeshSimplify.hpp.
#include "Hollow/Assets/MeshSimplify.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace Hollow {
namespace {

struct Quadric {
    // Symmetric 4x4: a2 ab ac ad b2 bc bd c2 cd d2
    double q[10]{};

    void add_plane(double a, double b, double c, double d, double w) {
        q[0] += w * a * a; q[1] += w * a * b; q[2] += w * a * c; q[3] += w * a * d;
        q[4] += w * b * b; q[5] += w * b * c; q[6] += w * b * d;
        q[7] += w * c * c; q[8] += w * c * d;
        q[9] += w * d * d;
    }
    void add(const Quadric& o) {
        for (int i = 0; i < 10; ++i) {
            q[i] += o.q[i];
        }
    }
    double error(const Vec3& p) const {
        const double x = p.x, y = p.y, z = p.z;
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x
             + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y
             + q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

struct Collapse {
    double cost;
    std::uint32_t from; // canonical
    std::uint32_t to;   // canonical
    std::uint32_t version;
    bool operator>(const Collapse& o) const { return cost > o.cost; }
};

struct PositionKey {
    float x, y, z;
    bool operator==(const PositionKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct PositionHash {
    std::size_t operator()(const PositionKey& k) const {
        std::uint32_t h[3];
        std::memcpy(&h[0], &k.x, 4);
        std::memcpy(&h[1], &k.y, 4);
        std::memcpy(&h[2], &k.z, 4);
        return (h[0] * 73856093u) ^ (h[1] * 19349663u) ^ (h[2] * 83492791u);
    }
};

} // namespace

SimplifyResult simplify_mesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& input, std::size_t target_triangles, float max_error) {
    SimplifyResult result;
    const std::size_t vertex_count = vertices.size();
    std::size_t triangle_count = input.size() / 3;
    if (triangle_count <= target_triangles || vertex_count == 0) {
        result.indices = input;
        return result;
    }

    // Weld by position: canonical vertex per position.
    std::vector<std::uint32_t> canon(vertex_count);
    std::unordered_map<PositionKey, std::uint32_t, PositionHash> weld;
    weld.reserve(vertex_count);
    std::vector<std::uint32_t> canonical_list; // canonical id -> representative original
    std::vector<std::uint32_t> original_count;
    for (std::uint32_t i = 0; i < vertex_count; ++i) {
        const Vec3& p = vertices[i].position;
        const auto [it, inserted] = weld.try_emplace({p.x, p.y, p.z}, static_cast<std::uint32_t>(canonical_list.size()));
        if (inserted) {
            canonical_list.push_back(i);
            original_count.push_back(0);
        }
        canon[i] = it->second;
        original_count[it->second]++;
    }
    const std::size_t canonical_count = canonical_list.size();

    Aabb bounds = Aabb::empty();
    for (const Vertex& v : vertices) {
        bounds.expand(v.position);
    }
    const double radius = std::max(0.5 * static_cast<double>(length(bounds.max - bounds.min)), 1e-6);
    const double max_cost = (max_error * radius) * (max_error * radius);

    std::vector<std::uint32_t> tris(input);
    std::vector<bool> tri_alive(triangle_count, true);
    std::vector<std::vector<std::uint32_t>> vertex_tris(canonical_count);
    std::vector<Quadric> quadrics(canonical_count);
    for (std::uint32_t t = 0; t < triangle_count; ++t) {
        const std::uint32_t a = canon[tris[t * 3]], b = canon[tris[t * 3 + 1]], c = canon[tris[t * 3 + 2]];
        if (a == b || b == c || a == c) {
            tri_alive[t] = false;
            continue;
        }
        const Vec3 pa = vertices[tris[t * 3]].position;
        const Vec3 pb = vertices[tris[t * 3 + 1]].position;
        const Vec3 pc = vertices[tris[t * 3 + 2]].position;
        Vec3 n = cross(pb - pa, pc - pa);
        const float area = length(n);
        if (area <= 1e-12f) {
            vertex_tris[a].push_back(t);
            vertex_tris[b].push_back(t);
            vertex_tris[c].push_back(t);
            continue;
        }
        n = n / area;
        const double d = -static_cast<double>(dot(n, pa));
        for (std::uint32_t v : {a, b, c}) {
            quadrics[v].add_plane(n.x, n.y, n.z, d, area);
            vertex_tris[v].push_back(t);
        }
    }

    // Border detection: an edge used by exactly one triangle.
    std::vector<bool> locked(canonical_count, false);
    {
        std::unordered_map<std::uint64_t, int> edge_use;
        edge_use.reserve(triangle_count * 3);
        for (std::uint32_t t = 0; t < triangle_count; ++t) {
            if (!tri_alive[t]) {
                continue;
            }
            for (int e = 0; e < 3; ++e) {
                std::uint32_t a = canon[tris[t * 3 + e]];
                std::uint32_t b = canon[tris[t * 3 + (e + 1) % 3]];
                if (a > b) std::swap(a, b);
                edge_use[(static_cast<std::uint64_t>(a) << 32) | b]++;
            }
        }
        for (const auto& [key, count] : edge_use) {
            if (count == 1) {
                locked[static_cast<std::size_t>(key >> 32)] = true;
                locked[static_cast<std::size_t>(key & 0xFFFFFFFFu)] = true;
            }
        }
    }
    // Attribute seams: several distinct original vertices at one position.
    for (std::size_t c = 0; c < canonical_count; ++c) {
        if (original_count[c] > 1) {
            locked[c] = true;
        }
    }

    std::vector<std::uint32_t> version(canonical_count, 0);
    std::vector<bool> removed(canonical_count, false);
    std::priority_queue<Collapse, std::vector<Collapse>, std::greater<Collapse>> heap;

    auto position_of = [&](std::uint32_t c) { return vertices[canonical_list[c]].position; };
    auto push_edges_of = [&](std::uint32_t c) {
        for (std::uint32_t t : vertex_tris[c]) {
            if (!tri_alive[t]) {
                continue;
            }
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t other = canon[tris[t * 3 + e]];
                if (other == c) {
                    continue;
                }
                // c -> other and other -> c
                if (!locked[c]) {
                    Quadric q = quadrics[c];
                    q.add(quadrics[other]);
                    heap.push({q.error(position_of(other)), c, other, version[c]});
                }
                if (!locked[other]) {
                    Quadric q = quadrics[other];
                    q.add(quadrics[c]);
                    heap.push({q.error(position_of(c)), other, c, version[other]});
                }
            }
        }
    };
    for (std::uint32_t c = 0; c < canonical_count; ++c) {
        if (!locked[c]) {
            push_edges_of(c);
        }
    }

    std::size_t alive = static_cast<std::size_t>(std::count(tri_alive.begin(), tri_alive.end(), true));
    double worst = 0.0;
    while (alive > target_triangles && !heap.empty()) {
        const Collapse top = heap.top();
        heap.pop();
        if (removed[top.from] || removed[top.to] || top.version != version[top.from]) {
            continue;
        }
        if (top.cost > max_cost) {
            break;
        }
        const Vec3 target = position_of(top.to);
        // Validity: moving `from` onto `to` mustn't flip any triangle.
        bool valid = true;
        for (std::uint32_t t : vertex_tris[top.from]) {
            if (!tri_alive[t]) {
                continue;
            }
            std::uint32_t c[3] = {canon[tris[t * 3]], canon[tris[t * 3 + 1]], canon[tris[t * 3 + 2]]};
            if (c[0] == top.to || c[1] == top.to || c[2] == top.to) {
                continue; // will collapse away
            }
            Vec3 p[3];
            Vec3 q[3];
            for (int i = 0; i < 3; ++i) {
                p[i] = position_of(c[i]);
                q[i] = c[i] == top.from ? target : p[i];
            }
            const Vec3 n0 = cross(p[1] - p[0], p[2] - p[0]);
            const Vec3 n1 = cross(q[1] - q[0], q[2] - q[0]);
            const float l0 = length(n0);
            const float l1 = length(n1);
            if (l1 <= 1e-12f || (l0 > 1e-12f && dot(n0, n1) < 0.2f * l0 * l1)) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            version[top.from]++; // requeued when a neighbor changes
            continue;
        }
        // The original vertex at `to` that `from`'s original maps onto:
        // the one sharing a triangle with it (same attribute island).
        const std::uint32_t from_original = canonical_list[top.from];
        std::uint32_t to_original = canonical_list[top.to];
        for (std::uint32_t t : vertex_tris[top.from]) {
            if (!tri_alive[t]) {
                continue;
            }
            bool has_from = false;
            std::uint32_t candidate = 0xFFFFFFFFu;
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t o = tris[t * 3 + i];
                if (o == from_original) has_from = true;
                if (canon[o] == top.to) candidate = o;
            }
            if (has_from && candidate != 0xFFFFFFFFu) {
                to_original = candidate;
                break;
            }
        }
        // Apply.
        for (std::uint32_t t : vertex_tris[top.from]) {
            if (!tri_alive[t]) {
                continue;
            }
            bool degenerate = false;
            for (int i = 0; i < 3; ++i) {
                std::uint32_t& o = tris[t * 3 + i];
                if (canon[o] == top.from) {
                    o = to_original;
                }
            }
            const std::uint32_t a = canon[tris[t * 3]], b = canon[tris[t * 3 + 1]], c = canon[tris[t * 3 + 2]];
            degenerate = a == b || b == c || a == c;
            if (degenerate) {
                tri_alive[t] = false;
                --alive;
            } else {
                vertex_tris[top.to].push_back(t);
            }
        }
        vertex_tris[top.from].clear();
        quadrics[top.to].add(quadrics[top.from]);
        removed[top.from] = true;
        worst = std::max(worst, top.cost);
        version[top.to]++;
        push_edges_of(top.to);
    }

    result.indices.reserve(alive * 3);
    for (std::size_t t = 0; t < triangle_count; ++t) {
        if (tri_alive[t]) {
            result.indices.push_back(tris[t * 3]);
            result.indices.push_back(tris[t * 3 + 1]);
            result.indices.push_back(tris[t * 3 + 2]);
        }
    }
    result.error = static_cast<float>(std::sqrt(std::max(worst, 0.0)) / radius);
    return result;
}

void generate_lods(MeshAsset& mesh) {
    if (mesh.lods.empty()) {
        mesh.lods.push_back({0, static_cast<std::uint32_t>(mesh.indices.size()), 1.0f, 0.0f});
    }
    const std::vector<std::uint32_t> base(mesh.indices.begin(), mesh.indices.begin() + mesh.lods[0].index_count);
    const std::size_t base_triangles = base.size() / 3;
    const float screen_sizes[3] = {0.22f, 0.10f, 0.045f};
    std::size_t previous = base_triangles;
    for (int level = 1; level <= 3; ++level) {
        const std::size_t target = base_triangles >> level;
        if (target < 32) {
            break;
        }
        SimplifyResult lod = simplify_mesh(mesh.vertices, base, target, 0.02f * static_cast<float>(1 << level));
        const std::size_t triangles = lod.indices.size() / 3;
        if (triangles == 0 || triangles > previous * 9 / 10) {
            break; // couldn't reduce meaningfully (locked seams etc.)
        }
        MeshLod entry;
        entry.first_index = static_cast<std::uint32_t>(mesh.indices.size());
        entry.index_count = static_cast<std::uint32_t>(lod.indices.size());
        entry.screen_size = screen_sizes[level - 1];
        entry.error = lod.error;
        mesh.indices.insert(mesh.indices.end(), lod.indices.begin(), lod.indices.end());
        mesh.lods.push_back(entry);
        previous = triangles;
    }
}

} // namespace Hollow
