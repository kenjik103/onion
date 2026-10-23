#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Reads the scene geometry contract, per FORMAT.md. This is the viewer's only scene
// input (SPEC.md 6.11 rule 1) -- it never touches the cache or the scene spec.

struct ContractMeta {
    int format_version = 0;
    int epsg = 0;
    double origin_x = 0, origin_y = 0;
    double bbox[4] = {0, 0, 0, 0};  // local xmin, ymin, xmax, ymax
    double crown_pad = 0, crown_g = 0, voxel_size_m = 0, crown_base_ratio = 0;
    // class -> number of objects, for the panel
    size_t n_objects = 0;
};

struct ContractMesh {
    std::vector<float> xyz;         // 3 per vertex, local
    std::vector<uint32_t> indices;    // 3 per face
    std::vector<uint32_t> object_id;  // 1 per face
    std::vector<uint8_t> cls;         // 1 per face
};

// Crown voxel centres. crowns.bin is hundreds of megabytes and most voxels are empty,
// so this streams the file and keeps only non-empty voxels, decimated to @p max_points.
// The decimation is display-only and is reported so a screenshot can state it.
struct CrownPoints {
    std::vector<float> xyz;
    std::vector<float> pad;
    size_t total_nonempty = 0;
    size_t total_voxels = 0;
    size_t crown_count = 0;
    int stride = 1;
};

// A crown block exactly as crowns.bin stores it (FORMAT.md section 4).
struct CrownBlock {
    uint32_t object_id = 0;
    int32_t ix = 0, iy = 0, iz = 0;
    uint32_t nx = 0, ny = 0, nz = 0;
    std::vector<float> pad;  // nx*ny*nz, x fastest then y then z
};

ContractMeta read_scene_geometry(const std::string& path);
std::vector<CrownBlock> read_crowns(const std::string& path);
ContractMesh read_opaque_ply(const std::string& path);
CrownPoints read_crown_points(const std::string& path, double voxel_size, size_t max_points);
