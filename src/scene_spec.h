#pragma once

#include <string>

#include "grid.h"
#include "request.h"

// SPEC.md 6.7: one height rule, a constant, for every footprint. Overture's optional
// height attribute is deliberately ignored so the scene never mixes sourced and guessed
// heights. SPEC.md 11 lists this constant as the user's decision.
inline constexpr double BUILDING_HEIGHT_M = 6.0;

// Reads the cache, derives the tree list and the building list, and writes
// scene_spec.json (SPEC.md 6.4). The scene spec is the only input to the builders.
void write_scene_spec(const TileRequest& r, const Grid& g, const std::string& cache,
                      const std::string& out_dir);
