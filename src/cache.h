#pragma once

#include <string>
#include <vector>

#include "request.h"

// One cached raw pull, as SPEC.md 6.3 requires: url, dataset version, vintage
// (acquisition date where the source states one) and a SHA-256 of the cached file.
struct Provenance {
    std::string name;     // cache key, e.g. "terrain"
    std::string url;      // exact URL the bytes came from
    std::string version;  // pinned dataset version
    std::string vintage;  // acquisition date or vintage, or why it is unknown
    std::string file;     // file name inside the cache directory
    std::string sha256;
};

std::string cache_path(const std::string& cache, const std::string& name);
bool file_exists(const std::string& path);
void ensure_cache_dir(const std::string& cache);

// Copies a remote file into the cache if it is not already there. Used for the
// canopy tile indexes, which are plain GeoJSON with no spatial index and so must be
// read whole; caching them is what keeps a second `fetch` offline.
void cache_remote_file(const std::string& url, const std::string& out);

// A temporary sibling path that KEEPS the final extension: "a/b.tif" -> "a/b.partial.tif".
// GDAL drivers select behaviour from the extension, and an unrecognised one fails in
// ways that are easy to miss -- GTiff writes nothing and reports no error, FlatGeobuf
// writes a directory of per-layer files. Fetchers write here and rename on success, so
// an interrupted run never leaves a file the next run would treat as cached.
std::string partial_path(const std::string& final_path);

void write_request(const std::string& cache, const TileRequest& r);
void write_provenance(const std::string& cache, const std::vector<Provenance>& entries);
