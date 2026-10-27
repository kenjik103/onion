#include "builders.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

float to_local(double v, double origin) { return static_cast<float>(v - origin); }

double ring_area2(const std::vector<double>& r) {
    double a = 0.0;
    const int n = static_cast<int>(r.size() / 2);
    for (int i = 0, j = n - 1; i < n; j = i++)
        a += r[2 * j] * r[2 * i + 1] - r[2 * i] * r[2 * j + 1];
    return a;
}

// Splice interior rings into the exterior with two-way bridges, turning a polygon with
// holes into one simple polygon the ear clipper can handle. Standard hole elimination:
// take the hole's rightmost vertex, cast a ray to +x, and bridge to the best vertex of
// the edge it hits.
std::vector<double> eliminate_holes(const std::vector<std::vector<double>>& rings) {
    std::vector<double> outer = rings[0];
    if (ring_area2(outer) < 0) {  // exterior counter-clockwise
        std::vector<double> r;
        for (int i = static_cast<int>(outer.size() / 2) - 1; i >= 0; --i) {
            r.push_back(outer[2 * i]);
            r.push_back(outer[2 * i + 1]);
        }
        outer.swap(r);
    }

    // Process holes right to left, so an earlier bridge cannot block a later one.
    std::vector<size_t> order;
    for (size_t h = 1; h < rings.size(); ++h) order.push_back(h);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        double ma = -1e300, mb = -1e300;
        for (size_t i = 0; i < rings[a].size(); i += 2) ma = std::max(ma, rings[a][i]);
        for (size_t i = 0; i < rings[b].size(); i += 2) mb = std::max(mb, rings[b][i]);
        return ma > mb;
    });

    for (const size_t h : order) {
        std::vector<double> hole = rings[h];
        const int hn = static_cast<int>(hole.size() / 2);
        if (hn < 3) continue;
        if (ring_area2(hole) > 0) {  // holes clockwise, opposite the exterior
            std::vector<double> r;
            for (int i = hn - 1; i >= 0; --i) {
                r.push_back(hole[2 * i]);
                r.push_back(hole[2 * i + 1]);
            }
            hole.swap(r);
        }
        int m = 0;
        for (int i = 1; i < hn; ++i)
            if (hole[2 * i] > hole[2 * m]) m = i;
        const double mx = hole[2 * m], my = hole[2 * m + 1];

        // Ray from (mx,my) toward +x: find the nearest crossing on the exterior and
        // bridge to whichever endpoint of that edge lies further right.
        const int on = static_cast<int>(outer.size() / 2);
        int best = -1;
        double best_x = 1e300;
        for (int i = 0, j = on - 1; i < on; j = i++) {
            const double y1 = outer[2 * j + 1], y2 = outer[2 * i + 1];
            if ((y1 > my) == (y2 > my)) continue;
            const double t = (my - y1) / (y2 - y1);
            const double x = outer[2 * j] + t * (outer[2 * i] - outer[2 * j]);
            if (x >= mx && x < best_x) {
                best_x = x;
                best = (outer[2 * i] > outer[2 * j]) ? i : j;
            }
        }
        if (best < 0) continue;  // no crossing: ring is not actually inside, leave it

        std::vector<double> merged;
        for (int i = 0; i <= best; ++i) {
            merged.push_back(outer[2 * i]);
            merged.push_back(outer[2 * i + 1]);
        }
        for (int k = 0; k <= hn; ++k) {
            const int i = (m + k) % hn;
            merged.push_back(hole[2 * i]);
            merged.push_back(hole[2 * i + 1]);
        }
        for (int i = best; i < on; ++i) {
            merged.push_back(outer[2 * i]);
            merged.push_back(outer[2 * i + 1]);
        }
        outer.swap(merged);
    }
    return outer;
}

// Ear clipping for a simple polygon. Footprints are concave often enough (L and U
// shapes) that a triangle fan would produce geometry outside the footprint.
std::vector<int> triangulate(const std::vector<double>& xy) {
    const int n = static_cast<int>(xy.size() / 2);
    std::vector<int> out;
    if (n < 3) return out;

    double area2 = 0.0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        area2 += xy[2 * j] * xy[2 * i + 1] - xy[2 * i] * xy[2 * j + 1];
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = (area2 > 0) ? i : n - 1 - i;  // force CCW

    const auto cross = [&](int a, int b, int c) {
        return (xy[2 * b] - xy[2 * a]) * (xy[2 * c + 1] - xy[2 * a + 1]) -
               (xy[2 * b + 1] - xy[2 * a + 1]) * (xy[2 * c] - xy[2 * a]);
    };
    // Compare coordinates, not indices. Hole bridging duplicates vertices, so a copy of
    // the ear's own corner sits exactly on the triangle's boundary at a different index
    // and would otherwise veto every ear, leaving the roof untriangulated.
    const auto same = [&](int p, int q) {
        return xy[2 * p] == xy[2 * q] && xy[2 * p + 1] == xy[2 * q + 1];
    };
    const auto inside = [&](int a, int b, int c, int p) {
        if (same(p, a) || same(p, b) || same(p, c)) return false;
        return cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0;
    };

    int guard = 0;
    while (idx.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        const int m = static_cast<int>(idx.size());
        for (int i = 0; i < m; ++i) {
            const int a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
            if (cross(a, b, c) <= 0) continue;  // reflex
            bool ok = true;
            for (const int p : idx)
                if (p != a && p != b && p != c && inside(a, b, c, p)) { ok = false; break; }
            if (!ok) continue;
            out.insert(out.end(), {a, b, c});
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) break;  // degenerate ring; emit what is left below
    }
    if (idx.size() == 3) out.insert(out.end(), {idx[0], idx[1], idx[2]});
    return out;
}

// Sutherland-Hodgman against an axis-aligned rectangle. The clip region is convex, so
// this is exact; a footprint wholly inside is returned unchanged.
std::vector<double> clip_to_rect(const std::vector<double>& xy, const Bbox& b) {
    std::vector<double> poly = xy;
    const double edges[4][3] = {{1, 0, b.xmin}, {-1, 0, -b.xmax},
                                {0, 1, b.ymin}, {0, -1, -b.ymax}};
    for (const auto& e : edges) {
        const std::vector<double> in = poly;
        poly.clear();
        const int n = static_cast<int>(in.size() / 2);
        if (n == 0) return {};
        const auto side = [&](int i) {
            return e[0] * in[2 * i] + e[1] * in[2 * i + 1] - e[2];
        };
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const double di = side(i);
            const double dj = side(j);
            if (di >= 0.0) { poly.push_back(in[2 * i]); poly.push_back(in[2 * i + 1]); }
            if ((di >= 0.0) != (dj >= 0.0)) {
                const double t = di / (di - dj);
                poly.push_back(in[2 * i] + t * (in[2 * j] - in[2 * i]));
                poly.push_back(in[2 * i + 1] + t * (in[2 * j + 1] - in[2 * i + 1]));
            }
        }
    }
    return poly;
}

// Lowest and highest terrain anywhere under a footprint, sampled on the same 1 m
// lattice the terrain mesh is built from, so this sees exactly the surface the scanner
// will see. Sampling only the footprint's vertices misses interior dips and rises:
// measured on a Boston tile, the vertex minimum sat up to 3.94 m above the true one.
void terrain_range_under(const std::vector<double>& ring, const Raster& terrain,
                         const Grid& g, double& lo, double& hi) {
    lo = 1e30;
    hi = -1e30;
    const int n = static_cast<int>(ring.size() / 2);
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (int i = 0; i < n; ++i) {
        const double x = ring[2 * i], y = ring[2 * i + 1];
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
        const double z = sample_bicubic(terrain, x, y);   // vertices always count
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }

    // Walk the lattice the terrain mesh uses, so a footprint that contains no lattice
    // node still gets its vertex samples above.
    const auto snap = [&](double v, double origin) {
        return origin + std::ceil((v - origin) / TERRAIN_RES_M) * TERRAIN_RES_M;
    };
    for (double y = snap(y0, g.padded_m.ymin); y <= y1; y += TERRAIN_RES_M) {
        for (double x = snap(x0, g.padded_m.xmin); x <= x1; x += TERRAIN_RES_M) {
            bool in = false;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                const double yi = ring[2 * i + 1], yj = ring[2 * j + 1];
                if ((yi > y) != (yj > y) &&
                    x < (ring[2 * j] - ring[2 * i]) * (y - yi) / (yj - yi) + ring[2 * i])
                    in = !in;
            }
            if (!in) continue;
            const double z = sample_bicubic(terrain, x, y);
            lo = std::min(lo, z);
            hi = std::max(hi, z);
        }
    }
}

void add_face(Mesh& m, uint32_t a, uint32_t b, uint32_t c, uint32_t oid, uint8_t cls) {
    m.indices.insert(m.indices.end(), {a, b, c});
    m.object_id.push_back(oid);
    m.cls.push_back(cls);
}

uint32_t add_vertex(Mesh& m, float x, float y, float z) {
    const uint32_t i = static_cast<uint32_t>(m.vertex_count());
    m.xyz.insert(m.xyz.end(), {x, y, z});
    return i;
}

}  // namespace

void build_terrain(const Raster& terrain, const Grid& g, uint32_t object_id, Mesh& m) {
    const double w = g.padded_m.xmax - g.padded_m.xmin;
    const double h = g.padded_m.ymax - g.padded_m.ymin;
    const int nx = static_cast<int>(std::llround(w / TERRAIN_RES_M));
    const int ny = static_cast<int>(std::llround(h / TERRAIN_RES_M));
    if (nx < 1 || ny < 1) throw std::runtime_error("terrain: padded tile is smaller than a cell");

    const uint32_t base = static_cast<uint32_t>(m.vertex_count());
    m.xyz.reserve(m.xyz.size() + static_cast<size_t>(nx + 1) * (ny + 1) * 3);
    for (int j = 0; j <= ny; ++j) {
        const double y = g.padded_m.ymin + j * TERRAIN_RES_M;
        for (int i = 0; i <= nx; ++i) {
            const double x = g.padded_m.xmin + i * TERRAIN_RES_M;
            m.xyz.push_back(to_local(x, g.origin_x));
            m.xyz.push_back(to_local(y, g.origin_y));
            m.xyz.push_back(static_cast<float>(sample_bicubic(terrain, x, y)));
        }
    }
    // Two triangles per cell with a fixed diagonal, so the surface is reproducible.
    const auto v = [&](int i, int j) {
        return base + static_cast<uint32_t>(j) * (nx + 1) + static_cast<uint32_t>(i);
    };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            add_face(m, v(i, j), v(i + 1, j), v(i + 1, j + 1), object_id, CLASS_GROUND);
            add_face(m, v(i, j), v(i + 1, j + 1), v(i, j + 1), object_id, CLASS_GROUND);
        }
}

uint32_t build_buildings(const std::vector<Footprint>& fps, const Raster& terrain, const Grid& g,
                         uint32_t first_object_id, Mesh& m, std::vector<ObjectRow>& objects) {
    uint32_t oid = first_object_id;
    for (const Footprint& raw : fps) {
        if (raw.rings.empty() || raw.rings[0].size() < 6)
            throw std::runtime_error("buildings: footprint " + raw.id + " has under 3 vertices");

        std::vector<std::vector<double>> rings;
        rings.push_back(clip_to_rect(raw.rings[0], g.padded_m));
        if (rings[0].size() < 6) continue;  // falls entirely outside the padded tile

        // Interior rings are kept only when the exterior survived clipping untouched.
        // A building the padded edge cuts is at least 60 m outside the sampled tile and
        // is never scanned, so a courtyard there cannot affect the output.
        const bool clipped = rings[0].size() != raw.rings[0].size();
        if (!clipped)
            for (size_t h = 1; h < raw.rings.size(); ++h)
                if (raw.rings[h].size() >= 6) rings.push_back(raw.rings[h]);

        // The solid spans from the lowest ground under the footprint to the height
        // constant above the HIGHEST ground under it. Basing the roof on the minimum
        // instead buries the building wherever the ground rises: measured on a Boston
        // tile, 876 of 1307 buildings had terrain more than 1 m inside them and 129
        // were swallowed whole, contributing no roof points at all.
        //
        // The base still sits at the minimum, so nothing floats (SPEC.md 6.7); the
        // height constant now means "above the ground the building stands on" rather
        // than "above its lowest corner".
        double lo = 1e30, hi = -1e30;
        for (const std::vector<double>& r : rings) {
            double rlo, rhi;
            terrain_range_under(r, terrain, g, rlo, rhi);
            lo = std::min(lo, rlo);
            hi = std::max(hi, rhi);
        }
        const float zb = static_cast<float>(lo);
        const float zt = static_cast<float>(hi + raw.height_m);

        // Walls for every ring. An interior ring gives the courtyard walls, which are
        // as real as the outside ones.
        for (const std::vector<double>& r : rings) {
            const int n = static_cast<int>(r.size() / 2);
            std::vector<uint32_t> bot(n), top(n);
            for (int i = 0; i < n; ++i) {
                const float lx = to_local(r[2 * i], g.origin_x);
                const float ly = to_local(r[2 * i + 1], g.origin_y);
                bot[i] = add_vertex(m, lx, ly, zb);
                top[i] = add_vertex(m, lx, ly, zt);
            }
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                add_face(m, bot[i], bot[j], top[j], oid, CLASS_BUILDING);
                add_face(m, bot[i], top[j], top[i], oid, CLASS_BUILDING);
            }
        }

        // Roof: bridge any holes into the exterior, then ear-clip the result.
        const std::vector<double> flat = rings.size() > 1 ? eliminate_holes(rings) : rings[0];
        std::vector<uint32_t> rv(flat.size() / 2);
        for (size_t i = 0; i < rv.size(); ++i)
            rv[i] = add_vertex(m, to_local(flat[2 * i], g.origin_x),
                               to_local(flat[2 * i + 1], g.origin_y), zt);
        const std::vector<int> roof = triangulate(flat);
        for (size_t t = 0; t + 2 < roof.size(); t += 3)
            add_face(m, rv[roof[t]], rv[roof[t + 1]], rv[roof[t + 2]], oid, CLASS_BUILDING);

        objects.push_back({oid, CLASS_BUILDING, "overture:" + raw.id});
        ++oid;
    }
    return oid;
}

std::vector<Crown> build_crowns(const std::vector<Tree>& trees, const Grid& g,
                                uint32_t first_object_id, std::vector<ObjectRow>& objects) {
    const double vs = CROWN_VOXEL_SIZE_M;
    std::vector<Crown> crowns;
    crowns.reserve(trees.size());
    uint32_t oid = first_object_id;

    for (const Tree& t : trees) {
        const double top_z = t.ground_z + t.height_m;
        const double base_z = t.ground_z + CROWN_BASE_RATIO * t.height_m;
        const double cz = 0.5 * (top_z + base_z);
        const double rz = 0.5 * (top_z - base_z);
        const double rx = t.crown_radius_m;
        if (!(rz > 0.0 && rx > 0.0)) continue;

        // Local coordinates, so the lattice is anchored at the local origin: index i
        // spans [i*vs, (i+1)*vs) measured from the origin, shared by every crown.
        const double lx = t.x - g.origin_x;
        const double ly = t.y - g.origin_y;

        // Clip the block to the padded tile in x and y. A crown centred near the edge
        // reaches past it by up to its radius, and the contract states that every local
        // coordinate lies inside the padded box. The clipped part is outside the scene
        // and the scanner only samples the tile proper, so nothing sampled is lost.
        const int32_t i_hi = static_cast<int32_t>(
                                 std::ceil((g.padded_m.xmax - g.padded_m.xmin) / vs)) - 1;
        const int32_t j_hi = static_cast<int32_t>(
                                 std::ceil((g.padded_m.ymax - g.padded_m.ymin) / vs)) - 1;
        const int32_t i0 = std::max<int32_t>(static_cast<int32_t>(std::floor((lx - rx) / vs)), 0);
        const int32_t j0 = std::max<int32_t>(static_cast<int32_t>(std::floor((ly - rx) / vs)), 0);
        const int32_t i1 = std::min<int32_t>(static_cast<int32_t>(std::floor((lx + rx) / vs)), i_hi);
        const int32_t j1 = std::min<int32_t>(static_cast<int32_t>(std::floor((ly + rx) / vs)), j_hi);
        if (i1 < i0 || j1 < j0) continue;  // entirely outside the padded tile

        Crown c;
        c.object_id = oid;
        c.ix = i0;
        c.iy = j0;
        c.iz = static_cast<int32_t>(std::floor((cz - rz) / vs));
        c.nx = static_cast<uint32_t>(i1 - i0 + 1);
        c.ny = static_cast<uint32_t>(j1 - j0 + 1);
        c.nz = static_cast<uint32_t>(std::floor((cz + rz) / vs)) - c.iz + 1;
        c.pad.assign(static_cast<size_t>(c.nx) * c.ny * c.nz, 0.0f);

        // Ellipsoid envelope: every voxel whose centre is inside gets the archetype PAD.
        size_t k = 0;
        for (uint32_t kz = 0; kz < c.nz; ++kz) {
            const double vz = (c.iz + kz + 0.5) * vs - cz;
            for (uint32_t ky = 0; ky < c.ny; ++ky) {
                const double vy = (c.iy + ky + 0.5) * vs - ly;
                for (uint32_t kx = 0; kx < c.nx; ++kx, ++k) {
                    const double vx = (c.ix + kx + 0.5) * vs - lx;
                    const double d = (vx * vx + vy * vy) / (rx * rx) + (vz * vz) / (rz * rz);
                    if (d <= 1.0) c.pad[k] = static_cast<float>(CROWN_PAD);
                }
            }
        }
        objects.push_back({oid, CLASS_HIGH_VEGETATION, "chm_crown"});
        crowns.push_back(std::move(c));
        ++oid;
    }
    return crowns;
}
