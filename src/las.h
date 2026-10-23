#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// SPEC.md 6.10. LAS 1.4, point data record format 6, written directly with no
// dependency. Fields the stand-in does not model are written as 0, except return
// number and number of returns, which are 1.
//
// Streaming rather than take-a-vector: a 1 km tile is about 51 million points, and
// buffering those as structs would cost roughly 1.6 GB before a byte is written.
struct LasPoint {
    double x;  // world coordinates
    double y;
    double z;
    uint32_t object_id;
    uint8_t cls;
};

struct LasOut {
    std::FILE* f = nullptr;
    double offset_x = 0;
    double offset_y = 0;
    std::vector<uint8_t> batch;
    uint64_t declared = 0;
    uint64_t written = 0;
    std::string path;
};

// @p wkt is the OGC WKT for the tile's CRS. It is written as a VLR and the WKT bit is
// set in the header's global encoding, which point formats 6 and up require (gotcha 11).
// @p bounds is xmin, xmax, ymin, ymax, zmin, zmax in world coordinates; the header needs
// them up front, so the caller computes them before streaming.
LasOut las_begin(const std::string& path, double offset_x, double offset_y,
                 const std::string& wkt, uint64_t n_points, const double bounds[6]);
void las_point(LasOut& o, const LasPoint& p);
void las_end(LasOut& o);
