#include <cstdint>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <stdexcept>
#include <string>

#include <cpl_conv.h>
#include <ogr_spatialref.h>
#include <gdal_priv.h>

#include "fetch.h"
#include "grid.h"
#include "request.h"
#include "builders.h"
#include "contract_read.h"
#include "contract_write.h"
#include "las.h"
#include "manifest.h"
#include "scanner.h"
#include "scene_spec.h"

namespace {

const char* const USAGE =
    "usage:\n"
    "  onion fetch    --lat <deg> --lon <deg> --extent <m> --seed <n> --cache <dir>\n"
    "  onion generate --lat <deg> --lon <deg> --extent <m> --seed <n> --cache <dir> --out <dir>\n";

[[noreturn]] void usage_error(const std::string& msg) {
    throw std::runtime_error(msg + "\n" + USAGE);
}

using Flags = std::map<std::string, std::string>;

Flags parse_flags(int argc, char** argv) {
    Flags flags;
    for (int i = 0; i < argc; i += 2) {
        const std::string key = argv[i];
        if (key.rfind("--", 0) != 0 || key.size() <= 2)
            usage_error("expected a --flag, got: " + key);
        if (i + 1 >= argc) usage_error("flag has no value: " + key);
        if (!flags.emplace(key.substr(2), argv[i + 1]).second)
            usage_error("flag given more than once: " + key);
    }
    return flags;
}

// Flags are consumed as they are read so whatever is left over is unknown.
std::string take(Flags& flags, const std::string& name) {
    const auto it = flags.find(name);
    if (it == flags.end()) usage_error("missing required flag: --" + name);
    const std::string value = it->second;
    flags.erase(it);
    return value;
}

double take_double(Flags& flags, const std::string& name) {
    const std::string s = take(flags, name);
    try {
        size_t n = 0;
        const double v = std::stod(s, &n);
        if (n != s.size()) throw std::invalid_argument("trailing characters");
        return v;
    } catch (const std::exception&) {
        usage_error("--" + name + " is not a number: " + s);
    }
}

uint64_t take_uint64(Flags& flags, const std::string& name) {
    const std::string s = take(flags, name);
    // std::stoull accepts a leading '-' and wraps it to a huge value, so reject the
    // sign before parsing rather than letting a negative seed through silently.
    if (!s.empty() && s[0] == '-')
        usage_error("--" + name + " is not a non-negative integer: " + s);
    try {
        size_t n = 0;
        const unsigned long long v = std::stoull(s, &n);
        if (n != s.size()) throw std::invalid_argument("trailing characters");
        return v;
    } catch (const std::exception&) {
        usage_error("--" + name + " is not a non-negative integer: " + s);
    }
}

void reject_unknown(const Flags& flags) {
    if (!flags.empty()) usage_error("unknown flag: --" + flags.begin()->first);
}

TileRequest take_request(Flags& flags) {
    TileRequest r{};
    r.lat = take_double(flags, "lat");
    r.lon = take_double(flags, "lon");
    r.extent_m = take_double(flags, "extent");
    r.seed = take_uint64(flags, "seed");
    return r;
}

void print_grid(const TileRequest& r, const Grid& g) {
    std::printf("request    lat %.6f  lon %.6f  extent %.1f m  seed %llu\n",
                r.lat, r.lon, r.extent_m, static_cast<unsigned long long>(r.seed));
    std::printf("utm_epsg   %d\n", g.utm_epsg);
    std::printf("tile_m     %.3f %.3f  ->  %.3f %.3f\n",
                g.tile_m.xmin, g.tile_m.ymin, g.tile_m.xmax, g.tile_m.ymax);
    std::printf("padded_m   %.3f %.3f  ->  %.3f %.3f\n",
                g.padded_m.xmin, g.padded_m.ymin, g.padded_m.xmax, g.padded_m.ymax);
    std::printf("origin     %.3f %.3f\n", g.origin_x, g.origin_y);
    std::printf("lonlat     %.7f %.7f  ->  %.7f %.7f\n",
                g.lonlat.xmin, g.lonlat.ymin, g.lonlat.xmax, g.lonlat.ymax);
}

void cmd_fetch(Flags& flags) {
    const TileRequest r = take_request(flags);
    const std::string cache = take(flags, "cache");
    reject_unknown(flags);

    validate_request(r);
    print_grid(r, make_grid(r));
    std::printf("cache      %s\n", cache.c_str());
    run_fetch(r, cache);
}

void cmd_generate(Flags& flags) {
    const TileRequest r = take_request(flags);
    const std::string cache = take(flags, "cache");
    const std::string out = take(flags, "out");
    reject_unknown(flags);

    validate_request(r);
    const Grid g = make_grid(r);
    print_grid(r, g);
    std::printf("cache      %s\nout        %s\n", cache.c_str(), out.c_str());

    // The scene spec is the only input to the builders; they never read the cached raw
    // pulls directly (SPEC.md 4.4).
    const SceneSpec spec = write_scene_spec(r, g, cache, out);

    // Builders -> scene geometry contract (SPEC.md 6.5-6.8).
    const Raster terrain = load_raster(spec.terrain_path);
    Mesh mesh;
    std::vector<ObjectRow> objects;

    objects.push_back({0, CLASS_GROUND, "gedtm30"});
    build_terrain(terrain, g, 0, mesh);
    std::printf("  terrain      %zu vertices, %zu triangles\n", mesh.vertex_count(),
                mesh.face_count());

    const size_t ground_faces = mesh.face_count();
    const uint32_t next_oid = build_buildings(spec.buildings, terrain, g, 1, mesh, objects);
    std::printf("  buildings    %zu triangles, %u kept of %zu\n",
                mesh.face_count() - ground_faces, next_oid - 1, spec.buildings.size());

    const std::vector<Crown> crowns = build_crowns(spec.trees, g, next_oid, objects);
    size_t voxels = 0;
    for (const Crown& c : crowns) voxels += c.pad.size();
    std::printf("  crowns       %zu crowns, %zu voxels\n", crowns.size(), voxels);

    write_opaque_ply(mesh, out + "/opaque.ply");
    write_crowns_bin(crowns, out + "/crowns.bin");
    write_scene_geometry_json(g, objects, out + "/scene_geometry.json");
    std::printf("  contract     opaque.ply, crowns.bin, scene_geometry.json (v%d)\n",
                CONTRACT_FORMAT_VERSION);

    // The scanner reads the contract and nothing else (SPEC.md 4.4), so the files are
    // read back rather than the in-memory mesh reused. That exercises the round trip on
    // every run.
    const ContractMesh scene = read_opaque_ply(out + "/opaque.ply");

    // Samples cover the tile, not the padded box (SPEC.md 6.9). floor, because the
    // extent need not be a whole number of 0.14 m cells; the leftover strip under one
    // cell wide is not sampled.
    ScanGrid sg;
    sg.x0 = g.tile_m.xmin - g.origin_x;
    sg.y0 = g.tile_m.ymin - g.origin_y;
    sg.nx = static_cast<int>(std::floor(r.extent_m / SCAN_SPACING_M));
    sg.ny = sg.nx;
    sg.seed = r.seed;
    OpaqueHits hits = scan_opaque(scene, scene.object_id, sg);
    std::printf("  scan         %d x %d samples, %zu opaque hits\n", sg.nx, sg.ny,
                hits.hit_count);

    // Vegetation, read back from the contract like the mesh (SPEC.md 4.4).
    const ContractMeta cmeta = read_scene_geometry(out + "/scene_geometry.json");
    const std::vector<CrownBlock> crown_blocks = read_crowns(out + "/crowns.bin");
    scan_vegetation(crown_blocks, cmeta.voxel_size_m, cmeta.crown_g, sg, hits);
    size_t veg = 0;
    for (const uint8_t c : hits.cls)
        if (c == 5) ++veg;
    std::printf("  vegetation   %zu of %zu points intercepted by crowns (%.1f%%)\n", veg,
                hits.hit_count, 100.0 * veg / std::max<size_t>(hits.hit_count, 1));

    double bounds[6] = {1e308, -1e308, 1e308, -1e308, 1e308, -1e308};
    for (int j = 0; j < sg.ny; ++j)
        for (int i = 0; i < sg.nx; ++i) {
            const size_t s = static_cast<size_t>(j) * sg.nx + i;
            if (hits.z[s] == SCAN_MISS_Z) continue;
            double sx, sy;
            sample_xy(sg, i, j, sx, sy);
            const double wx = sx + g.origin_x;
            const double wy = sy + g.origin_y;
            bounds[0] = std::min(bounds[0], wx); bounds[1] = std::max(bounds[1], wx);
            bounds[2] = std::min(bounds[2], wy); bounds[3] = std::max(bounds[3], wy);
            bounds[4] = std::min<double>(bounds[4], hits.z[s]);
            bounds[5] = std::max<double>(bounds[5], hits.z[s]);
        }

    OGRSpatialReference srs;
    if (srs.importFromEPSG(g.utm_epsg) != OGRERR_NONE)
        throw std::runtime_error("generate: cannot load EPSG for the LAS CRS VLR");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* wkt_raw = nullptr;
    if (srs.exportToWkt(&wkt_raw) != OGRERR_NONE || !wkt_raw)
        throw std::runtime_error("generate: cannot export WKT for the LAS CRS VLR");
    const std::string wkt(wkt_raw);
    CPLFree(wkt_raw);

    LasOut las = las_begin(out + "/points.las", g.origin_x, g.origin_y, wkt,
                           hits.hit_count, bounds);
    for (int j = 0; j < sg.ny; ++j)
        for (int i = 0; i < sg.nx; ++i) {
            const size_t s = static_cast<size_t>(j) * sg.nx + i;
            if (hits.z[s] == SCAN_MISS_Z) continue;
            double sx, sy;
            sample_xy(sg, i, j, sx, sy);
            las_point(las, {sx + g.origin_x, sy + g.origin_y, hits.z[s],
                            hits.object_id[s], hits.cls[s]});
        }
    las_end(las);
    std::printf("  las          points.las, %zu points\n", hits.hit_count);

    write_manifest(r, g, cache, out, hits.hit_count);
    std::printf("  manifest     manifest.json\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Process-global, so both are set once here rather than inside a fetcher (gotcha 4).
    CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
    GDALAllRegister();

    try {
        if (argc < 2) usage_error("no subcommand given");
        const std::string cmd = argv[1];
        Flags flags = parse_flags(argc - 2, argv + 2);

        if (cmd == "fetch") {
            cmd_fetch(flags);
        } else if (cmd == "generate") {
            cmd_generate(flags);
        } else {
            usage_error("unknown subcommand: " + cmd);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
