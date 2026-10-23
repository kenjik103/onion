#pragma once

#include <string>
#include <vector>

// A cached raster held in memory with what is needed to sample it by UTM coordinate.
// North-up only, which is all fetch_raster ever writes.
struct Raster {
    int w = 0;
    int h = 0;
    double origin_x = 0;  // north-west corner
    double origin_y = 0;
    double px = 0;        // pixel size in x, positive
    double py = 0;        // pixel size in y, negative (north-up)
    std::string path;     // kept for error messages
    std::vector<float> v;
};

// SPEC.md 8.2. No scale is applied: no verified source needs one (NOTES/sources.md).
// Throws if any pixel carries the source's declared NoData value, which means the tile
// is not fully covered.
Raster load_raster(const std::string& path);

// Sampling by UTM coordinate. Out of bounds throws rather than clamping: the padded
// tile exists so that every sample lands inside every raster (SPEC.md 6.2), so a miss
// is a bug in the grid, not a condition to paper over.
float sample_nearest(const Raster& r, double x, double y);
float sample_bilinear(const Raster& r, double x, double y);
