// Building extrusion, including footprints with courtyards. The decisive check is area:
// a roof triangulated from a polygon with holes must cover exactly the exterior area
// minus the holes. Walls project to zero area in xy, so summing every building triangle
// in plan view gives the roof area directly.
#include <cmath>
#include <exception>
#include <cstdio>
#include <string>
#include <vector>

#include "builders.h"

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

// Terrain sloping along x, to check a building is not buried by ground rising under it.
Raster sloped_terrain(double z0, double dz_per_m) {
    Raster r;
    r.w = 64; r.h = 64;
    r.origin_x = -50.0; r.origin_y = 50.0;
    r.px = 100.0 / 64.0; r.py = -100.0 / 64.0;
    r.path = "<synthetic slope>";
    r.v.resize(static_cast<size_t>(r.w) * r.h);
    for (int j = 0; j < r.h; ++j)
        for (int i = 0; i < r.w; ++i) {
            const double x = r.origin_x + (i + 0.5) * r.px;
            r.v[static_cast<size_t>(j) * r.w + i] = static_cast<float>(z0 + dz_per_m * x);
        }
    return r;
}

// A flat terrain raster covering the test area.
Raster flat_terrain(double z) {
    Raster r;
    r.w = 8; r.h = 8;
    r.origin_x = -50.0; r.origin_y = 50.0;
    r.px = 12.5; r.py = -12.5;
    r.path = "<synthetic>";
    r.v.assign(static_cast<size_t>(r.w) * r.h, static_cast<float>(z));
    return r;
}

Grid flat_grid() {
    Grid g{};
    g.utm_epsg = 32616;
    g.tile_m = {-40, -40, 40, 40};
    g.padded_m = {-45, -45, 45, 45};
    g.origin_x = -45; g.origin_y = -45;
    return g;
}

std::vector<double> rect(double x0, double y0, double x1, double y1) {
    return {x0, y0, x1, y0, x1, y1, x0, y1};
}

// Total area of every building triangle in plan view. Walls are vertical, so they
// contribute nothing and this is exactly the roof area.
double roof_area(const Mesh& m) {
    double a = 0.0;
    for (size_t f = 0; f < m.cls.size(); ++f) {
        if (m.cls[f] != CLASS_BUILDING) continue;
        const uint32_t i0 = m.indices[3 * f], i1 = m.indices[3 * f + 1], i2 = m.indices[3 * f + 2];
        const double ax = m.xyz[3 * i0], ay = m.xyz[3 * i0 + 1];
        const double bx = m.xyz[3 * i1], by = m.xyz[3 * i1 + 1];
        const double cx = m.xyz[3 * i2], cy = m.xyz[3 * i2 + 1];
        a += std::abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) * 0.5;
    }
    return a;
}

void run(const std::string& name, const std::vector<std::vector<double>>& rings, double want) {
    Footprint f;
    f.id = name;
    f.height_m = 6.0;
    f.rings = rings;
    Mesh m;
    std::vector<ObjectRow> objs;
    const uint32_t next = build_buildings({f}, flat_terrain(100.0), flat_grid(), 1, m, objs);
    const double got = roof_area(m);
    check(next == 2 && objs.size() == 1, name + ": one object emitted");
    check(std::abs(got - want) < 1e-6,
          name + ": roof area " + fmt(got) + " = " + fmt(want) + " m2");
}

// Regression: bicubic clamped its stencil but never checked the sample point, so terrain
// built against another tile's cached raster came out flat with no error. A point on the
// raster's outer edge is still inside; one beyond it must throw.
void test_bicubic_bounds() {
    const Raster r = flat_terrain(100.0);  // covers x, y in [-50, 50]
    bool edge_ok = true;
    try {
        sample_bicubic(r, -50.0, 50.0);
        sample_bicubic(r, 50.0, -50.0);
    } catch (const std::exception&) {
        edge_ok = false;
    }
    check(edge_ok, "bicubic: samples on the raster's outer edge are accepted");
    for (const double x : {-50.5, 50.5, 1000.0}) {
        bool threw = false;
        try {
            sample_bicubic(r, x, 0.0);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "bicubic: sample at x " + fmt(x) + " outside the raster throws");
    }
}

}  // namespace

int main() {
    test_bicubic_bounds();
    // Plain square: 20 x 20.
    run("square", {rect(-10, -10, 10, 10)}, 400.0);

    // Square with a central courtyard: 400 - 36.
    run("one courtyard", {rect(-10, -10, 10, 10), rect(-3, -3, 3, 3)}, 400.0 - 36.0);

    // Two courtyards, as one Boston building has.
    run("two courtyards",
        {rect(-10, -10, 10, 10), rect(-8, -8, -4, -4), rect(4, 4, 8, 8)},
        400.0 - 16.0 - 16.0);

    // Concave exterior plus a courtyard, which is where a triangle fan would fail.
    // The L is a 20 x 20 square less one 10 x 10 quadrant, so 300 m2, less the 3 x 3
    // courtyard.
    run("L-shape with courtyard",
        {{-10, -10, 10, -10, 10, 0, 0, 0, 0, 10, -10, 10}, rect(-8, -8, -5, -5)},
        300.0 - 9.0);

    // A 20 m footprint on a 20% slope spans 4 m of ground. With the roof measured from
    // the lowest corner it would be swallowed; measured from the highest it cannot be.
    {
        Footprint f;
        f.id = "on a slope";
        f.height_m = 6.0;
        f.rings = {rect(-10, -10, 10, 10)};
        Mesh m;
        std::vector<ObjectRow> objs;
        const Raster terr = sloped_terrain(100.0, 0.20);
        build_buildings({f}, terr, flat_grid(), 1, m, objs);
        float zlo = 1e30f, zhi = -1e30f;
        for (size_t i = 2; i < m.xyz.size(); i += 3) {
            zlo = std::min(zlo, m.xyz[i]);
            zhi = std::max(zhi, m.xyz[i]);
        }
        // Ground under the footprint runs 98 .. 102 m, so the roof must clear 102.
        check(std::abs(zlo - 98.0) < 0.05,
              "slope: base sits on the lowest ground (" + fmt(zlo) + " = 98)");
        check(zhi >= 102.0 + 6.0 - 0.05,
              "slope: roof clears the highest ground by the height constant (" +
                  fmt(zhi) + " >= 108)");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
