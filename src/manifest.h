#pragma once

#include <string>

#include "grid.h"
#include "request.h"

// SPEC.md 6.10: tile request, seed, Overture release, all provenance from the scene
// spec, the contract format version, and the generator's git commit.
void write_manifest(const TileRequest& r, const Grid& g, const std::string& cache,
                    const std::string& out_dir, uint64_t point_count);
