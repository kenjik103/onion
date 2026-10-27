// Building extrusion, including footprints with courtyards. The decisive check is area:
// a roof triangulated from a polygon with holes must cover exactly the exterior area
// minus the holes. Walls project to zero area in xy, so summing every building triangle
// in plan view gives the roof area directly.
#include <cmath>
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

}  // namespace

int main() {
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

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
