#pragma once

#include <string>

#include "grid.h"
#include "request.h"

// SPEC.md 6.7: one height rule, a constant, for every footprint. Overture's optional
// height attribute is deliberately ignored so the scene never mixes sourced and guessed
// heights. SPEC.md 11 lists this constant as the user's decision.
inline constexpr double BUILDING_HEIGHT_M = 6.0;

#include <vector>

#include "builders.h"
#include "trees.h"

// The scene spec, in memory. Assembly writes it to disk and the builders read it from
// there in spirit: it is the only input they get (SPEC.md 4.4).
struct SceneSpec {
    std::vector<Tree> trees;
    std::vector<Footprint> buildings;
    std::string terrain_path;
};

// Reads the cache, derives the tree list and the building list, and writes
// scene_spec.json (SPEC.md 6.4).
SceneSpec write_scene_spec(const TileRequest& r, const Grid& g, const std::string& cache,
                           const std::string& out_dir);
