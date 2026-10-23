#include "sources.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include <ogrsf_frmts.h>

std::string worldcover_tile_name(double lat, double lon) {
    const int lat_sw = static_cast<int>(std::floor(lat / 3.0)) * 3;
    const int lon_sw = static_cast<int>(std::floor(lon / 3.0)) * 3;
    // A precondition, not defensive padding: validate_request() has already bounded the
    // request, so a tile corner outside this range means the grid itself is wrong.
    if (lat_sw < -90 || lat_sw > 90 || lon_sw < -180 || lon_sw > 180)
        throw std::runtime_error("worldcover: tile corner out of range (" +
                                 std::to_string(lat_sw) + ", " + std::to_string(lon_sw) + ")");
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%02d%c%03d", lat_sw < 0 ? 'S' : 'N', std::abs(lat_sw),
                  lon_sw < 0 ? 'W' : 'E', std::abs(lon_sw));
    return buf;
}

std::string worldcover_tile_for(const Grid& g) {
    const std::string sw = worldcover_tile_name(g.lonlat.ymin, g.lonlat.xmin);
    const std::string ne = worldcover_tile_name(g.lonlat.ymax, g.lonlat.xmax);
    if (sw != ne)
        throw std::runtime_error(
            "worldcover: the padded tile spans more than one 3x3 degree tile (" + sw + " and " +
            ne + "); mosaicking across WorldCover tiles is not implemented");
    return sw;
}

std::string canopy_tile_for(const std::string& index_path, const Grid& g) {
    const GDALDatasetUniquePtr ds(
        GDALDataset::Open(index_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    if (!ds) throw std::runtime_error("canopy: cannot open tile index " + index_path);

    OGRLayer* layer = ds->GetLayer(0);
    if (!layer) throw std::runtime_error("canopy: tile index has no layer: " + index_path);

    layer->SetSpatialFilterRect(g.lonlat.xmin, g.lonlat.ymin, g.lonlat.xmax, g.lonlat.ymax);
    layer->ResetReading();

    std::vector<std::string> tiles;
    for (const OGRFeatureUniquePtr& f : layer) {
        const int i = f->GetFieldIndex("tile");
        if (i < 0) throw std::runtime_error("canopy: tile index has no 'tile' field: " + index_path);
        tiles.emplace_back(f->GetFieldAsString(i));
    }

    if (tiles.empty())
        throw std::runtime_error("canopy: no tile in " + index_path + " covers the padded tile; "
                                 "this location is outside the canopy map's coverage");
    if (tiles.size() > 1) {
        std::string names;
        for (const std::string& t : tiles) names += (names.empty() ? "" : ", ") + t;
        throw std::runtime_error("canopy: the padded tile spans " + std::to_string(tiles.size()) +
                                 " canopy tiles (" + names +
                                 "); mosaicking across canopy tiles is not implemented");
    }
    return tiles.front();
}
