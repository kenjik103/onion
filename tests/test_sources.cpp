// Unit checks for the two pieces of M2 that encode a verified convention in code:
// WorldCover's tile naming and the temporary-path rule that GDAL's extension-based
// driver selection forces on us.
#include <cstdio>
#include <stdexcept>
#include <string>

#include "cache.h"
#include "sources.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void eq(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " -> " + want + (got == want ? "" : ", got " + got));
}

void test_worldcover_names() {
    // Verified live: this tile returns HTTP 200 and its corners are 45-48 N / 99-96 W,
    // so it contains the Fargo test tile.
    eq(worldcover_tile_name(46.88, -96.79), "N45W099", "Fargo");
    eq(worldcover_tile_name(0.0, 0.0), "N00E000", "origin");
    eq(worldcover_tile_name(-33.87, 151.21), "S36E150", "Sydney");
    // Just south/west of zero must round down, not toward zero.
    eq(worldcover_tile_name(-0.5, -0.5), "S03W003", "just below zero");
    eq(worldcover_tile_name(45.0, -99.0), "N45W099", "exactly on a tile corner");
}

void test_partial_path() {
    eq(partial_path("/a/b/terrain.tif"), "/a/b/terrain.partial.tif", "raster temp name");
    eq(partial_path("/a/b/buildings.fgb"), "/a/b/buildings.partial.fgb", "vector temp name");
    // The extension must survive, so a path without one is an error rather than a
    // silently odd name: GTiff would then write nothing and FlatGeobuf a directory.
    try {
        partial_path("/a.dir/noextension");
        check(false, "rejected: path with no extension");
    } catch (const std::exception&) {
        check(true, "rejected: path with no extension");
    }
}

}  // namespace

int main() {
    try {
        test_worldcover_names();
        test_partial_path();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
