#include "cache.h"

#include <stdexcept>

#include <cpl_conv.h>
#include <cpl_json.h>
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

void write_request(const std::string& cache, const TileRequest& r) {
    CPLJSONDocument doc;
    CPLJSONObject root = doc.GetRoot();
    root.Add("lat", r.lat);
    root.Add("lon", r.lon);
    root.Add("extent_m", r.extent_m);
    root.Add("seed", static_cast<GInt64>(r.seed));
    if (!doc.Save(cache_path(cache, "request.json")))
        throw std::runtime_error("cache: cannot write request.json in " + cache);
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
