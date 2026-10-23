#!/usr/bin/env python3
"""G4 evidence on a real tile: canopy gap fraction, voxel banding, cross-section.

Reads the LAS and crowns.bin from the format specs, independent of the generator.

usage: g4_crown.py <contract-dir> <out-prefix>
"""
import json
import struct
import sys

import numpy as np
from PIL import Image


def las_arrays(path):
    with open(path, "rb") as f:
        h = f.read(375)
        data_off = struct.unpack_from("<I", h, 96)[0]
        plen = struct.unpack_from("<H", h, 105)[0]
        sx, sy, sz = struct.unpack_from("<ddd", h, 131)
        ox, oy, oz = struct.unpack_from("<ddd", h, 155)
        n = struct.unpack_from("<Q", h, 247)[0]
    raw = np.memmap(path, dtype=np.uint8, mode="r", offset=data_off, shape=(n, plen))
    xi = raw[:, 0:4].copy().view(np.int32).ravel()
    yi = raw[:, 4:8].copy().view(np.int32).ravel()
    zi = raw[:, 8:12].copy().view(np.int32).ravel()
    cls = raw[:, 16].copy()
    return xi * sx + ox, yi * sy + oy, zi * sz + oz, cls


def crown_column_sums(path, voxel, nvx):
    """Summed PAD down each column of the tile-wide voxel lattice.

    A dense lattice-resolution grid, so every voxel is counted exactly once. Scattering
    voxels onto the finer sample grid instead double-counts where voxel footprints
    overlap, which inflates the optical depth.
    """
    colsum = np.zeros((nvx, nvx), np.float64)
    with open(path, "rb") as f:
        assert f.read(8) == b"ONIONCRW"
        _, count = struct.unpack("<II", f.read(8))
        for _ in range(count):
            oid, ix, iy, iz, cnx, cny, cnz = struct.unpack("<IiiiIII", f.read(28))
            n = cnx * cny * cnz
            pad = np.frombuffer(f.read(4 * n), np.float32).reshape(cnz, cny, cnx)
            tot = pad.sum(axis=0, dtype=np.float64)
            yy, xx = np.nonzero(tot)
            if not len(yy):
                continue
            a, b = ix + xx, iy + yy
            m = (a >= 0) & (a < nvx) & (b >= 0) & (b < nvx)
            np.add.at(colsum, (b[m], a[m]), tot[yy[m], xx[m]])
    return colsum


def main():
    d, prefix = sys.argv[1], sys.argv[2]
    meta = json.load(open(f"{d}/scene_geometry.json"))
    ox, oy = meta["local_origin"]
    voxel = meta["crown_archetype"]["voxel_size_m"]
    pad_val = meta["crown_archetype"]["pad"]
    gcoef = meta["crown_archetype"]["g"]

    x, y, z, cls = las_arrays(f"{d}/points.las")
    n = len(z)
    veg = cls == 5
    print(f"points {n}   vegetation {veg.sum()} ({100*veg.mean():.1f}%)   "
          f"ground/building {(~veg).sum()}")

    # --- banding: phase of each vegetation hit inside its own voxel layer -----------
    phase = (z[veg] / voxel) % 1.0
    hist, _ = np.histogram(phase, bins=20, range=(0, 1))
    rel = hist / hist.mean()
    print(f"\nvoxel-layer banding check (vegetation z mod {voxel} m, 20 bins):")
    print(f"  bin counts relative to mean: min {rel.min():.3f}  max {rel.max():.3f}")
    # A FLAT distribution is the wrong null. Within a voxel the ray attenuates, so hits
    # concentrate toward the voxel top; phase = 1 - depth/voxel, giving density
    # proportional to exp(+rate*voxel*phase). Banding would be a SPIKE at one phase,
    # not a smooth gradient, so the test is whether the gradient is smooth and whether
    # the implied optical depth is physically sensible.
    centres = (np.arange(20) + 0.5) / 20
    slope, intercept = np.polyfit(centres, np.log(rel), 1)
    resid = np.log(rel) - (slope * centres + intercept)
    print(f"  fitted log-linear gradient: exp({slope:+.4f} * phase)")
    print(f"    -> implied rate*voxel = {slope:.4f}, i.e. summed PAD "
          f"{slope/(gcoef*voxel*voxel):.2f} against a single-crown {pad_val}")
    print(f"  residual from a smooth exponential: max |{np.abs(resid).max():.4f}| "
          f"({100*np.abs(resid).max():.2f}%)")
    print(f"  largest single-bin excess over the fit: "
          f"{100*(np.exp(np.abs(resid).max())-1):.2f}%  (a layer artefact would spike)")
    for i in range(0, 20, 2):
        print(f"    phase {i/20:.2f}-{(i+2)/20:.2f}  {'#'*int(40*rel[i])}")

    # --- gap fraction ---------------------------------------------------------------
    nvx = int(round(meta["padded_bbox_local"][2] / voxel))
    colsum = crown_column_sums(f"{d}/crowns.bin", voxel, nvx)

    # Each point's own xy is the sample position, so the lookup is exact.
    vx = np.floor((x - ox) / voxel).astype(np.int64)
    vy = np.floor((y - oy) / voxel).astype(np.int64)
    np.clip(vx, 0, nvx - 1, out=vx)
    np.clip(vy, 0, nvx - 1, out=vy)
    cs = colsum[vy, vx]
    under = cs > 0
    gap = (~veg) & under
    print(f"\ncanopy gap fraction:")
    print(f"  samples with canopy above : {under.sum()} ({100*under.mean():.1f}% of tile)")
    print(f"  of those, reaching a surface instead of a crown : {gap.sum()} "
          f"({100*gap.sum()/max(under.sum(),1):.1f}%)")
    print(f"  (PAD {pad_val}, G {gcoef}, voxel {voxel} m)")

    # Optical depth: each voxel contributes pad * voxel_size of path.
    tau = gcoef * cs[under] * voxel
    print(f"\n  optical depth under canopy: mean {tau.mean():.2f}, median "
          f"{np.median(tau):.2f}, p10 {np.percentile(tau,10):.2f}, "
          f"p90 {np.percentile(tau,90):.2f}")
    print(f"  analytic gap at this PAD: {100*np.exp(-tau).mean():.1f}%  "
          f"(Monte Carlo measured {100*gap.sum()/max(under.sum(),1):.1f}%)")
    print(f"\n  gap fraction if PAD were scaled (analytic):")
    for k in (0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.7, 1.0):
        print(f"    PAD {k:4.2f} -> {100*np.exp(-tau*(k/pad_val)).mean():5.1f}%")

    # --- vertical cross-section ------------------------------------------------------
    ymid = (y.min() + y.max()) / 2
    sl = np.abs(y - ymid) < 2.5   # thicker slab, so one crown has enough points to read
    xs, zs, cs2 = x[sl], z[sl], cls[sl]
    print(f"\ncross-section at y = {ymid:.1f}: {sl.sum()} points")

    def draw(x0, width, name, tag):
        w, hgt = 1600, 620
        img = np.zeros((hgt, w, 3), np.uint8)
        win = (xs >= x0) & (xs <= x0 + width)
        if win.sum() < 10:
            return
        zz = zs[win]
        z0, z1 = zz.min() - 1.5, zz.max() + 1.5
        px = ((xs[win] - x0) / width * (w - 2)).astype(int)
        pz = ((1 - (zz - z0) / (z1 - z0)) * (hgt - 2)).astype(int)
        colour = np.where((cs2[win] == 5)[:, None],
                          np.array([70, 240, 70]), np.array([215, 185, 135]))
        # 2x2 blocks, or a sparse slice renders as near-invisible single pixels.
        for dy in (0, 1):
            for dx in (0, 1):
                img[pz + dy, px + dx] = colour
        # Rules at the voxel layer boundaries down the left margin only, so they mark
        # the layers without covering the points. Banding would align with these.
        for k in range(int(np.floor(z0 / voxel)), int(np.ceil(z1 / voxel)) + 1):
            zr = k * voxel
            if z0 <= zr <= z1:
                r = int((1 - (zr - z0) / (z1 - z0)) * (hgt - 2))
                img[r, 0:28] = np.array([90, 90, 130], np.uint8)
        Image.fromarray(img).save(name)
        print(f"  wrote {name}  ({width:.0f} m wide, {z1-z0:.1f} m tall, "
              f"1 px = {(z1-z0)/hgt*100:.1f} cm; {tag})")

    draw(xs.min(), 120.0, f"{prefix}_crosssection.png", "voxel layers ruled in grey")
    # A single crown: find the densest 14 m window of vegetation along the slice.
    vx = xs[cs2 == 5]
    if len(vx):
        hist, edges = np.histogram(vx, bins=int((vx.max() - vx.min()) / 10) + 1)
        best = edges[np.argmax(hist)] - 5.0
        draw(best, 20.0, f"{prefix}_onecrown.png", "voxel layers ruled at left")


if __name__ == "__main__":
    main()
