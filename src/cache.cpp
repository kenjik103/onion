#include "cache.h"

#include <stdexcept>

#include <cpl_conv.h>
#include <cpl_json.h>
#include <cpl_string.h>
#include <cpl_vsi.h>

#include "sha256.h"

std::string cache_path(const std::string& cache, const std::string& name) {
    return cache + "/" + name;
}

bool file_exists(const std::string& path) {
    VSIStatBufL st;
    return VSIStatL(path.c_str(), &st) == 0;
}

void ensure_dir(const std::string& path) {
    VSIStatBufL st;
    if (VSIStatL(path.c_str(), &st) == 0) {
        if (!VSI_ISDIR(st.st_mode))
            throw std::runtime_error("not a directory: " + path);
        return;
    }
    if (VSIMkdir(path.c_str(), 0755) != 0)
        throw std::runtime_error("cannot create directory: " + path);
}

std::string partial_path(const std::string& final_path) {
    const size_t slash = final_path.find_last_of('/');
    const size_t dot = final_path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        throw std::runtime_error("cache: expected a file extension on " + final_path);
    return final_path.substr(0, dot) + ".partial" + final_path.substr(dot);
}

void cache_remote_file(const std::string& url, const std::string& out) {
    if (file_exists(out)) return;
    // Copy to a temporary name first so an interrupted download can never be mistaken
    // for a complete cache entry on the next run.
    const std::string tmp = out + ".partial";
    if (CPLCopyFile(tmp.c_str(), ("/vsicurl/" + url).c_str()) != 0) {
        VSIUnlink(tmp.c_str());
        throw std::runtime_error("cache: download failed: " + url);
    }
    if (VSIRename(tmp.c_str(), out.c_str()) != 0) {
        VSIUnlink(tmp.c_str());
        throw std::runtime_error("cache: cannot move " + tmp + " into place");
    }
}

namespace {

const char* const TILE_KEYS[] = {"lat", "lon", "extent_m"};

CPLJSONDocument request_doc(const TileRequest& r) {
    CPLJSONDocument doc;
    CPLJSONObject root = doc.GetRoot();
    root.Add("lat", r.lat);
    root.Add("lon", r.lon);
    root.Add("extent_m", r.extent_m);
    root.Add("seed", static_cast<GInt64>(r.seed));
    return doc;
}

// Compared as recorded text, not as numbers: CPLJSON writes doubles to about 15
// significant digits, so 46.88000000000001 reads back as 46.880000000000003 and an exact
// numeric comparison would reject a cache against its own request. The text form is
// stable through a save and load (measured), so equal text means the same recorded tile.
void check_request(const std::string& cache, const TileRequest& r) {
    const std::string path = cache_path(cache, "request.json");
    CPLJSONDocument stored;
    if (!stored.Load(path)) throw std::runtime_error("cache: cannot read " + path);
    const CPLJSONDocument want = request_doc(r);
    for (const char* key : TILE_KEYS) {
        const CPLJSONObject s = stored.GetRoot().GetObj(key);
        if (!s.IsValid()) throw std::runtime_error("cache: " + path + " has no " + key);
        const std::string got = s.Format(CPLJSONObject::PrettyFormat::Plain);
        const std::string asked =
            want.GetRoot().GetObj(key).Format(CPLJSONObject::PrettyFormat::Plain);
        if (got != asked)
            throw std::runtime_error("cache: " + cache + " holds a different tile (" + key +
                                     " " + got + " in request.json, " + asked +
                                     " requested); use a separate cache directory per tile");
    }
}

}  // namespace

void claim_cache(const std::string& cache, const TileRequest& r) {
    const std::string path = cache_path(cache, "request.json");
    if (file_exists(path)) {
        check_request(cache, r);
        return;
    }
    // Without request.json nothing says which tile any file here belongs to.
    const CPLStringList entries(VSIReadDir(cache.c_str()));
    for (int i = 0; i < entries.size(); ++i) {
        const std::string name = entries[i];
        if (name != "." && name != "..")
            throw std::runtime_error("cache: " + cache + " has no request.json but is not "
                                     "empty (found " + name + "), so its tile is unknown");
    }
    if (!request_doc(r).Save(path))
        throw std::runtime_error("cache: cannot write request.json in " + cache);
}

void require_cache_matches(const std::string& cache, const TileRequest& r) {
    if (!file_exists(cache_path(cache, "request.json")))
        throw std::runtime_error("cache: " + cache + " has no request.json; run fetch first");
    check_request(cache, r);
}

void write_provenance(const std::string& cache, const std::vector<Provenance>& entries) {
    CPLJSONDocument doc;
    CPLJSONObject root = doc.GetRoot();
    for (const Provenance& p : entries) {
        CPLJSONObject e;
        e.Add("url", p.url);
        e.Add("version", p.version);
        e.Add("vintage", p.vintage);
        e.Add("file", p.file);
        e.Add("sha256", p.sha256);
        root.Add(p.name, e);
    }
    if (!doc.Save(cache_path(cache, "provenance.json")))
        throw std::runtime_error("cache: cannot write provenance.json in " + cache);
}
