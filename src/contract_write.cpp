#include "contract_write.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

#include <cpl_json.h>

namespace {

using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

FilePtr open_w(const std::string& path) {
    FilePtr f(std::fopen(path.c_str(), "wb"), std::fclose);
    if (!f) throw std::runtime_error("contract: cannot write " + path);
    return f;
}

void put(std::FILE* f, const void* p, size_t n, const std::string& path) {
    if (std::fwrite(p, 1, n, f) != n) throw std::runtime_error("contract: write failed: " + path);
}

}  // namespace

// Binary little-endian PLY. One uint32 object_id and one uint8 class per face, so every
// primitive carries both (SPEC.md 6.8).
void write_opaque_ply(const Mesh& m, const std::string& path) {
    const FilePtr f = open_w(path);
    const std::string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "comment onion scene geometry contract version " +
        std::to_string(CONTRACT_FORMAT_VERSION) +
        "\n"
        "element vertex " + std::to_string(m.vertex_count()) + "\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face " + std::to_string(m.face_count()) + "\n"
        "property list uchar uint vertex_indices\n"
        "property uint object_id\n"
        "property uchar class\n"
        "end_header\n";
    put(f.get(), header.data(), header.size(), path);
    put(f.get(), m.xyz.data(), m.xyz.size() * sizeof(float), path);

    for (size_t i = 0; i < m.face_count(); ++i) {
        const uint8_t n = 3;
        put(f.get(), &n, 1, path);
        put(f.get(), &m.indices[3 * i], 3 * sizeof(uint32_t), path);
        put(f.get(), &m.object_id[i], sizeof(uint32_t), path);
        put(f.get(), &m.cls[i], 1, path);
    }
}

// One record per crown: a fixed header then nx*ny*nz float32 PAD values, x fastest.
void write_crowns_bin(const std::vector<Crown>& crowns, const std::string& path) {
    const FilePtr f = open_w(path);
    const char magic[8] = {'O', 'N', 'I', 'O', 'N', 'C', 'R', 'W'};
    put(f.get(), magic, 8, path);
    const uint32_t version = CONTRACT_FORMAT_VERSION;
    const uint32_t count = static_cast<uint32_t>(crowns.size());
    put(f.get(), &version, 4, path);
    put(f.get(), &count, 4, path);

    for (const Crown& c : crowns) {
        put(f.get(), &c.object_id, 4, path);
        const int32_t org[3] = {c.ix, c.iy, c.iz};
        put(f.get(), org, 12, path);
        const uint32_t dim[3] = {c.nx, c.ny, c.nz};
        put(f.get(), dim, 12, path);
        put(f.get(), c.pad.data(), c.pad.size() * sizeof(float), path);
    }
}

void write_scene_geometry_json(const Grid& g, const std::vector<ObjectRow>& objects,
                               const std::string& path) {
    CPLJSONDocument doc;
    CPLJSONObject root = doc.GetRoot();
    root.Add("format_version", CONTRACT_FORMAT_VERSION);
    root.Add("epsg", g.utm_epsg);

    CPLJSONArray origin;
    origin.Add(g.origin_x);
    origin.Add(g.origin_y);
    root.Add("local_origin", origin);

    // Padded bounding box in LOCAL coordinates, which is what the contract geometry uses.
    CPLJSONArray bbox;
    for (const double v : {0.0, 0.0, g.padded_m.xmax - g.padded_m.xmin,
                           g.padded_m.ymax - g.padded_m.ymin})
        bbox.Add(v);
    root.Add("padded_bbox_local", bbox);

    CPLJSONObject arch;
    arch.Add("pad", CROWN_PAD);
    arch.Add("g", CROWN_G);
    arch.Add("voxel_size_m", CROWN_VOXEL_SIZE_M);
    arch.Add("crown_base_ratio", CROWN_BASE_RATIO);
    arch.Add("envelope", "ellipsoid");
    root.Add("crown_archetype", arch);

    CPLJSONArray table;
    for (const ObjectRow& o : objects) {
        CPLJSONObject r;
        r.Add("object_id", static_cast<GInt64>(o.object_id));
        r.Add("class", o.cls);
        r.Add("source", o.source);
        table.Add(r);
    }
    root.Add("objects", table);

    if (!doc.Save(path)) throw std::runtime_error("contract: cannot write " + path);
}
