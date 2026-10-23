#!/usr/bin/env python3
"""M5/G4 check: does every LAS point sit where the contract says it should?

Opaque points (class 2 ground, 6 building) must lie within 1 mm of the highest opaque
surface under them. Vegetation points (class 5) must NOT: they are interception points
inside a crown, so they are checked instead for lying above the opaque surface and
inside a voxel that actually carries plant area density.

Independent of the generator. The LAS is parsed from the format spec and the mesh from
FORMAT.md; for each sampled point this finds every triangle whose xy box contains it,
computes the triangle's z there, and compares the highest against the point's z, without reusing
the scanner's code.

usage: m5_verify.py <contract-dir> [n_samples]
"""
import json
import struct
import sys

import numpy as np


def read_las(path, n_sample, rng):
    with open(path, "rb") as f:
        h = f.read(375)
        assert h[:4] == b"LASF", "not a LAS file"
        data_off = struct.unpack_from("<I", h, 96)[0]
        fmt = h[104]
        plen = struct.unpack_from("<H", h, 105)[0]
        sx, sy, sz = struct.unpack_from("<ddd", h, 131)
        ox, oy, oz = struct.unpack_from("<ddd", h, 155)
        npts = struct.unpack_from("<Q", h, 247)[0]
        print(f"  LAS: format {fmt}, {plen} B/point, {npts} points, scale {sx}")

        idx = np.sort(rng.choice(npts, size=min(n_sample, npts), replace=False))
        out = np.empty((len(idx), 3))
        cls = np.empty(len(idx), np.uint8)
        oid = np.empty(len(idx), np.uint32)
        for k, i in enumerate(idx):
            f.seek(data_off + int(i) * plen)
            rec = f.read(plen)
            xi, yi, zi = struct.unpack_from("<iii", rec, 0)
            out[k] = (xi * sx + ox, yi * sy + oy, zi * sz + oz)
            cls[k] = rec[16]  # format 6: byte 15 is the flags byte, 16 is Classification
            oid[k] = struct.unpack_from("<I", rec, 30)[0]
    return out, cls, oid


def read_ply(path):
    with open(path, "rb") as f:
        nv = nf = 0
        while True:
            line = f.readline()
            if line.startswith(b"element vertex"):
                nv = int(line.split()[2])
            elif line.startswith(b"element face"):
                nf = int(line.split()[2])
            elif line.startswith(b"end_header"):
                break
        verts = np.frombuffer(f.read(nv * 12), np.float32).reshape(nv, 3)
        raw = np.frombuffer(f.read(nf * 18), np.uint8).reshape(nf, 18)
    idx = raw[:, 1:13].copy().view(np.uint32).reshape(nf, 3)
    cls = raw[:, 17].copy()
    return verts, idx, cls


def crown_hits(path, pts_local, voxel):
    """For each given local xyz, whether some crown has PAD > 0 in its voxel."""
    vx = np.floor(pts_local[:, 0] / voxel).astype(np.int64)
    vy = np.floor(pts_local[:, 1] / voxel).astype(np.int64)
    vz = np.floor(pts_local[:, 2] / voxel).astype(np.int64)
    found = np.zeros(len(pts_local), bool)
    with open(path, "rb") as f:
        assert f.read(8) == b"ONIONCRW"
        _, count = struct.unpack("<II", f.read(8))
        for _ in range(count):
            oid, ix, iy, iz, nx, ny, nz = struct.unpack("<IiiiIII", f.read(28))
            n = nx * ny * nz
            inside = (~found & (vx >= ix) & (vx < ix + nx) & (vy >= iy) &
                      (vy < iy + ny) & (vz >= iz) & (vz < iz + nz))
            if not inside.any():
                f.seek(4 * n, 1)
                continue
            pad = np.frombuffer(f.read(4 * n), np.float32).reshape(nz, ny, nx)
            for k in np.nonzero(inside)[0]:
                if pad[vz[k] - iz, vy[k] - iy, vx[k] - ix] > 0:
                    found[k] = True
    return found


def main():
    d = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    rng = np.random.default_rng(12345)

    meta = json.load(open(f"{d}/scene_geometry.json"))
    ox, oy = meta["local_origin"]
    pts, cls, oid = read_las(f"{d}/points.las", n, rng)
    verts, idx, fcls = read_ply(f"{d}/opaque.ply")
    print(f"  mesh: {len(verts)} vertices, {len(idx)} triangles")

    a = verts[idx[:, 0]].astype(np.float64)
    b = verts[idx[:, 1]].astype(np.float64)
    c = verts[idx[:, 2]].astype(np.float64)
    tminx = np.minimum(np.minimum(a[:, 0], b[:, 0]), c[:, 0])
    tmaxx = np.maximum(np.maximum(a[:, 0], b[:, 0]), c[:, 0])
    tminy = np.minimum(np.minimum(a[:, 1], b[:, 1]), c[:, 1])
    tmaxy = np.maximum(np.maximum(a[:, 1], b[:, 1]), c[:, 1])
    area = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])

    worst = 0.0
    bad = 0
    nohit = 0
    cls_mismatch = 0
    veg_below = 0
    veg_idx = []
    for k in range(len(pts)):
        px, py, pz = pts[k][0] - ox, pts[k][1] - oy, pts[k][2]
        m = (tminx <= px) & (tmaxx >= px) & (tminy <= py) & (tmaxy >= py) & (area != 0)
        if not m.any():
            nohit += 1
            continue
        aa, bb, cc, ar = a[m], b[m], c[m], area[m]
        w0 = ((bb[:, 0] - aa[:, 0]) * (py - aa[:, 1]) - (bb[:, 1] - aa[:, 1]) * (px - aa[:, 0])) / ar
        w1 = ((cc[:, 0] - bb[:, 0]) * (py - bb[:, 1]) - (cc[:, 1] - bb[:, 1]) * (px - bb[:, 0])) / ar
        w2 = ((aa[:, 0] - cc[:, 0]) * (py - cc[:, 1]) - (aa[:, 1] - cc[:, 1]) * (px - cc[:, 0])) / ar
        inside = (w0 >= -1e-12) & (w1 >= -1e-12) & (w2 >= -1e-12)
        if not inside.any():
            nohit += 1
            continue
        z = w1[inside] * aa[inside][:, 2] + w2[inside] * bb[inside][:, 2] + w0[inside] * cc[inside][:, 2]
        hi = z.max()
        if cls[k] == 5:
            # An interception inside a crown: it must sit above the opaque surface,
            # not on it. 1 mm of slack covers the LAS quantisation.
            veg_idx.append(k)
            if pz < hi - 0.001:
                veg_below += 1
            continue
        err = abs(hi - pz)
        worst = max(worst, err)
        if err > 0.001:
            bad += 1
        if fcls[m][inside][np.argmax(z)] != cls[k]:
            cls_mismatch += 1

    n_op = len(pts) - len(veg_idx)
    print(f"\n  sampled {len(pts)} points: {n_op} opaque (class 2/6), "
          f"{len(veg_idx)} vegetation (class 5)")
    print(f"  opaque: no containing triangle      : {nohit}")
    print(f"  opaque: further than 1 mm from surface : {bad}")
    print(f"  opaque: worst vertical error        : {worst*1000:.4f} mm")
    print(f"  opaque: class disagreements         : {cls_mismatch}")

    veg_bad_voxel = 0
    if veg_idx:
        local = np.column_stack([pts[veg_idx, 0] - ox, pts[veg_idx, 1] - oy, pts[veg_idx, 2]])
        voxel = meta["crown_archetype"]["voxel_size_m"]
        hit = crown_hits(f"{d}/crowns.bin", local, voxel)
        veg_bad_voxel = int((~hit).sum())
        print(f"  vegetation: below the opaque surface : {veg_below}")
        print(f"  vegetation: in a voxel with no PAD   : {veg_bad_voxel}")
    ok = bad == 0 and nohit == 0 and veg_below == 0 and veg_bad_voxel == 0
    print("\n" + ("PASS: opaque points lie on a contract surface within 1 mm; "
                  "vegetation points lie inside crown voxels above it" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
