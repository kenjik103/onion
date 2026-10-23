#pragma once

#include <cstddef>
#include <string>

#include "grid.h"

// Queries Overture buildings covering the padded tile and writes them to @p out as
// FlatGeobuf in the tile's UTM CRS (SPEC.md 6.3 step 2). Returns the feature count.
//
// FlatGeobuf rather than GeoJSON or GeoPackage: GeoJSON mandates WGS84, so storing
// UTM coordinates in it would mislead every tool that opens the file, and GeoPackage
// is SQLite and embeds timestamps, which would change the provenance SHA-256 on every
// run. FlatGeobuf carries a real CRS and is byte-stable.
//
// Network: this is the only DuckDB use in the pipeline, and it runs only in `fetch`.
size_t fetch_buildings(const Grid& g, const std::string& release, const std::string& out);
