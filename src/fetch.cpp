#include "fetch.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

#include <cpl_vsi.h>

#include "buildings.h"
#include "cache.h"
#include "fetch_raster.h"
#include "grid.h"
#include "sha256.h"
#include "sources.h"

namespace {

// Fetchers are independent of each other (SPEC.md 6.3) and each one skips work it
// has already done, which is what makes a second run offline.
Provenance fetch_one_raster(const std::string& cache, const std::string& name,
                            const std::string& url, const char* resampling,
                            const std::string& version, const std::string& vintage,
                            const Grid& g) {
    const std::string file = name + ".tif";
    const std::string out = cache_path(cache, file);

    if (file_exists(out)) {
        std::printf("  %-12s cached\n", name.c_str());
    } else {
        // Warp to a temporary name and rename on success, so an interrupted run can
        // never leave a truncated file that the next run would treat as cached.
        const std::string tmp = partial_path(out);
        VSIUnlink(tmp.c_str());
        double res;
        try {
            res = fetch_raster({url, resampling, tmp}, g);
        } catch (...) {
            VSIUnlink(tmp.c_str());
            throw;
        }
        if (VSIRename(tmp.c_str(), out.c_str()) != 0) {
            VSIUnlink(tmp.c_str());
            throw std::runtime_error("fetch: cannot move " + tmp + " into place");
        }
        std::printf("  %-12s fetched at %.4f m/px\n", name.c_str(), res);
    }
    return {name, url, version, vintage, file, sha256_file(out)};
}

Provenance fetch_terrain(const std::string& cache, const Grid& g) {
    return fetch_one_raster(cache, "terrain", GEDTM30_URL, "bilinear", "GEDTM30 v1.2",
                            "2006-01-01/2015-12-31 (encoded in the file name)", g);
}

Provenance fetch_landcover(const std::string& cache, const Grid& g) {
    const std::string tile = worldcover_tile_for(g);
    const std::string url =
        std::string(WORLDCOVER_BASE) + "ESA_WorldCover_10m_2021_v200_" + tile + "_Map.tif";
    // Categorical: nearest neighbour, never bilinear (SPEC.md 6.3).
    return fetch_one_raster(cache, "landcover", url, "near",
                            "ESA WorldCover v200 (2021), tile " + tile,
                            "2021-01-01/2021-12-31", g);
}

Provenance fetch_canopy(const std::string& cache, const Grid& g) {
    const std::string index_path = cache_path(cache, "chm_tiles.geojson");
    const bool had_index = file_exists(index_path);
    cache_remote_file(std::string(CANOPY_BASE) + "tiles.geojson", index_path);
    if (!had_index) std::printf("  %-12s index cached\n", "chm");

    const std::string tile = canopy_tile_for(index_path, g);
    return fetch_one_raster(cache, "chm", std::string(CANOPY_BASE) + "chm/" + tile + ".tif",
                            "bilinear",
                            std::string("Meta/WRI canopy height ") + CANOPY_VERSION +
                                ", tile " + tile,
                            // Source imagery vintage varies by location (gotcha 7) and v2
                            // publishes no per-tile acquisition date.
                            "varies by location; not published per tile for v2", g);
}

Provenance fetch_overture_buildings(const std::string& cache, const Grid& g) {
    const std::string file = "buildings.fgb";
    const std::string out = cache_path(cache, file);
    const std::string url = "s3://overturemaps-us-west-2/release/" +
                            std::string(OVERTURE_RELEASE) + "/theme=buildings/type=building/*";

    if (file_exists(out)) {
        std::printf("  %-12s cached\n", "buildings");
    } else {
        const std::string tmp = partial_path(out);
        VSIUnlink(tmp.c_str());
        size_t n;
        try {
            n = fetch_buildings(g, OVERTURE_RELEASE, tmp);
        } catch (...) {
            VSIUnlink(tmp.c_str());
            throw;
        }
        if (VSIRename(tmp.c_str(), out.c_str()) != 0) {
            VSIUnlink(tmp.c_str());
            throw std::runtime_error("fetch: cannot move " + tmp + " into place");
        }
        std::printf("  %-12s fetched %zu footprints\n", "buildings", n);
    }
    return {"buildings", url, std::string("Overture release ") + OVERTURE_RELEASE,
            OVERTURE_RELEASE, file, sha256_file(out)};
}

}  // namespace

void run_fetch(const TileRequest& r, const std::string& cache) {
    const Grid g = make_grid(r);
    ensure_cache_dir(cache);
    write_request(cache, r);

    std::vector<Provenance> prov;
    prov.push_back(fetch_terrain(cache, g));
    prov.push_back(fetch_landcover(cache, g));
    prov.push_back(fetch_canopy(cache, g));
    prov.push_back(fetch_overture_buildings(cache, g));

    write_provenance(cache, prov);
    std::printf("  provenance   %zu sources -> provenance.json\n", prov.size());
}
