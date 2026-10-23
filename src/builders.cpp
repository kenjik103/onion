#include "builders.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

float to_local(double v, double origin) { return static_cast<float>(v - origin); }

// Catmull-Rom, the interpolating cubic. Used for the terrain upsample (SPEC.md 6.5).
double cubic(double p0, double p1, double p2, double p3, double t) {
    return p1 + 0.5 * t *
                    (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 +
                                    t * (3.0 * (p1 - p2) + p3 - p0)));
}

double sample_bicubic(const Raster& r, double x, double y) {
    const double c = (x - r.origin_x) / r.px - 0.5;  // pixel-centre coordinates
    const double rw = (y - r.origin_y) / r.py - 0.5;
    const int c0 = static_cast<int>(std::floor(c));
    const int r0 = static_cast<int>(std::floor(rw));
    const double fx = c - c0;
    const double fy = rw - r0;
    const auto at = [&](int rr, int cc) {
        rr = std::clamp(rr, 0, r.h - 1);
        cc = std::clamp(cc, 0, r.w - 1);
        return static_cast<double>(r.v[static_cast<size_t>(rr) * r.w + cc]);
    };
    double col[4];
    for (int j = 0; j < 4; ++j)
        col[j] = cubic(at(r0 - 1 + j, c0 - 1), at(r0 - 1 + j, c0), at(r0 - 1 + j, c0 + 1),
                       at(r0 - 1 + j, c0 + 2), fx);
    return cubic(col[0], col[1], col[2], col[3], fy);
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
    const auto inside = [&](int a, int b, int c, int p) {
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
        if (raw.xy.size() < 6)
            throw std::runtime_error("buildings: footprint " + raw.id + " has under 3 vertices");
        Footprint f = raw;
        f.xy = clip_to_rect(raw.xy, g.padded_m);
        const int n = static_cast<int>(f.xy.size() / 2);
        if (n < 3) continue;  // falls entirely outside the padded tile

        // Base at the minimum terrain elevation under the footprint, so the solid never
        // floats on sloping ground (SPEC.md 6.7).
        double base_z = 1e30;
        for (int i = 0; i < n; ++i)
            base_z = std::min(base_z, sample_bicubic(terrain, f.xy[2 * i], f.xy[2 * i + 1]));
        const float zb = static_cast<float>(base_z);
        const float zt = static_cast<float>(base_z + f.height_m);

        std::vector<uint32_t> bot(n), top(n);
        for (int i = 0; i < n; ++i) {
            const float lx = to_local(f.xy[2 * i], g.origin_x);
            const float ly = to_local(f.xy[2 * i + 1], g.origin_y);
            bot[i] = add_vertex(m, lx, ly, zb);
            top[i] = add_vertex(m, lx, ly, zt);
        }
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            add_face(m, bot[i], bot[j], top[j], oid, CLASS_BUILDING);
            add_face(m, bot[i], top[j], top[i], oid, CLASS_BUILDING);
        }
        const std::vector<int> roof = triangulate(f.xy);
        for (size_t t = 0; t + 2 < roof.size(); t += 3)
            add_face(m, top[roof[t]], top[roof[t + 1]], top[roof[t + 2]], oid, CLASS_BUILDING);
        objects.push_back({oid, CLASS_BUILDING, "overture:" + f.id});
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
