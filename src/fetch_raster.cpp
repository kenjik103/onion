#include "fetch_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>

namespace {

struct CTDeleter {
    void operator()(OGRCoordinateTransformation* p) const {
        OGRCoordinateTransformation::DestroyCT(p);
    }
};
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, CTDeleter>;

void load_utm(OGRSpatialReference& srs, int epsg) {
    if (srs.importFromEPSG(epsg) != OGRERR_NONE)
        throw std::runtime_error("fetch_raster: importFromEPSG(" + std::to_string(epsg) + ") failed");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // gotcha 3
}

// The output resolution SPEC.md 6.3 calls "the source's native resolution", measured
// in UTM metres at the tile centre.
//
// A source pixel is generally not square on the ground: GEDTM30 and WorldCover are in
// EPSG:4326, where an east-west pixel shrinks by cos(latitude), and the canopy maps are
// in EPSG:3857, where every pixel is inflated by 1/cos(latitude). Taking the **finer**
// of the two axes means the warp never discards detail the source actually carries; the
// coarser axis is merely interpolated.
//
// This is measured through GDAL rather than computed from a spherical-earth formula, so
// it stays correct for any source CRS without this file knowing any geodesy.
double native_ground_res_m(GDALDatasetH src, const Grid& g, const std::string& url) {
    double gt[6];
    if (GDALGetGeoTransform(src, gt) != CE_None)
        throw std::runtime_error("fetch_raster: no geotransform: " + url);
    if (gt[2] != 0.0 || gt[4] != 0.0)
        throw std::runtime_error("fetch_raster: source geotransform is rotated, which this "
                                 "pipeline does not handle: " + url);

    const char* wkt = GDALGetProjectionRef(src);
    if (!wkt || wkt[0] == '\0')
        throw std::runtime_error("fetch_raster: source has no CRS: " + url);

    OGRSpatialReference src_srs;
    if (src_srs.importFromWkt(&wkt) != OGRERR_NONE)
        throw std::runtime_error("fetch_raster: cannot parse source CRS: " + url);
    src_srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference utm;
    load_utm(utm, g.utm_epsg);

    const TransformPtr to_src(OGRCreateCoordinateTransformation(&utm, &src_srs));
    const TransformPtr to_utm(OGRCreateCoordinateTransformation(&src_srs, &utm));
    if (!to_src || !to_utm)
        throw std::runtime_error("fetch_raster: cannot build transformations for " + url);

    // Tile centre, into the source CRS.
    double sx = (g.tile_m.xmin + g.tile_m.xmax) / 2.0;
    double sy = (g.tile_m.ymin + g.tile_m.ymax) / 2.0;
    if (!to_src->Transform(1, &sx, &sy))
        throw std::runtime_error("fetch_raster: tile centre does not project into " + url);

    // The centre and one source pixel along each axis, back into UTM metres.
    double xs[3] = {sx, sx + std::abs(gt[1]), sx};
    double ys[3] = {sy, sy, sy + std::abs(gt[5])};
    if (!to_utm->Transform(3, xs, ys))
        throw std::runtime_error("fetch_raster: pixel-step transform failed for " + url);

    const double res_x = std::hypot(xs[1] - xs[0], ys[1] - ys[0]);
    const double res_y = std::hypot(xs[2] - xs[0], ys[2] - ys[0]);
    if (!(res_x > 0.0) || !(res_y > 0.0))
        throw std::runtime_error("fetch_raster: degenerate pixel size for " + url);
    return std::min(res_x, res_y);
}

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

}  // namespace

double fetch_raster(const RasterFetch& f, const Grid& g) {
    const std::string vsi = "/vsicurl/" + f.url;
    GDALDatasetH src = GDALOpen(vsi.c_str(), GA_ReadOnly);
    if (!src)
        throw std::runtime_error("fetch_raster: open failed: " + f.url + ": " +
                                 CPLGetLastErrorMsg());

    double res;
    try {
        res = native_ground_res_m(src, g, f.url);
    } catch (...) {
        GDALClose(src);
        throw;
    }

    CPLStringList argv;
    // -of is explicit because the warp writes to a ".partial" temporary name, and
    // GDAL's extension-based driver guess silently produces no output for an
    // unrecognised extension rather than reporting an error.
    for (const std::string& a : {std::string("-of"), std::string("GTiff"),
                                 std::string("-t_srs"), "EPSG:" + std::to_string(g.utm_epsg),
                                 std::string("-te"), num(g.padded_m.xmin), num(g.padded_m.ymin),
                                 num(g.padded_m.xmax), num(g.padded_m.ymax),
                                 std::string("-tr"), num(res), num(res),
                                 std::string("-r"), std::string(f.resampling)})
        argv.AddString(a.c_str());

    GDALWarpAppOptions* opts = GDALWarpAppOptionsNew(argv.List(), nullptr);
    if (!opts) {
        GDALClose(src);
        throw std::runtime_error("fetch_raster: bad warp options for " + f.url);
    }
    int usage_err = 0;
    GDALDatasetH dst = GDALWarp(f.out.c_str(), nullptr, 1, &src, opts, &usage_err);
    GDALWarpAppOptionsFree(opts);
    GDALClose(src);
    if (!dst || usage_err)
        throw std::runtime_error("fetch_raster: warp failed: " + f.url + ": " +
                                 CPLGetLastErrorMsg());
    GDALClose(dst);
    return res;
}
