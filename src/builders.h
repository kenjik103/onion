#pragma once

#include <string>
#include <vector>

#include "contract.h"
#include "grid.h"
#include "raster.h"
#include "trees.h"

// One building footprint from the scene spec, already in UTM metres.
// rings[0] is the exterior; any further rings are interior (courtyards). Each ring is
// x,y pairs and is not closed.
struct Footprint {
    std::string id;
    double height_m = 0;
    std::vector<std::vector<double>> rings;
};

// SPEC.md 6.5. Bicubically upsamples the terrain raster to a 1 m grid over the padded
// tile and triangulates it, two triangles per cell with a fixed diagonal. Appends to
// @p m with class 2 and the given object id.
void build_terrain(const Raster& terrain, const Grid& g, uint32_t object_id, Mesh& m);

// SPEC.md 6.7. Extrudes each footprint into a flat-roofed solid whose base sits at the
// minimum terrain elevation under the footprint. One object id per building.
// Footprints are clipped to the padded tile: Overture's bbox query is an overlap test,
// so an edge-straddling building comes back whole and would otherwise put geometry
// outside the scene. Returns the next free object id.
uint32_t build_buildings(const std::vector<Footprint>& f, const Raster& terrain, const Grid& g,
                         uint32_t first_object_id, Mesh& m, std::vector<ObjectRow>& objects);

// SPEC.md 6.6. Voxelises each crown onto the tile-wide lattice aligned to the local
// origin. Crowns only; stems are out of scope (SPEC.md 3).
std::vector<Crown> build_crowns(const std::vector<Tree>& trees, const Grid& g,
                                uint32_t first_object_id, std::vector<ObjectRow>& objects);
