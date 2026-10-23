#!/usr/bin/env python3
"""M5 check: is every LAS point within 1 mm of a contract surface?

Independent of the generator. The LAS is parsed from the format spec and the mesh from
FORMAT.md; for each sampled point this finds every triangle whose xy box contains it,
computes the triangle's z there, and compares the highest against the point's z. That is
the scanner's defining property -- highest opaque surface under the sample -- rechecked
without reusing the scanner's code.

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
        err = abs(hi - pz)
        worst = max(worst, err)
        if err > 0.001:
            bad += 1
        if fcls[m][inside][np.argmax(z)] != cls[k]:
            cls_mismatch += 1

    print(f"\n  sampled {len(pts)} points")
    print(f"  points with no containing triangle : {nohit}")
    print(f"  points further than 1 mm from the highest surface : {bad}")
    print(f"  worst vertical error : {worst*1000:.4f} mm")
    print(f"  class disagreements with the highest triangle : {cls_mismatch}")
    ok = bad == 0 and nohit == 0
    print("\n" + ("PASS: every sampled point lies on a contract surface within 1 mm"
                  if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
