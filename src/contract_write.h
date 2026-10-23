#pragma once

#include <string>
#include <vector>

#include "contract.h"
#include "grid.h"

// Writes the three contract files documented in FORMAT.md (SPEC.md 6.8).
void write_opaque_ply(const Mesh& m, const std::string& path);
void write_crowns_bin(const std::vector<Crown>& crowns, const std::string& path);
void write_scene_geometry_json(const Grid& g, const std::vector<ObjectRow>& objects,
                               const std::string& path);
