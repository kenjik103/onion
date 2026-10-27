// Unit checks for the two pieces of M2 that encode a verified convention in code:
// WorldCover's tile naming and the temporary-path rule that GDAL's extension-based
// driver selection forces on us.
#include <cstdio>
#include <cstdlib>
#include <functional>
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

// Regression: `fetch` created its cache directory but `generate` did not create its
// output directory, so the documented sequence failed on a fresh checkout. Both now go
// through ensure_dir.
void test_ensure_dir() {
    const std::string base = "/tmp/onion_ensure_dir_test";
    std::remove(base.c_str());
    ensure_dir(base);
    check(file_exists(base), "ensure_dir creates a missing directory");
    ensure_dir(base);  // must be idempotent, since fetch re-runs against a warm cache
    check(file_exists(base), "ensure_dir on an existing directory is a no-op");
    std::remove(base.c_str());
}

void expect_throw(const std::function<void()>& f, const std::string& what) {
    try {
        f();
    } catch (const std::exception&) {
        check(true, "rejected: " + what);
        return;
    }
    check(false, "rejected: " + what);
}

void expect_ok(const std::function<void()>& f, const std::string& what) {
    try {
        f();
    } catch (const std::exception& e) {
        check(false, what + " (" + e.what() + ")");
        return;
    }
    check(true, what);
}

// Regression: a cache reused for another tile was treated as cached file by file, so
// fetch recorded one tile's URLs against another's bytes and generate built flat terrain
// with no error. request.json is now the cache's identity and both subcommands check it.
void test_cache_identity() {
    const std::string base = "/tmp/onion_cache_identity_test";
    std::system(("rm -rf " + base).c_str());
    ensure_dir(base);

    const TileRequest fargo{46.88, -96.79, 1000.0, 1};
    expect_throw([&] { require_cache_matches(base, fargo); },
                 "generate against a cache with no request.json");
    expect_ok([&] { claim_cache(base, fargo); }, "fetch claims an empty cache");
    check(file_exists(cache_path(base, "request.json")), "claiming writes request.json");
    expect_ok([&] { claim_cache(base, fargo); }, "fetch reuses its own cache");
    expect_ok([&] { claim_cache(base, {46.88, -96.79, 1000.0, 99}); },
              "a different seed is the same tile");
    expect_ok([&] { require_cache_matches(base, fargo); }, "generate accepts a matching cache");

    expect_throw([&] { claim_cache(base, {42.36, -71.06, 1000.0, 1}); },
                 "fetch into another tile's cache");
    expect_throw([&] { require_cache_matches(base, {46.88, -96.79, 500.0, 1}); },
                 "generate with a different extent");
    expect_throw([&] { require_cache_matches(base, {46.8801, -96.79, 1000.0, 1}); },
                 "generate with a different latitude");

    // More digits than CPLJSON keeps: must still match itself.
    const TileRequest fine{46.88000000000001, -96.79, 1000.0, 1};
    std::system(("rm -rf " + base + " && mkdir " + base).c_str());
    claim_cache(base, fine);
    expect_ok([&] { require_cache_matches(base, fine); },
              "a request with 16 significant digits matches its own cache");

    // Files with no request.json: their tile is unknown.
    std::system(("rm -rf " + base + " && mkdir " + base + " && touch " + base +
                 "/terrain.tif").c_str());
    expect_throw([&] { claim_cache(base, fargo); },
                 "fetch into a non-empty cache with no request.json");

    std::system(("rm -rf " + base).c_str());
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
        test_ensure_dir();
        test_cache_identity();
        test_partial_path();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
