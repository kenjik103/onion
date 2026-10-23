#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The scene geometry contract (SPEC.md 6.8). Any change to any contract file bumps
// this version. FORMAT.md documents the files byte by byte.
inline constexpr int CONTRACT_FORMAT_VERSION = 1;

// ASPRS classification codes (SPEC.md 6.10).
inline constexpr uint8_t CLASS_GROUND = 2;
inline constexpr uint8_t CLASS_HIGH_VEGETATION = 5;
inline constexpr uint8_t CLASS_BUILDING = 6;

// --- Crown archetype (SPEC.md 6.6) -----------------------------------------------
// PROVISIONAL. SPEC.md 11 settles these at gate G4; they are gathered here so that
// gate changes one block. G and VOXEL_SIZE_M are SPEC.md's stated defaults.
//
// CROWN_VOXEL_SIZE_M is 0.50 m, not SPEC.md's suggested 0.25 m, because 0.25 m was
// measured against the real tree list and is impractical: 1511 M voxels, a 6.04 GB
// crowns.bin for one 1 km tile. At 0.50 m the same tile is 203 M voxels and 0.81 GB.
// The trade-off is recorded in NOTES/m4/README.md for G4 to settle.
inline constexpr double CROWN_PAD = 0.5;            // plant area density, m2/m3
inline constexpr double CROWN_G = 0.5;              // nadir projection, random leaves
inline constexpr double CROWN_BASE_RATIO = 0.4;     // crown starts at this fraction of height
inline constexpr double CROWN_VOXEL_SIZE_M = 0.50;  // metres

// Opaque triangle mesh in local coordinates (float32 relative to the local origin,
// gotcha 9). Every face carries an object id and a class (SPEC.md 6.8).
struct Mesh {
    std::vector<float> xyz;          // 3 floats per vertex
    std::vector<uint32_t> indices;   // 3 per face
    std::vector<uint32_t> object_id; // 1 per face
    std::vector<uint8_t> cls;        // 1 per face

    size_t vertex_count() const { return xyz.size() / 3; }
    size_t face_count() const { return indices.size() / 3; }
};

// One voxelised crown: a block of the tile-wide lattice that is aligned to the local
// origin, so overlapping crowns combine by summing PAD per voxel (SPEC.md 6.6).
struct Crown {
    uint32_t object_id = 0;
    int32_t ix = 0, iy = 0, iz = 0;   // lattice index of the block's minimum corner
    uint32_t nx = 0, ny = 0, nz = 0;
    std::vector<float> pad;           // nx*ny*nz, x fastest then y then z
};

// The object table written into scene_geometry.json.
struct ObjectRow {
    uint32_t object_id;
    uint8_t cls;
    std::string source;
};
