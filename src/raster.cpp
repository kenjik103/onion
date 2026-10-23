#include "raster.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <gdal_priv.h>

namespace {

// Pixel coordinates measured from the raster's outer edge, so the valid domain is
// [0, w] x [0, h] -- the raster's actual extent. Mixing this up with centre-based
// indices silently excludes the outer half pixel on every side.
struct PixelPos {
    double col;
    double row;
};

PixelPos to_pixel(const Raster& r, double x, double y) {
    const PixelPos p{(x - r.origin_x) / r.px, (y - r.origin_y) / r.py};
    const double eps = 1e-6;
    if (!(p.col >= -eps && p.col <= r.w + eps && p.row >= -eps && p.row <= r.h + eps))
        throw std::runtime_error("raster: sample (" + std::to_string(x) + ", " +
                                 std::to_string(y) + ") falls outside " + r.path);
    return p;
}

}  // namespace

Raster load_raster(const std::string& path) {
    const GDALDatasetUniquePtr ds(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
    if (!ds) throw std::runtime_error("raster: open failed: " + path);

    double gt[6];
    if (ds->GetGeoTransform(gt) != CE_None)
        throw std::runtime_error("raster: no geotransform: " + path);
    if (gt[2] != 0.0 || gt[4] != 0.0)
        throw std::runtime_error("raster: rotated geotransform not handled: " + path);

    Raster r;
    r.path = path;
    GDALRasterBand* band = ds->GetRasterBand(1);
    r.w = band->GetXSize();
    r.h = band->GetYSize();
    r.origin_x = gt[0];
    r.origin_y = gt[3];
    r.px = gt[1];
    r.py = gt[5];
    r.v.resize(static_cast<size_t>(r.w) * r.h);

    if (band->RasterIO(GF_Read, 0, 0, r.w, r.h, r.v.data(), r.w, r.h, GDT_Float32, 0, 0) != CE_None)
        throw std::runtime_error("raster: read failed: " + path);

    int has_nd = 0;
    // Cast to float before comparing, or a Float32 sentinel never matches the double
    // GetNoDataValue returns and the check silently passes (gotcha 1).
    const float nd = static_cast<float>(band->GetNoDataValue(&has_nd));
    if (has_nd) {
        for (const float x : r.v)
            if (x == nd) throw std::runtime_error("raster: nodata inside the tile: " + path);
    }
    return r;
}

float sample_nearest(const Raster& r, double x, double y) {
    const PixelPos p = to_pixel(r, x, y);
    const int c = std::clamp(static_cast<int>(std::floor(p.col)), 0, r.w - 1);
    const int row = std::clamp(static_cast<int>(std::floor(p.row)), 0, r.h - 1);
    return r.v[static_cast<size_t>(row) * r.w + c];
}

float sample_bilinear(const Raster& r, double x, double y) {
    const PixelPos p = to_pixel(r, x, y);
    // Interpolate between pixel centres, which sit half a pixel inside the edge.
    // Samples in the outer half pixel clamp to the edge pixel rather than extrapolate.
    const double cc = std::clamp(p.col - 0.5, 0.0, static_cast<double>(r.w - 1));
    const double rc = std::clamp(p.row - 0.5, 0.0, static_cast<double>(r.h - 1));
    const int c0 = std::min(static_cast<int>(std::floor(cc)), std::max(r.w - 2, 0));
    const int r0 = std::min(static_cast<int>(std::floor(rc)), std::max(r.h - 2, 0));
    const int c1 = std::min(c0 + 1, r.w - 1);
    const int r1 = std::min(r0 + 1, r.h - 1);
    const double fx = cc - c0;
    const double fy = rc - r0;
    const auto at = [&](int rr, int ccx) {
        return static_cast<double>(r.v[static_cast<size_t>(rr) * r.w + ccx]);
    };
    return static_cast<float>(at(r0, c0) * (1 - fx) * (1 - fy) + at(r0, c1) * fx * (1 - fy) +
                              at(r1, c0) * (1 - fx) * fy + at(r1, c1) * fx * fy);
}
