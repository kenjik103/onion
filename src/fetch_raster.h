#pragma once

#include <string>

#include "grid.h"

// SPEC.md 6.3 / 8.1. One shared function for every raster source: windowed read
// over /vsicurl/, warped into the tile's UTM grid, written to the cache.
struct RasterFetch {
    std::string url;         // HTTPS URL of the source, without the /vsicurl/ prefix
    const char* resampling;  // "near" for categorical data, "bilinear" for continuous
    std::string out;         // output GeoTIFF path
};

// Warps the padded tile out of @p f.url into @p f.out. Returns the output pixel
// size in metres, which is derived from the source's own geotransform rather than
// hardcoded (see native_ground_res_m in the .cpp). Throws std::runtime_error.
double fetch_raster(const RasterFetch& f, const Grid& g);
