#include "buildings.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

#include <duckdb.hpp>
#include <ogrsf_frmts.h>

namespace {

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

}  // namespace

size_t fetch_buildings(const Grid& g, const std::string& release, const std::string& out) {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    // INSTALL needs the network, which is fine: only `fetch` uses DuckDB (gotcha 8).
    auto setup = con.Query("INSTALL spatial; LOAD spatial; INSTALL httpfs; LOAD httpfs; "
                           "SET s3_region='us-west-2';");
    if (setup->HasError()) throw std::runtime_error("buildings: DuckDB setup: " + setup->GetError());

    // bbox is a struct column Overture provides for row-group pruning; the partition is
    // flat, so this filter is the only thing narrowing the read. ORDER BY id makes the
    // output byte-identical across runs, which SPEC.md 4.3 requires.
    const std::string sql =
        "SELECT id, ST_AsText(geometry) FROM read_parquet("
        "'s3://overturemaps-us-west-2/release/" + release + "/theme=buildings/type=building/*') "
        "WHERE bbox.xmin < " + num(g.lonlat.xmax) + " AND bbox.xmax > " + num(g.lonlat.xmin) +
        " AND bbox.ymin < " + num(g.lonlat.ymax) + " AND bbox.ymax > " + num(g.lonlat.ymin) +
        " ORDER BY id";

    auto res = con.Query(sql);
    if (res->HasError()) throw std::runtime_error("buildings: query failed: " + res->GetError());

    OGRSpatialReference wgs84;
    OGRSpatialReference utm;
    if (wgs84.importFromEPSG(4326) != OGRERR_NONE || utm.importFromEPSG(g.utm_epsg) != OGRERR_NONE)
        throw std::runtime_error("buildings: cannot load CRS");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // gotcha 3
    utm.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("FlatGeobuf");
    if (!drv) throw std::runtime_error("buildings: FlatGeobuf driver not available");

    GDALDatasetUniquePtr ds(drv->Create(out.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!ds) throw std::runtime_error("buildings: cannot create " + out);

    // No spatial index: a tile holds a few hundred buildings, and omitting it keeps the
    // bytes a straight function of the feature order.
    CPLStringList layer_opts;
    layer_opts.AddNameValue("SPATIAL_INDEX", "NO");
    OGRLayer* layer = ds->CreateLayer("buildings", &utm, wkbMultiPolygon, layer_opts.List());
    if (!layer) throw std::runtime_error("buildings: cannot create layer in " + out);

    OGRFieldDefn id_field("id", OFTString);
    if (layer->CreateField(&id_field) != OGRERR_NONE)
        throw std::runtime_error("buildings: cannot create id field");

    size_t written = 0;
    for (duckdb::idx_t i = 0; i < res->RowCount(); ++i) {
        const std::string id = res->GetValue(0, i).ToString();
        const std::string wkt = res->GetValue(1, i).ToString();

        OGRGeometry* raw = nullptr;
        const char* wkt_ptr = wkt.c_str();
        if (OGRGeometryFactory::createFromWkt(&wkt_ptr, &wgs84, &raw) != OGRERR_NONE || !raw)
            throw std::runtime_error("buildings: unparseable geometry for id " + id);
        std::unique_ptr<OGRGeometry> geom(raw);

        if (geom->transformTo(&utm) != OGRERR_NONE)
            throw std::runtime_error("buildings: cannot reproject geometry for id " + id);

        std::unique_ptr<OGRGeometry> multi(OGRGeometryFactory::forceToMultiPolygon(geom.release()));
        if (!multi) throw std::runtime_error("buildings: geometry for id " + id + " is not areal");

        const OGRFeatureUniquePtr f(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        f->SetField(0, id.c_str());
        if (f->SetGeometry(multi.get()) != OGRERR_NONE ||
            layer->CreateFeature(f.get()) != OGRERR_NONE)
            throw std::runtime_error("buildings: cannot write feature for id " + id);
        ++written;
    }

    ds.reset();  // flush before the caller hashes the file
    return written;
}
