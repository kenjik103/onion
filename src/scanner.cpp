#include "scanner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

OpaqueHits scan_opaque(const ContractMesh& mesh, const std::vector<uint32_t>& object_id,
                       const ScanGrid& g) {
    OpaqueHits out;
    const size_t n = static_cast<size_t>(g.nx) * g.ny;
    out.z.assign(n, SCAN_MISS_Z);
    out.object_id.assign(n, 0);
    out.cls.assign(n, 0);

    const size_t nf = mesh.cls.size();
    for (size_t f = 0; f < nf; ++f) {
        const uint32_t ia = mesh.indices[3 * f];
        const uint32_t ib = mesh.indices[3 * f + 1];
        const uint32_t ic = mesh.indices[3 * f + 2];
        const double ax = mesh.xyz[3 * ia], ay = mesh.xyz[3 * ia + 1], az = mesh.xyz[3 * ia + 2];
        const double bx = mesh.xyz[3 * ib], by = mesh.xyz[3 * ib + 1], bz = mesh.xyz[3 * ib + 2];
        const double cx = mesh.xyz[3 * ic], cy = mesh.xyz[3 * ic + 1], cz = mesh.xyz[3 * ic + 2];

        // Signed area in xy. Exactly zero means the triangle is vertical, so it projects
        // to a line and can receive no nadir sample. This is what keeps wall triangles
        // from collecting points (SPEC.md 9, M5).
        const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (area == 0.0) continue;
        const double inv = 1.0 / area;

        const double xlo = std::min({ax, bx, cx});
        const double xhi = std::max({ax, bx, cx});
        const double ylo = std::min({ay, by, cy});
        const double yhi = std::max({ay, by, cy});

        // A sample sits anywhere inside its cell, so widen the cell range by one on each
        // side rather than assuming the sample is at the cell's corner.
        const int i0 = std::max(0, static_cast<int>(std::floor((xlo - g.x0) / g.spacing)) - 1);
        const int i1 = std::min(g.nx - 1, static_cast<int>(std::floor((xhi - g.x0) / g.spacing)) + 1);
        const int j0 = std::max(0, static_cast<int>(std::floor((ylo - g.y0) / g.spacing)) - 1);
        const int j1 = std::min(g.ny - 1, static_cast<int>(std::floor((yhi - g.y0) / g.spacing)) + 1);
        if (i1 < i0 || j1 < j0) continue;

        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                double sx, sy;
                sample_xy(g, i, j, sx, sy);

                // Inclusive edge test: a sample on a shared edge hits both triangles,
                // which is harmless, rather than neither, which would leave a hole
                // (gotcha 10).
                const double w0 = ((bx - ax) * (sy - ay) - (by - ay) * (sx - ax)) * inv;
                const double w1 = ((cx - bx) * (sy - by) - (cy - by) * (sx - bx)) * inv;
                const double w2 = ((ax - cx) * (sy - cy) - (ay - cy) * (sx - cx)) * inv;
                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;

                // Barycentric interpolation of z. w1 belongs to vertex a, w2 to b,
                // w0 to c, because each edge function is opposite its vertex.
                const double z = w1 * az + w2 * bz + w0 * cz;
                const size_t s = static_cast<size_t>(j) * g.nx + i;
                const float zf = static_cast<float>(z);
                if (zf > out.z[s] || (zf == out.z[s] && object_id[f] < out.object_id[s])) {
                    if (out.z[s] == SCAN_MISS_Z) ++out.hit_count;
                    out.z[s] = zf;
                    out.object_id[s] = object_id[f];
                    out.cls[s] = mesh.cls[f];
                }
            }
        }
    }
    return out;
}

namespace {

// Summed PAD and the smallest contributing object id, for one voxel of one column.
struct VoxelSample {
    double pad = 0.0;
    uint32_t object_id = 0;
    bool any = false;
};

}  // namespace

void scan_vegetation(const std::vector<CrownBlock>& crowns, double voxel_size, double g_coef,
                     const ScanGrid& grid, OpaqueHits& hits) {
    if (crowns.empty() || !(voxel_size > 0.0) || !(g_coef > 0.0)) return;

    // Index crowns by the sample cells their xy block covers, so a sample only consults
    // the handful of crowns actually above it rather than all of them.
    int32_t kz_lo = INT32_MAX, kz_hi = INT32_MIN;
    for (const CrownBlock& c : crowns) {
        kz_lo = std::min(kz_lo, c.iz);
        kz_hi = std::max(kz_hi, c.iz + static_cast<int32_t>(c.nz) - 1);
    }

    std::vector<std::vector<uint32_t>> index(static_cast<size_t>(grid.nx) * grid.ny);
    for (uint32_t ci = 0; ci < crowns.size(); ++ci) {
        const CrownBlock& c = crowns[ci];
        const double x0 = c.ix * voxel_size, x1 = (c.ix + c.nx) * voxel_size;
        const double y0 = c.iy * voxel_size, y1 = (c.iy + c.ny) * voxel_size;
        const int i0 = std::max(0, static_cast<int>(std::floor((x0 - grid.x0) / grid.spacing)) - 1);
        const int i1 = std::min(grid.nx - 1,
                                static_cast<int>(std::floor((x1 - grid.x0) / grid.spacing)) + 1);
        const int j0 = std::max(0, static_cast<int>(std::floor((y0 - grid.y0) / grid.spacing)) - 1);
        const int j1 = std::min(grid.ny - 1,
                                static_cast<int>(std::floor((y1 - grid.y0) / grid.spacing)) + 1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                index[static_cast<size_t>(j) * grid.nx + i].push_back(ci);
    }

    std::vector<VoxelSample> column;
    for (int j = 0; j < grid.ny; ++j) {
        for (int i = 0; i < grid.nx; ++i) {
            const size_t s = static_cast<size_t>(j) * grid.nx + i;
            const std::vector<uint32_t>& cand = index[s];
            if (cand.empty()) continue;

            double sx, sy;
            sample_xy(grid, i, j, sx, sy);
            const int32_t vx = static_cast<int32_t>(std::floor(sx / voxel_size));
            const int32_t vy = static_cast<int32_t>(std::floor(sy / voxel_size));

            // Accumulate the column once, so overlapping crowns sum exactly.
            const int32_t nz = kz_hi - kz_lo + 1;
            column.assign(static_cast<size_t>(nz), VoxelSample{});
            bool any = false;
            for (const uint32_t ci : cand) {
                const CrownBlock& c = crowns[ci];
                const int32_t lx = vx - c.ix;
                const int32_t ly = vy - c.iy;
                if (lx < 0 || ly < 0 || lx >= static_cast<int32_t>(c.nx) ||
                    ly >= static_cast<int32_t>(c.ny))
                    continue;
                for (uint32_t k = 0; k < c.nz; ++k) {
                    const float p = c.pad[(static_cast<size_t>(k) * c.ny + ly) * c.nx + lx];
                    if (p <= 0.0f) continue;
                    VoxelSample& v = column[static_cast<size_t>(c.iz + k - kz_lo)];
                    v.pad += p;
                    v.object_id = v.any ? std::min(v.object_id, c.object_id) : c.object_id;
                    v.any = true;
                    any = true;
                }
            }
            if (!any) continue;

            // Walk down from the top of the lattice to the opaque surface.
            const double opaque_z = hits.z[s] == SCAN_MISS_Z ? -1e30 : hits.z[s];
            uint64_t step = 0;
            for (int32_t k = nz - 1; k >= 0; --k, ++step) {
                const VoxelSample& v = column[static_cast<size_t>(k)];
                const double top = (kz_lo + k + 1) * voxel_size;
                const double bot = top - voxel_size;
                if (bot < opaque_z) break;
                if (!v.any || v.pad <= 0.0) continue;

                const double rate = g_coef * v.pad;
                const double p_hit = 1.0 - std::exp(-rate * voxel_size);
                // Draws 0 and 1 are the sample's xy offset, so vegetation starts at 2.
                if (scan_rand01(grid.seed, s, 2 + 2 * step) >= p_hit) continue;

                // Depth inside the voxel, from the truncated exponential with this rate.
                const double u = scan_rand01(grid.seed, s, 3 + 2 * step);
                const double d = -std::log(1.0 - u * p_hit) / rate;
                hits.z[s] = static_cast<float>(top - d);
                hits.object_id[s] = v.object_id;
                hits.cls[s] = 5;
                if (opaque_z == -1e30) ++hits.hit_count;
                break;
            }
        }
    }
}
