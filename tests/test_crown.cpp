// G4 synthetic acceptance (SPEC.md 9). A uniform-PAD voxel block of known depth must
// intercept a fraction 1 - exp(-G * PAD * depth) of nadir rays. Checked at four PAD
// values spanning thin to nearly opaque.
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

constexpr double VOXEL = 0.5;
constexpr double G_COEF = 0.5;

// A slab of uniform PAD covering every sample, nz voxels deep.
CrownBlock uniform_block(double pad, int nz, double span) {
    CrownBlock c;
    c.object_id = 42;
    c.ix = -2;
    c.iy = -2;
    c.iz = 200;  // arbitrary elevation, well above the ground plane below
    c.nx = static_cast<uint32_t>(std::ceil(span / VOXEL)) + 4;
    c.ny = c.nx;
    c.nz = static_cast<uint32_t>(nz);
    c.pad.assign(static_cast<size_t>(c.nx) * c.ny * c.nz, static_cast<float>(pad));
    return c;
}

void test_interception(double pad, int nz) {
    const int n = 220;
    ScanGrid g;
    g.x0 = 0; g.y0 = 0; g.nx = n; g.ny = n; g.spacing = SCAN_SPACING_M; g.seed = 7;
    const double span = n * g.spacing;

    OpaqueHits hits;
    const size_t total = static_cast<size_t>(n) * n;
    hits.z.assign(total, 0.0f);          // a ground plane at z = 0, far below the slab
    hits.object_id.assign(total, 0);
    hits.cls.assign(total, 2);
    hits.hit_count = total;

    const std::vector<CrownBlock> crowns{uniform_block(pad, nz, span)};
    scan_vegetation(crowns, VOXEL, G_COEF, g, hits);

    size_t veg = 0;
    for (const uint8_t c : hits.cls)
        if (c == 5) ++veg;
    const double measured = static_cast<double>(veg) / total;
    const double depth = nz * VOXEL;
    const double expect = 1.0 - std::exp(-G_COEF * pad * depth);
    // Binomial standard error; four of them is a generous but still meaningful band.
    const double se = std::sqrt(expect * (1.0 - expect) / total);
    const double tol = std::max(4.0 * se, 1e-4);

    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "PAD %.2f depth %4.1f m: measured %.4f, expected %.4f, diff %+.4f "
                  "(4 s.e. = %.4f)",
                  pad, depth, measured, expect, measured - expect, tol);
    check(std::abs(measured - expect) <= tol, buf);
}

// Every vegetation point must lie inside the slab, and the depths must fill it rather
// than piling up at voxel boundaries.
void test_depth_distribution() {
    const int n = 220;
    ScanGrid g;
    g.x0 = 0; g.y0 = 0; g.nx = n; g.ny = n; g.spacing = SCAN_SPACING_M; g.seed = 11;
    const double span = n * g.spacing;
    const int nz = 40;
    const double pad = 0.15;

    OpaqueHits hits;
    const size_t total = static_cast<size_t>(n) * n;
    hits.z.assign(total, 0.0f);
    hits.object_id.assign(total, 0);
    hits.cls.assign(total, 2);
    hits.hit_count = total;
    scan_vegetation({uniform_block(pad, nz, span)}, VOXEL, G_COEF, g, hits);

    const double top = (200 + nz) * VOXEL;
    const double bottom = 200 * VOXEL;
    size_t outside = 0, veg = 0;
    // Phase of the hit inside its own voxel. Banding would show as a spike in one bin.
    std::vector<size_t> phase(10, 0);
    for (size_t i = 0; i < total; ++i) {
        if (hits.cls[i] != 5) continue;
        ++veg;
        const double z = hits.z[i];
        if (z > top + 1e-6 || z < bottom - 1e-6) ++outside;
        const double f = (top - z) / VOXEL;
        phase[static_cast<size_t>((f - std::floor(f)) * 10) % 10]++;
    }
    check(outside == 0, "every vegetation point lies inside the slab");
    check(veg > total / 2, "slab intercepts most rays (" + std::to_string(veg) + ")");

    size_t lo = SIZE_MAX, hi = 0;
    for (const size_t c : phase) { lo = std::min(lo, c); hi = std::max(hi, c); }
    // With rate*s = 0.0375 the within-voxel density is nearly flat, so a correct
    // sampler gives near-equal bins; quantising to layer boundaries would not.
    const double spread = static_cast<double>(hi - lo) / (static_cast<double>(veg) / 10.0);
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "within-voxel depth phase is flat: 10 bins, spread %.3f of the mean",
                  spread);
    check(spread < 0.15, buf);
}

}  // namespace

int main() {
    std::printf("Beer-Lambert interception, G = %.2f, voxel %.2f m\n\n", G_COEF, VOXEL);
    test_interception(0.10, 20);
    test_interception(0.25, 20);
    test_interception(0.50, 20);
    test_interception(1.00, 20);
    std::printf("\n");
    test_depth_distribution();
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
