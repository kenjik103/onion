// M5 synthetic acceptance tests (SPEC.md 9). These pin the rasteriser against geometry
// whose correct answer is known exactly, which a real tile cannot do.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "scanner.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string fmt(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

uint32_t add_v(ContractMesh& m, double x, double y, double z) {
    const uint32_t i = static_cast<uint32_t>(m.xyz.size() / 3);
    m.xyz.push_back(static_cast<float>(x));
    m.xyz.push_back(static_cast<float>(y));
    m.xyz.push_back(static_cast<float>(z));
    return i;
}

void add_f(ContractMesh& m, uint32_t a, uint32_t b, uint32_t c, uint32_t oid, uint8_t cls) {
    m.indices.insert(m.indices.end(), {a, b, c});
    m.object_id.push_back(oid);
    m.cls.push_back(cls);
}

// Axis-aligned horizontal quad at height z.
void add_quad(ContractMesh& m, double x0, double y0, double x1, double y1, double z,
              uint32_t oid, uint8_t cls) {
    const uint32_t a = add_v(m, x0, y0, z), b = add_v(m, x1, y0, z);
    const uint32_t c = add_v(m, x1, y1, z), d = add_v(m, x0, y1, z);
    add_f(m, a, b, c, oid, cls);
    add_f(m, a, c, d, oid, cls);
}

ScanGrid make_grid(int n) {
    ScanGrid g;
    g.x0 = 0; g.y0 = 0; g.nx = n; g.ny = n; g.spacing = SCAN_SPACING_M; g.seed = 1;
    return g;
}

void test_flat_plane() {
    const int n = 100;
    const ScanGrid g = make_grid(n);
    const double span = n * g.spacing;

    ContractMesh m;
    add_quad(m, -1.0, -1.0, span + 1.0, span + 1.0, 100.0, 0, 2);
    const OpaqueHits h = scan_opaque(m, m.object_id, g);

    const size_t cells = static_cast<size_t>(n) * n;
    check(h.hit_count == cells,
          "flat plane: point count " + std::to_string(h.hit_count) + " equals cell count " +
              std::to_string(cells));

    size_t wrong_z = 0, wrong_cls = 0;
    for (size_t i = 0; i < cells; ++i) {
        if (std::abs(h.z[i] - 100.0) > 0.001) ++wrong_z;
        if (h.cls[i] != 2) ++wrong_cls;
    }
    check(wrong_z == 0, "flat plane: every z within 1 mm of the plane");
    check(wrong_cls == 0, "flat plane: every point class 2");

    // SPEC.md 6.9: one sample per cell gives exactly 1/0.14^2 per m^2.
    const double density = static_cast<double>(h.hit_count) / (span * span);
    const double want = 1.0 / (SCAN_SPACING_M * SCAN_SPACING_M);
    check(std::abs(density - want) < 1e-9,
          "flat plane: density " + fmt(density) + " = 1/0.14^2 = " + fmt(want));
}

void test_box_building() {
    const int n = 200;
    const ScanGrid g = make_grid(n);
    const double span = n * g.spacing;

    ContractMesh m;
    add_quad(m, -1.0, -1.0, span + 1.0, span + 1.0, 0.0, 0, 2);  // ground

    // A 4 x 4 m box with a flat roof at 5 m: roof quad plus four vertical walls.
    const double bx0 = 10.0, by0 = 10.0, bx1 = 14.0, by1 = 14.0, zt = 5.0;
    add_quad(m, bx0, by0, bx1, by1, zt, 1, 6);
    const double corners[4][2] = {{bx0, by0}, {bx1, by0}, {bx1, by1}, {bx0, by1}};
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        const uint32_t a = add_v(m, corners[i][0], corners[i][1], 0.0);
        const uint32_t b = add_v(m, corners[j][0], corners[j][1], 0.0);
        const uint32_t c = add_v(m, corners[j][0], corners[j][1], zt);
        const uint32_t d = add_v(m, corners[i][0], corners[i][1], zt);
        add_f(m, a, b, c, 1, 6);
        add_f(m, a, c, d, 1, 6);
    }

    const OpaqueHits h = scan_opaque(m, m.object_id, g);
    size_t roof = 0, ground = 0, on_wall = 0, roof_outside = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            double sx, sy;
            sample_xy(g, i, j, sx, sy);
            const size_t s = static_cast<size_t>(j) * n + i;
            const bool inside = sx >= bx0 && sx <= bx1 && sy >= by0 && sy <= by1;
            // A wall hit would land strictly between ground and roof.
            if (h.z[s] > 0.001 && h.z[s] < zt - 0.001) ++on_wall;
            if (std::abs(h.z[s] - zt) < 0.001) {
                ++roof;
                if (!inside) ++roof_outside;
            } else if (std::abs(h.z[s]) < 0.001) {
                ++ground;
            }
        }
    check(on_wall == 0, "box: no points on walls");
    check(roof_outside == 0, "box: no roof-height point outside the footprint");
    check(roof > 0 && ground > 0,
          "box: roof points " + std::to_string(roof) + ", ground points " +
              std::to_string(ground));
    check(roof + ground == static_cast<size_t>(n) * n,
          "box: every sample is either roof or ground");

    // The roof covers 16 m^2, so expect that area times the sampling density.
    const double expect = 16.0 / (SCAN_SPACING_M * SCAN_SPACING_M);
    check(std::abs(static_cast<double>(roof) - expect) < 0.05 * expect,
          "box: roof point count " + std::to_string(roof) + " near 16 m^2 x density " +
              fmt(expect));
}

void test_determinism_and_tie_break() {
    const ScanGrid g = make_grid(50);
    ContractMesh m;
    // Two coplanar quads at the same height with different object ids: the smaller id
    // must win every tie (SPEC.md 6.9).
    add_quad(m, -1, -1, 100, 100, 7.0, 9, 2);
    add_quad(m, -1, -1, 100, 100, 7.0, 4, 2);
    const OpaqueHits a = scan_opaque(m, m.object_id, g);
    size_t wrong = 0;
    for (const uint32_t o : a.object_id)
        if (o != 4) ++wrong;
    check(wrong == 0, "exact z ties break to the smallest object id");

    const OpaqueHits b = scan_opaque(m, m.object_id, g);
    check(a.z == b.z && a.object_id == b.object_id, "two scans of one mesh are identical");
}

}  // namespace

int main() {
    test_flat_plane();
    test_box_building();
    test_determinism_and_tie_break();
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
