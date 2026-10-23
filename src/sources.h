#pragma once

#include <string>

#include "grid.h"

// Every constant here was verified against the live source on 2026-10-23.
// See NOTES/sources.md for the commands and the full gdalinfo output.
// SPEC.md 4.5: nothing is hardcoded that has not been inspected.

inline const char* const GEDTM30_URL =
    "https://s3.opengeohub.org/global/dtm/v1.2/"
    "gedtm_rf_m_30m_s_20060101_20151231_go_epsg.4326.3855_v1.2.tif";

inline const char* const WORLDCOVER_BASE =
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/";

// v2 (CHMv2, DINOv3-based), chosen at gate G3 on 2026-10-23; see NOTES/g3/README.md.
// The version is recorded in the provenance, not in the cache file name, so a future
// change of version stays auditable without renaming anything.
inline const char* const CANOPY_BASE =
    "https://dataforgood-fb-data.s3.us-east-1.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3/";
inline const char* const CANOPY_VERSION = "v2";

// Pinned, and recorded in the manifest (SPEC.md 6.10). Latest release as of
// 2026-10-23; the two older ones present are 2026-09-23.0 and 2026-09-23.1.
inline const char* const OVERTURE_RELEASE = "2026-10-21.0";

// WorldCover tiles are 3x3 degrees named by their south-west corner, e.g. "N45W099".
std::string worldcover_tile_name(double lat, double lon);

// Throws if the padded tile spans more than one WorldCover tile: mosaicking is not
// implemented, and silently using one tile would leave a hole.
std::string worldcover_tile_for(const Grid& g);

// Looks the covering canopy tile up in a cached tiles.geojson (the verified index
// mechanism, SPEC.md 7.3). Throws unless exactly one tile covers the padded box.
std::string canopy_tile_for(const std::string& index_path, const Grid& g);
