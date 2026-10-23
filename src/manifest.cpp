#include "manifest.h"

#include <stdexcept>

#include <cpl_json.h>

#include "cache.h"
#include "contract.h"
#include "sources.h"

#ifndef ONION_GIT_COMMIT
#define ONION_GIT_COMMIT "unknown"
#endif

void write_manifest(const TileRequest& r, const Grid& g, const std::string& cache,
                    const std::string& out_dir, uint64_t point_count) {
    CPLJSONDocument doc;
    CPLJSONObject root = doc.GetRoot();

    CPLJSONObject req;
    req.Add("lat", r.lat);
    req.Add("lon", r.lon);
    req.Add("extent_m", r.extent_m);
    req.Add("seed", static_cast<GInt64>(r.seed));
    root.Add("request", req);

    root.Add("utm_epsg", g.utm_epsg);
    root.Add("overture_release", OVERTURE_RELEASE);
    root.Add("contract_format_version", CONTRACT_FORMAT_VERSION);
    root.Add("git_commit", ONION_GIT_COMMIT);
    root.Add("point_count", static_cast<GInt64>(point_count));

    CPLJSONDocument prov;
    if (!prov.Load(cache_path(cache, "provenance.json")))
        throw std::runtime_error("manifest: cannot read provenance.json in " + cache);
    root.Add("provenance", prov.GetRoot());

    const std::string path = out_dir + "/manifest.json";
    if (!doc.Save(path)) throw std::runtime_error("manifest: cannot write " + path);
}
