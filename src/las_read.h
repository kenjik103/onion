#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Reads points back out of the LAS the generator wrote, for the viewer (SPEC.md 6.11).
// A tile is about 51 million points, so this decimates on load. Display only: nothing is
// written back, and the factor is reported so a screenshot can state it (SPEC.md 9, V2).
struct LasPoints {
    std::vector<float> xyz;   // x, y local to the contract origin; z absolute
    std::vector<uint8_t> cls;
    size_t total = 0;         // points in the file, before decimation
    int stride = 1;
};

LasPoints read_las_points(const std::string& path, double origin_x, double origin_y,
                          size_t max_points);
