#include "scene_spec.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

#include <cpl_json.h>
#include <ogrsf_frmts.h>

#include "cache.h"
#include "raster.h"
#include "trees.h"

namespace {

CPLJSONArray xy_ring(const OGRLinearRing* ring) {
    CPLJSONArray out;
    // Drop the duplicated closing vertex: the ring is implicitly closed.
    const int n = ring->getNumPoints() > 1 ? ring->getNumPoints() - 1 : 0;
    for (int i = 0; i < n; ++i) {
        CPLJSONArray p;
        p.Add(ring->getX(i));
        p.Add(ring->getY(i));
        out.Add(p);
    }
    return out;
}

// One entry per polygon part, all parts of a building sharing its id, so a multi-part
// building stays one object (SPEC.md 6.7) without losing geometry. Measured on the test
// tiles: 794 of 795 Fargo buildings are single-part, one has two parts, and no building
// has an interior ring.
CPLJSONArray read_buildings(const std::string& path) {
    const GDALDatasetUniquePtr ds(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    if (!ds) throw std::runtime_error("scene spec: cannot open " + path);
    OGRLayer* layer = ds->GetLayer(0);
    if (!layer) throw std::runtime_error("scene spec: no layer in " + path);

    CPLJSONArray out;
    layer->ResetReading();
    for (const OGRFeatureUniquePtr& f : layer) {
        const std::string id = f->GetFieldAsString(0);
        const OGRGeometry* geom = f->GetGeometryRef();
        if (!geom) throw std::runtime_error("scene spec: building " + id + " has no geometry");

        std::vector<const OGRPolygon*> parts;
        if (const auto* mp = dynamic_cast<const OGRMultiPolygon*>(geom)) {
            for (const OGRPolygon* p : *mp) parts.push_back(p);
        } else if (const auto* p = dynamic_cast<const OGRPolygon*>(geom)) {
            parts.push_back(p);
        } else {
            throw std::runtime_error("scene spec: building " + id + " is not areal");
        }

        for (const OGRPolygon* p : parts) {
            // Courtyards would extrude as solid. None occur in the test tiles, so this
            // fails loudly rather than producing silently wrong geometry (SPEC.md 4.2).
            if (p->getNumInteriorRings() != 0)
                throw std::runtime_error("scene spec: building " + id +
                                         " has an interior ring; extruding footprints with "
                                         "holes is not implemented");
            CPLJSONObject b;
            b.Add("id", id);
            b.Add("polygon_utm", xy_ring(p->getExteriorRing()));
            b.Add("height_m", BUILDING_HEIGHT_M);
            out.Add(b);
        }
    }
    return out;
}

}  // namespace

void write_scene_spec(const TileRequest& r, const Grid& g, const std::string& cache,
                      const std::string& out_dir) {
    const Raster chm = load_raster(cache_path(cache, "chm.tif"));
    const Raster landcover = load_raster(cache_path(cache, "landcover.tif"));
    const Raster terrain = load_raster(cache_path(cache, "terrain.tif"));

    const std::vector<Tree> trees = detect_trees(chm, landcover, terrain, g.padded_m);
    std::printf("  trees        %zu detected\n", trees.size());

    CPLJSONArray tree_arr;
    for (const Tree& t : trees) {
        CPLJSONObject o;
        o.Add("x", t.x);
        o.Add("y", t.y);
        o.Add("ground_z", t.ground_z);
        o.Add("height_m", t.height_m);
        o.Add("crown_radius_m", t.crown_radius_m);
        tree_arr.Add(o);
    }

    const CPLJSONArray buildings = read_buildings(cache_path(cache, "buildings.fgb"));
    std::printf("  buildings    %d footprint parts\n", buildings.Size());

    CPLJSONDocument doc;
    CPLJSONObject root = doc.GetRoot();

    CPLJSONObject jg;
    jg.Add("utm_epsg", g.utm_epsg);
    CPLJSONArray bbox;
    for (const double v : {g.padded_m.xmin, g.padded_m.ymin, g.padded_m.xmax, g.padded_m.ymax})
        bbox.Add(v);
    jg.Add("padded_bbox_m", bbox);
    CPLJSONArray origin;
    origin.Add(g.origin_x);
    origin.Add(g.origin_y);
    jg.Add("local_origin", origin);
    root.Add("grid", jg);

    // Rasters stay as paths into the cache, exactly as SPEC.md 6.4 specifies.
    CPLJSONObject rasters;
    rasters.Add("terrain", cache_path(cache, "terrain.tif"));
    rasters.Add("landcover", cache_path(cache, "landcover.tif"));
    rasters.Add("chm", cache_path(cache, "chm.tif"));
    root.Add("rasters", rasters);

    root.Add("buildings", buildings);
    root.Add("trees", tree_arr);

    CPLJSONDocument prov;
    if (!prov.Load(cache_path(cache, "provenance.json")))
        throw std::runtime_error("scene spec: cannot read provenance.json in " + cache);
    root.Add("provenance", prov.GetRoot());

    CPLJSONObject req;
    req.Add("lat", r.lat);
    req.Add("lon", r.lon);
    req.Add("extent_m", r.extent_m);
    req.Add("seed", static_cast<GInt64>(r.seed));
    root.Add("request", req);

    const std::string path = out_dir + "/scene_spec.json";
    if (!doc.Save(path)) throw std::runtime_error("scene spec: cannot write " + path);
    std::printf("  scene spec   %s\n", path.c_str());
}
