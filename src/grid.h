#pragma once

#include "request.h"

struct Bbox {
    double xmin;
    double ymin;
    double xmax;
    double ymax;
};

// SPEC.md 6.2.
inline constexpr double TERRAIN_RES_M = 1.0;
inline constexpr double SAMPLE_SPACING_M = 0.14;

// "A few source pixels of margin" (SPEC.md 6.2). The coarsest source is GEDTM30
// at 30 m and bicubic upsampling needs two neighbours per side, so 60 m is two
// GEDTM30 pixels, six WorldCover pixels and about fifty canopy pixels.
inline constexpr double PAD_M = 60.0;

struct Grid {
    int utm_epsg;
    Bbox tile_m;      // the requested tile, UTM metres
    Bbox padded_m;    // tile_m grown by PAD_M on every side
    double origin_x;  // local origin: padded SW corner, kept as doubles (gotcha 9)
    double origin_y;
    Bbox lonlat;      // bounds of padded_m in lon/lat, for vector queries
};

// Throws std::runtime_error if any projection step fails. Assumes the request
// has already passed validate_request().
Grid make_grid(const TileRequest& r);
