#include "scanner.h"

#include <algorithm>
#include <cmath>

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
