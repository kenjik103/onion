#pragma once

#include <vector>

#include "grid.h"
#include "raster.h"

// SPEC.md 6.4 tree list entry.
struct Tree {
    double x;               // UTM metres
    double y;
    double ground_z;        // terrain elevation at (x, y), EGM2008
    double height_m;        // canopy height above ground
    double crown_radius_m;
};

// Approved at M3 on 2026-10-23; see NOTES/m3/README.md for the evidence.
// Detection window radius = WINDOW_K * tree height.
inline constexpr double WINDOW_K = 0.100;
inline constexpr double TREE_H_MIN = 2.0;   // metres; below this is ground clutter
inline constexpr int WINDOW_R_MIN_PX = 1;
inline constexpr int WINDOW_R_MAX_PX = 12;
inline constexpr int WORLDCOVER_TREE = 10;

// Local maxima with a height-scaled window, tied plateaus reduced to one tree each,
// crown size from a marker-controlled watershed over the canopy.
// @p padded is the scene extent. Each cached raster is snapped outward to a whole
// number of its own pixels, so the canopy raster reaches past the padded box; detection
// is confined to the box so every tree lies inside the scene the contract describes.
std::vector<Tree> detect_trees(const Raster& chm, const Raster& landcover,
                               const Raster& terrain, const Bbox& padded);
