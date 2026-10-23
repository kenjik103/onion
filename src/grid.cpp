#include "grid.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include <ogr_spatialref.h>

namespace {

struct CTDeleter {
    void operator()(OGRCoordinateTransformation* p) const {
        OGRCoordinateTransformation::DestroyCT(p);
    }
};
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, CTDeleter>;

// EPSG:4326 is officially latitude-first. Without the traditional GIS order every
// transform silently swaps its coordinates (gotcha 3).
void load_crs(OGRSpatialReference& srs, int epsg) {
    if (srs.importFromEPSG(epsg) != OGRERR_NONE)
        throw std::runtime_error("grid: importFromEPSG(" + std::to_string(epsg) + ") failed");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

TransformPtr make_transform(const OGRSpatialReference& from, const OGRSpatialReference& to) {
    TransformPtr ct(OGRCreateCoordinateTransformation(&from, &to));
    if (!ct) throw std::runtime_error("grid: failed to create coordinate transformation");
    return ct;
}

int utm_epsg_for(double lat, double lon) {
    const int zone = static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1;
    return (lat >= 0.0 ? 32600 : 32700) + zone;
}

}  // namespace

Grid make_grid(const TileRequest& r) {
    Grid g{};
    g.utm_epsg = utm_epsg_for(r.lat, r.lon);

    OGRSpatialReference wgs84;
    OGRSpatialReference utm;
    load_crs(wgs84, 4326);
    load_crs(utm, g.utm_epsg);

    const TransformPtr to_utm = make_transform(wgs84, utm);
    double cx = r.lon;
    double cy = r.lat;
    if (!to_utm->Transform(1, &cx, &cy))
        throw std::runtime_error("grid: centre lon/lat -> UTM transform failed");

    const double half = r.extent_m / 2.0;
    g.tile_m = {cx - half, cy - half, cx + half, cy + half};
    g.padded_m = {g.tile_m.xmin - PAD_M, g.tile_m.ymin - PAD_M,
                  g.tile_m.xmax + PAD_M, g.tile_m.ymax + PAD_M};

    // The local origin is the padded SW corner, so the 1 m terrain lattice and the
    // crown voxel lattice (SPEC.md 6.6) both start there by construction. No snapping.
    g.origin_x = g.padded_m.xmin;
    g.origin_y = g.padded_m.ymin;

    // All four corners, not two: UTM meridian convergence tilts the padded box in
    // lon/lat, so opposite corners alone understate the extent.
    const TransformPtr to_wgs84 = make_transform(utm, wgs84);
    double xs[4] = {g.padded_m.xmin, g.padded_m.xmax, g.padded_m.xmin, g.padded_m.xmax};
    double ys[4] = {g.padded_m.ymin, g.padded_m.ymin, g.padded_m.ymax, g.padded_m.ymax};
    if (!to_wgs84->Transform(4, xs, ys))
        throw std::runtime_error("grid: padded corners UTM -> lon/lat transform failed");

    g.lonlat = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
                *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4)};
    return g;
}
