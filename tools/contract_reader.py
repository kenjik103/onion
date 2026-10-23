#!/usr/bin/env python3
"""Independent reader for the scene geometry contract.

Written ONLY from FORMAT.md. It does not import, include, or consult any generator
source. If this script loads a scene, FORMAT.md is sufficient to build a reader from --
which is the point of M4, because the user's proprietary simulator will be built the
same way.

It also checks the M4 acceptance conditions.

usage: contract_reader.py <contract-dir>
"""
import json
import struct
import sys
from pathlib import Path


def read_scene_geometry(d):
    g = json.loads((d / "scene_geometry.json").read_text())
    for k in ("format_version", "epsg", "local_origin", "padded_bbox_local",
              "crown_archetype", "objects"):
        if k not in g:
            raise SystemExit(f"scene_geometry.json: missing {k}")
    return g


def read_ply(path):
    """Parse the header generically, then read the two binary blocks."""
    with open(path, "rb") as f:
        if f.readline().strip() != b"ply":
            raise SystemExit("opaque.ply: not a PLY file")
        fmt = f.readline().split()
        if fmt[1] != b"binary_little_endian":
            raise SystemExit(f"opaque.ply: unexpected format {fmt[1]!r}")
        elements, cur = [], None
        while True:
            line = f.readline()
            if not line:
                raise SystemExit("opaque.ply: header never ends")
            t = line.split()
            if not t or t[0] == b"comment":
                continue
            if t[0] == b"element":
                cur = {"name": t[1].decode(), "count": int(t[2]), "props": []}
                elements.append(cur)
            elif t[0] == b"property":
                cur["props"].append(t[1:])
            elif t[0] == b"end_header":
                break
        data_start = f.tell()

    sizes = {b"float": 4, b"uint": 4, b"int": 4, b"uchar": 1, b"char": 1,
             b"double": 8, b"ushort": 2, b"short": 2}
    codes = {b"float": "f", b"uint": "I", b"int": "i", b"uchar": "B", b"char": "b",
             b"double": "d", b"ushort": "H", b"short": "h"}

    out = {}
    with open(path, "rb") as f:
        f.seek(data_start)
        for el in elements:
            fixed = all(p[0] != b"list" for p in el["props"])
            if fixed:
                rec = "<" + "".join(codes[p[0]] for p in el["props"])
                n = struct.calcsize(rec)
                buf = f.read(n * el["count"])
                if len(buf) != n * el["count"]:
                    raise SystemExit(f"opaque.ply: short read in {el['name']}")
                out[el["name"]] = (rec, n, buf, [p[1].decode() for p in el["props"]])
            else:
                # list property: count byte then that many entries, then the scalars
                rows, buf = [], f.read()
                off = 0
                for _ in range(el["count"]):
                    row = []
                    for p in el["props"]:
                        if p[0] == b"list":
                            cnt = struct.unpack_from("<" + codes[p[1]], buf, off)[0]
                            off += sizes[p[1]]
                            row.append(list(struct.unpack_from(
                                "<" + codes[p[2]] * cnt, buf, off)))
                            off += sizes[p[2]] * cnt
                        else:
                            row.append(struct.unpack_from("<" + codes[p[0]], buf, off)[0])
                            off += sizes[p[0]]
                    rows.append(row)
                out[el["name"]] = rows
    return elements, out


def read_crowns(path, sample=5):
    """Stream crowns.bin: every header, and the PAD block of the first `sample`."""
    recs, sampled = [], []
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != b"ONIONCRW":
            raise SystemExit(f"crowns.bin: bad magic {magic!r}")
        version, n = struct.unpack("<II", f.read(8))
        for i in range(n):
            hdr = f.read(28)
            if len(hdr) != 28:
                raise SystemExit(f"crowns.bin: short header at record {i}")
            oid, ix, iy, iz, nx, ny, nz = struct.unpack("<IiiiIII", hdr)
            nbytes = 4 * nx * ny * nz
            if i < sample:
                blk = f.read(nbytes)
                if len(blk) != nbytes:
                    raise SystemExit(f"crowns.bin: short PAD block at record {i}")
                sampled.append(struct.unpack(f"<{nx*ny*nz}f", blk))
            else:
                f.seek(nbytes, 1)
            recs.append((oid, ix, iy, iz, nx, ny, nz))
        if f.read(1):
            raise SystemExit("crowns.bin: trailing bytes after the last record")
    return version, recs, sampled


def main():
    d = Path(sys.argv[1])
    g = read_scene_geometry(d)
    print(f"format_version {g['format_version']}  epsg {g['epsg']}")
    print(f"local_origin   {g['local_origin']}")
    print(f"padded bbox    {g['padded_bbox_local']}")
    print(f"archetype      {g['crown_archetype']}")
    table = {o["object_id"]: o for o in g["objects"]}
    print(f"objects        {len(table)}")

    elements, ply = read_ply(d / "opaque.ply")
    vrec, vn, vbuf, vprops = ply["vertex"]
    nv = len(vbuf) // vn
    faces = ply["face"]
    print(f"\nopaque.ply     {nv} vertices, {len(faces)} faces, props {vprops}")

    xmin, ymin, xmax, ymax = g["padded_bbox_local"]
    vx = [struct.unpack_from(vrec, vbuf, i * vn) for i in range(nv)]
    bad = [v for v in vx if not (xmin - 1e-3 <= v[0] <= xmax + 1e-3
                                 and ymin - 1e-3 <= v[1] <= ymax + 1e-3)]
    print(f"  vertex x {min(v[0] for v in vx):.3f}..{max(v[0] for v in vx):.3f}"
          f"   y {min(v[1] for v in vx):.3f}..{max(v[1] for v in vx):.3f}"
          f"   z {min(v[2] for v in vx):.2f}..{max(v[2] for v in vx):.2f}")
    print(f"  vertices outside padded bbox: {len(bad)}")

    cls_count, missing = {}, 0
    for fr in faces:
        idx, oid, cls = fr[0], fr[1], fr[2]
        cls_count[cls] = cls_count.get(cls, 0) + 1
        if oid not in table or table[oid]["class"] != cls:
            missing += 1
        if len(idx) != 3:
            raise SystemExit("opaque.ply: non-triangular face")
    print(f"  faces by class: {cls_count}")
    print(f"  faces whose object_id is absent from the table or class-mismatched: {missing}")

    version, recs, sampled = read_crowns(d / "crowns.bin")
    vox = sum(r[4] * r[5] * r[6] for r in recs)
    print(f"\ncrowns.bin     version {version}, {len(recs)} crowns, {vox} voxels")
    bad_oid = sum(1 for r in recs if r[0] not in table or table[r[0]]["class"] != 5)
    print(f"  crowns whose object_id is absent or not class 5: {bad_oid}")
    s = g["crown_archetype"]["voxel_size_m"]
    cx = [(r[1] * s, (r[1] + r[4]) * s) for r in recs]
    cy = [(r[2] * s, (r[2] + r[5]) * s) for r in recs]
    print(f"  crown blocks x {min(a for a,_ in cx):.2f}..{max(b for _,b in cx):.2f}"
          f"   y {min(a for a,_ in cy):.2f}..{max(b for _,b in cy):.2f}")
    vals = sorted({v for blk in sampled for v in blk})
    print(f"  distinct PAD values in the first {len(sampled)} crowns: {vals}")
    fill = [sum(1 for v in blk if v > 0) / len(blk) for blk in sampled]
    print(f"  envelope fill fraction: {[f'{x:.2f}' for x in fill]}")

    # M4: terrain must cover the padded tile with no gaps.
    ground = [fr for fr in faces if fr[2] == 2]
    nxc = round(xmax - xmin)
    expect = 2 * nxc * round(ymax - ymin)
    print(f"\nterrain: {len(ground)} ground faces, expected {expect} "
          f"for a {nxc} x {round(ymax-ymin)} m tile at 1 m "
          f"-> {'COVERS, no gaps' if len(ground) == expect else 'MISMATCH'}")
    print("\nall three contract files parsed from FORMAT.md alone")


if __name__ == "__main__":
    main()
