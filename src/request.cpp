#include "request.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

// Verified 2026-10-23 with gdalinfo on the GEDTM30 v1.2 COG: the raster spans
// 85.00125 N to 65.00125 S. See NOTES/sources.md.
//
// Only GEDTM30's bounds are checked here because it is the only source with a
// verified global latitude extent. WorldCover has no verified global bound, and
// canopy coverage is per-tile; both are caught at `fetch` time when the tile
// lookup finds nothing, which is the correct fail-fast point for them.
constexpr double GEDTM30_LAT_MIN = -65.00125;
constexpr double GEDTM30_LAT_MAX = 85.00125;

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string describe(const TileRequest& r) {
    return "tile centre (" + fmt(r.lat) + ", " + fmt(r.lon) + "), extent " +
           fmt(r.extent_m) + " m";
}

}  // namespace

void validate_request(const TileRequest& r) {
    if (!(r.lat >= GEDTM30_LAT_MIN && r.lat <= GEDTM30_LAT_MAX))
        throw std::runtime_error(
            "request: latitude " + fmt(r.lat) + " is outside GEDTM30 v1.2 coverage [" +
            fmt(GEDTM30_LAT_MIN) + ", " + fmt(GEDTM30_LAT_MAX) + "]; " + describe(r));

    if (!(r.lon >= -180.0 && r.lon < 180.0))
        throw std::runtime_error(
            "request: longitude " + fmt(r.lon) + " is outside [-180, 180); longitude 180 "
            "is rejected because the UTM zone formula maps it to zone 61 and a tile there "
            "straddles the antimeridian; " + describe(r));

    if (!(r.extent_m > 0.0))
        throw std::runtime_error(
            "request: extent " + fmt(r.extent_m) + " m must be greater than 0; " + describe(r));
}
