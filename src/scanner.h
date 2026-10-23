#pragma once

#include <cstdint>
#include <vector>

#include "contract_read.h"

// SPEC.md 6.9. The stand-in scanner: every sample is a ray cast straight down, which is
// an orthographic depth buffer rendered from above. No sensor physics lives here --
// no off-nadir rays, multiple returns, noise, intensity, or beam footprint (SPEC.md 3).

inline constexpr double SCAN_SPACING_M = 0.14;

// Counter-based randomness (SplitMix64), so a draw depends only on (seed, sample, draw)
// and never on evaluation order. Parallelising later cannot change the output.
inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline double scan_rand01(uint64_t seed, uint64_t sample, uint64_t draw) {
    const uint64_t h = splitmix64(splitmix64(seed ^ (sample * 0x2545F4914F6CDD1Dull)) ^ draw);
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);  // 2^-53
}

// The sample grid, in local coordinates. Cells cover the tile, not the padded box.
struct ScanGrid {
    double x0 = 0, y0 = 0;  // south-west corner of cell (0,0)
    int nx = 0, ny = 0;
    double spacing = SCAN_SPACING_M;
    uint64_t seed = 0;
};

// One sample's position, at a seeded random offset inside its cell.
inline void sample_xy(const ScanGrid& g, int i, int j, double& x, double& y) {
    const uint64_t s = static_cast<uint64_t>(j) * g.nx + i;
    x = g.x0 + (i + scan_rand01(g.seed, s, 0)) * g.spacing;
    y = g.y0 + (j + scan_rand01(g.seed, s, 1)) * g.spacing;
}

// Highest opaque surface under each sample. miss_z marks samples with no hit.
struct OpaqueHits {
    std::vector<float> z;
    std::vector<uint32_t> object_id;
    std::vector<uint8_t> cls;
    size_t hit_count = 0;
};

inline constexpr float SCAN_MISS_Z = -1e30f;

// Rasterises every triangle onto the samples inside its xy bounding box with an
// inclusive edge test, keeping the maximum z and breaking exact ties by the smallest
// object id (SPEC.md 6.9, gotcha 10).
OpaqueHits scan_opaque(const ContractMesh& mesh, const std::vector<uint32_t>& object_id,
                       const ScanGrid& g);

// SPEC.md 6.9, vegetation. For each sample, walks the voxel column from the top down to
// the opaque hit. A voxel with PAD > 0 intercepts the ray with probability
// 1 - exp(-G * PAD * voxel_size); on interception the hit depth inside the voxel is
// drawn from the truncated exponential with rate G * PAD. The point then gets class 5
// and the crown's object id. If nothing intercepts, the opaque hit stands.
//
// Overlapping crowns sum their PAD (SPEC.md 6.6). The object id reported for a voxel
// several crowns reach is the smallest among them, matching the opaque tie-break rule.
// @p hits is updated in place.
void scan_vegetation(const std::vector<CrownBlock>& crowns, double voxel_size, double g_coef,
                     const ScanGrid& grid, OpaqueHits& hits);
