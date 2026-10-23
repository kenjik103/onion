// M1 acceptance checks (SPEC.md 9). Plain checks, no test framework: SPEC.md 4.6
// allows no dependencies beyond GDAL, DuckDB and Polyscope.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include <ogr_spatialref.h>

#include "grid.h"
#include "request.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

struct CTDeleter {
    void operator()(OGRCoordinateTransformation* p) const {
        OGRCoordinateTransformation::DestroyCT(p);
    }
};
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, CTDeleter>;

void load_crs(OGRSpatialReference& srs, int epsg) {
    srs.importFromEPSG(epsg);
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

// UTM -> lon/lat -> UTM. The distance between the two UTM points is the round-trip
// error in metres, which is the unit the acceptance criterion is stated in.
double roundtrip_error_m(int epsg, double x, double y) {
    OGRSpatialReference wgs84;
    OGRSpatialReference utm;
    load_crs(wgs84, 4326);
    load_crs(utm, epsg);

    const TransformPtr inv(OGRCreateCoordinateTransformation(&utm, &wgs84));
    const TransformPtr fwd(OGRCreateCoordinateTransformation(&wgs84, &utm));
    if (!inv || !fwd) throw std::runtime_error("test: failed to build transformations");

    double lon = x;
    double lat = y;
    if (!inv->Transform(1, &lon, &lat)) throw std::runtime_error("test: UTM -> lon/lat failed");
    double x2 = lon;
    double y2 = lat;
    if (!fwd->Transform(1, &x2, &y2)) throw std::runtime_error("test: lon/lat -> UTM failed");
    return std::hypot(x2 - x, y2 - y);
}

// Centre plus all four padded corners.
double worst_roundtrip_m(const Grid& g) {
    const double xs[5] = {(g.tile_m.xmin + g.tile_m.xmax) / 2.0, g.padded_m.xmin,
                          g.padded_m.xmax, g.padded_m.xmin, g.padded_m.xmax};
    const double ys[5] = {(g.tile_m.ymin + g.tile_m.ymax) / 2.0, g.padded_m.ymin,
                          g.padded_m.ymin, g.padded_m.ymax, g.padded_m.ymax};
    double worst = 0.0;
    for (int i = 0; i < 5; ++i) worst = std::max(worst, roundtrip_error_m(g.utm_epsg, xs[i], ys[i]));
    return worst;
}

void expect_rejected(const TileRequest& r, const std::string& what) {
    try {
        validate_request(r);
    } catch (const std::exception&) {
        check(true, "rejected: " + what);
        return;
    }
    check(false, "rejected: " + what);
}

void test_fargo() {
    const TileRequest r{46.88, -96.79, 1000.0, 1};
    validate_request(r);
    const Grid g = make_grid(r);

    check(g.utm_epsg == 32614, "Fargo (46.88, -96.79) -> EPSG:32614, got " + std::to_string(g.utm_epsg));

    const double worst = worst_roundtrip_m(g);
    check(worst < 1.0, "lon/lat <-> UTM round trip < 1 m at centre and padded corners (worst " +
                           fmt(worst) + " m)");

    check(std::abs((g.tile_m.xmax - g.tile_m.xmin) - r.extent_m) < 1e-6 &&
              std::abs((g.tile_m.ymax - g.tile_m.ymin) - r.extent_m) < 1e-6,
          "tile box is extent_m square");

    check(std::abs((g.tile_m.xmin - g.padded_m.xmin) - PAD_M) < 1e-9 &&
              std::abs((g.tile_m.ymin - g.padded_m.ymin) - PAD_M) < 1e-9 &&
              std::abs((g.padded_m.xmax - g.tile_m.xmax) - PAD_M) < 1e-9 &&
              std::abs((g.padded_m.ymax - g.tile_m.ymax) - PAD_M) < 1e-9,
          "padded box contains the tile by exactly PAD_M on all four sides");

    check(g.origin_x == g.padded_m.xmin && g.origin_y == g.padded_m.ymin,
          "local origin is the padded SW corner");

    check(g.lonlat.xmin < r.lon && r.lon < g.lonlat.xmax && g.lonlat.ymin < r.lat &&
              r.lat < g.lonlat.ymax,
          "lon/lat bbox contains the request centre");
}

void test_southern_hemisphere() {
    const TileRequest r{-33.87, 151.21, 1000.0, 7};  // Sydney
    validate_request(r);
    const Grid g = make_grid(r);
    check(g.utm_epsg == 32756, "Sydney (-33.87, 151.21) -> EPSG:32756, got " + std::to_string(g.utm_epsg));
    const double worst = worst_roundtrip_m(g);
    check(worst < 1.0, "southern round trip < 1 m (worst " + fmt(worst) + " m)");
}

void test_validation() {
    expect_rejected({91.0, -96.79, 1000.0, 0}, "latitude above GEDTM30 coverage");
    expect_rejected({-70.0, -96.79, 1000.0, 0}, "latitude below GEDTM30 coverage");
    expect_rejected({std::nan(""), -96.79, 1000.0, 0}, "NaN latitude");
    expect_rejected({46.88, 180.0, 1000.0, 0}, "longitude at the antimeridian");
    expect_rejected({46.88, -181.0, 1000.0, 0}, "longitude below -180");
    expect_rejected({46.88, -96.79, 0.0, 0}, "zero extent");
    expect_rejected({46.88, -96.79, -5.0, 0}, "negative extent");
}

}  // namespace

int main() {
    try {
        test_fargo();
        test_southern_hemisphere();
        test_validation();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
