#include "contract_read.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <cpl_json.h>

namespace {

using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

FilePtr open_r(const std::string& p) {
    FilePtr f(std::fopen(p.c_str(), "rb"), std::fclose);
    if (!f) throw std::runtime_error("contract: cannot open " + p);
    return f;
}

void get(std::FILE* f, void* dst, size_t n, const std::string& p) {
    if (std::fread(dst, 1, n, f) != n) throw std::runtime_error("contract: short read: " + p);
}

}  // namespace

ContractMeta read_scene_geometry(const std::string& path) {
    CPLJSONDocument doc;
    if (!doc.Load(path)) throw std::runtime_error("contract: cannot parse " + path);
    const CPLJSONObject root = doc.GetRoot();

    ContractMeta m;
    m.format_version = root.GetInteger("format_version", -1);
    if (m.format_version != 1)
        throw std::runtime_error("contract: unsupported format_version " +
                                 std::to_string(m.format_version) + " in " + path);
    m.epsg = root.GetInteger("epsg", 0);

    const CPLJSONArray o = root.GetArray("local_origin");
    if (o.Size() != 2) throw std::runtime_error("contract: bad local_origin in " + path);
    m.origin_x = o[0].ToDouble();
    m.origin_y = o[1].ToDouble();

    const CPLJSONArray b = root.GetArray("padded_bbox_local");
    if (b.Size() != 4) throw std::runtime_error("contract: bad padded_bbox_local in " + path);
    for (int i = 0; i < 4; ++i) m.bbox[i] = b[i].ToDouble();

    const CPLJSONObject a = root.GetObj("crown_archetype");
    m.crown_pad = a.GetDouble("pad", 0);
    m.crown_g = a.GetDouble("g", 0);
    m.voxel_size_m = a.GetDouble("voxel_size_m", 0);
    m.crown_base_ratio = a.GetDouble("crown_base_ratio", 0);
    if (!(m.voxel_size_m > 0))
        throw std::runtime_error("contract: crown_archetype.voxel_size_m missing in " + path);

    m.n_objects = static_cast<size_t>(root.GetArray("objects").Size());
    return m;
}

ContractMesh read_opaque_ply(const std::string& path) {
    const FilePtr f = open_r(path);

    size_t nv = 0, nf = 0;
    bool in_face = false;
    char line[512];
    while (std::fgets(line, sizeof line, f.get())) {
        if (std::sscanf(line, "element vertex %zu", &nv) == 1) { in_face = false; continue; }
        if (std::sscanf(line, "element face %zu", &nf) == 1) { in_face = true; continue; }
        (void)in_face;
        if (std::strncmp(line, "end_header", 10) == 0) break;
    }
    if (nv == 0) throw std::runtime_error("contract: no vertices in " + path);

    ContractMesh m;
    m.xyz.resize(nv * 3);
    get(f.get(), m.xyz.data(), m.xyz.size() * sizeof(float), path);

    m.indices.resize(nf * 3);
    m.cls.resize(nf);
    for (size_t i = 0; i < nf; ++i) {
        uint8_t n = 0;
        get(f.get(), &n, 1, path);
        if (n != 3) throw std::runtime_error("contract: non-triangular face in " + path);
        get(f.get(), &m.indices[3 * i], 3 * sizeof(uint32_t), path);
        uint32_t oid = 0;
        get(f.get(), &oid, sizeof(uint32_t), path);  // object_id, unused by the viewer
        get(f.get(), &m.cls[i], 1, path);
    }
    return m;
}

CrownPoints read_crown_points(const std::string& path, double voxel_size, size_t max_points) {
    const FilePtr f = open_r(path);

    char magic[8];
    get(f.get(), magic, 8, path);
    if (std::memcmp(magic, "ONIONCRW", 8) != 0)
        throw std::runtime_error("contract: bad magic in " + path);
    uint32_t version = 0, count = 0;
    get(f.get(), &version, 4, path);
    get(f.get(), &count, 4, path);
    if (version != 1)
        throw std::runtime_error("contract: unsupported crowns.bin version " +
                                 std::to_string(version));

    CrownPoints out;
    out.crown_count = count;

    // Two passes: the first counts non-empty voxels so the decimation stride can be
    // chosen up front, the second collects every stride-th one.
    const long data_start = std::ftell(f.get());
    std::vector<float> block;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            out.stride = out.total_nonempty > max_points
                             ? static_cast<int>((out.total_nonempty + max_points - 1) / max_points)
                             : 1;
            out.xyz.reserve(3 * std::min(out.total_nonempty, max_points) + 3);
            std::fseek(f.get(), data_start, SEEK_SET);
        }
        size_t seen = 0;
        for (uint32_t c = 0; c < count; ++c) {
            uint32_t oid = 0, dim[3] = {0, 0, 0};
            int32_t org[3] = {0, 0, 0};
            get(f.get(), &oid, 4, path);
            get(f.get(), org, 12, path);
            get(f.get(), dim, 12, path);
            const size_t n = static_cast<size_t>(dim[0]) * dim[1] * dim[2];
            block.resize(n);
            get(f.get(), block.data(), n * sizeof(float), path);
            if (pass == 0) {
                out.total_voxels += n;
                for (const float v : block)
                    if (v > 0.0f) ++out.total_nonempty;
                continue;
            }
            size_t k = 0;
            for (uint32_t kz = 0; kz < dim[2]; ++kz)
                for (uint32_t ky = 0; ky < dim[1]; ++ky)
                    for (uint32_t kx = 0; kx < dim[0]; ++kx, ++k) {
                        if (block[k] <= 0.0f) continue;
                        if (seen++ % out.stride) continue;
                        // FORMAT.md: voxel (i,j,k) spans [i*s,(i+1)*s); centre is +0.5.
                        out.xyz.push_back(static_cast<float>((org[0] + kx + 0.5) * voxel_size));
                        out.xyz.push_back(static_cast<float>((org[1] + ky + 0.5) * voxel_size));
                        out.xyz.push_back(static_cast<float>((org[2] + kz + 0.5) * voxel_size));
                        out.pad.push_back(block[k]);
                    }
        }
    }
    return out;
}
