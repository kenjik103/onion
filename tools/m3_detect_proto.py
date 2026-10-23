#!/usr/bin/env python3
"""M3 prototype: individual tree detection from the canopy height raster.

Proposal to be approved before it is written in C++. Deliberately uses only numpy,
mirroring the no-new-dependency rule the C++ side lives under (no scipy, no skimage),
so nothing here relies on a primitive the real implementation could not have.

Method
  1. Mask to WorldCover tree class 10 and canopy >= H_MIN.
  2. Find local maxima with a window whose radius scales with height, because a tall
     tree's crown is wider and a fixed window splits it into several detections.
  3. Handle the 1 m quantisation explicitly: v2 has ~66% of adjacent pixels equal, so
     maxima come in tied plateaus. Plateaus are grouped by connectivity and reduced to
     their centroid, giving ONE tree per plateau instead of dozens.
  4. Crown radius is measured from the raster by walking outward from each peak until
     the surface drops below a fraction of the peak or starts rising into a neighbour.
  5. Ground elevation is sampled from the terrain raster at the tree position.

usage: m3_detect_proto.py <cache-dir> [sweep]
"""
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

os.environ["GDAL_DISABLE_READDIR_ON_OPEN"] = "EMPTY_DIR"

H_MIN = 2.0        # metres; below this is ground clutter, not a tree
CROWN_FRAC = 0.5   # crown edge where the surface falls to this fraction of peak height
R_MIN_PX = 1       # smallest half-window, in pixels
R_MAX_PX = 12


def info(p):
    return json.loads(subprocess.run(["gdalinfo", "-json", str(p)], check=True,
                                     capture_output=True, text=True).stdout)


def align(src, dst, te, ts, resampling):
    subprocess.run(["gdalwarp", "-q", "-overwrite", "-te", *[f"{v:.9f}" for v in te],
                    "-ts", str(ts[0]), str(ts[1]), "-r", resampling, "-of", "GTiff",
                    str(src), str(dst)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def max_filter(a, r):
    """Square max filter, done separably so cost is O(n*r) not O(n*r^2)."""
    if r <= 0:
        return a
    out = a
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="constant", constant_values=-np.inf)
        sl = [np.s_[:], np.s_[:]]
        acc = None
        for k in range(2 * r + 1):
            sl[axis] = np.s_[k:k + a.shape[axis]]
            w = p[tuple(sl)]
            acc = w if acc is None else np.maximum(acc, w)
        out = acc
    return out


def label_plateaus(mask):
    """Connected components (8-connectivity) by iterative label propagation.

    Plain numpy: every cell starts with a unique id and repeatedly takes the maximum id
    among its neighbours until nothing changes. Components here are a few pixels across,
    so this converges in a handful of passes.
    """
    lab = np.where(mask, np.arange(mask.size).reshape(mask.shape) + 1, 0)
    while True:
        m = lab.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                m = np.maximum(m, np.roll(np.roll(lab, dy, 0), dx, 1))
        m = np.where(mask, m, 0)
        if np.array_equal(m, lab):
            return lab
        lab = m


def detect(chm, mask, px, k):
    """Local maxima with a height-dependent window. k = crown radius / tree height."""
    # A handful of discrete radii instead of a true per-pixel window: each pixel is
    # tested against the filter matching its own height band.
    radii = sorted({int(np.clip(round(k * h / px), R_MIN_PX, R_MAX_PX))
                    for h in range(int(H_MIN), 61)})
    want = np.clip(np.round(k * chm / px), R_MIN_PX, R_MAX_PX).astype(int)
    is_max = np.zeros(chm.shape, bool)
    for r in radii:
        band = mask & (want == r)
        if not band.any():
            continue
        is_max |= band & (chm >= max_filter(chm, r))

    lab = label_plateaus(is_max)
    ids = lab[lab > 0]
    if ids.size == 0:
        return np.empty((0, 4))
    uniq, inv = np.unique(ids, return_inverse=True)
    ys, xs = np.nonzero(lab > 0)
    cy = np.bincount(inv, weights=ys) / np.bincount(inv)
    cx = np.bincount(inv, weights=xs) / np.bincount(inv)
    hh = np.bincount(inv, weights=chm[ys, xs]) / np.bincount(inv)
    # Renumber plateaus to 1..n so they can seed the watershed.
    seeds = np.zeros(chm.shape, np.int32)
    seeds[ys, xs] = inv + 1
    return np.column_stack([cx, cy, hh, np.bincount(inv)]), seeds


def watershed(chm, mask, seeds_lab):
    """Marker-controlled watershed: flood from the seed maxima in descending height.

    A radial descent cannot size crowns in closed canopy -- neighbouring crowns are as
    tall as the peak, so the walk never reaches a crown edge and simply runs to the
    search cap (measured: r_p90 pinned at the cap, crown cover 190%). Flooding instead
    partitions the masked canopy among the seeds, so every canopy pixel belongs to
    exactly one crown and crown area is what the raster actually supports.

    Pixels are visited tallest first; each takes the label of its tallest already
    labelled neighbour. One pass, no priority queue.
    """
    lab = seeds_lab.copy()
    ys, xs = np.nonzero(mask & (lab == 0))
    order = np.argsort(-chm[ys, xs], kind="stable")
    ys, xs = ys[order], xs[order]
    ny, nx = chm.shape
    nbr = [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]
    pending = list(zip(ys.tolist(), xs.tolist()))
    for _ in range(64):                       # converges quickly; guard against stalls
        leftover = []
        for y, x in pending:
            best, bh = 0, -np.inf
            for dy, dx in nbr:
                yy, xx = y + dy, x + dx
                if 0 <= yy < ny and 0 <= xx < nx and lab[yy, xx] > 0 and chm[yy, xx] > bh:
                    best, bh = lab[yy, xx], chm[yy, xx]
            if best:
                lab[y, x] = best
            else:
                leftover.append((y, x))
        if not leftover or len(leftover) == len(pending):
            break
        pending = leftover
    return lab


def crown_radii_from_segments(lab, n, px):
    """Crown radius as the radius of a circle with the segment's area."""
    counts = np.bincount(lab.ravel(), minlength=n + 1)[1:]
    return np.sqrt(np.maximum(counts, 1) * px * px / np.pi), counts


def main():
    cache = Path(sys.argv[1])
    out = Path("/tmp/m3"); out.mkdir(parents=True, exist_ok=True)
    ci = info(cache / "chm.tif")
    gt, (nx, ny) = ci["geoTransform"], ci["size"]
    px = gt[1]
    te = [gt[0], gt[3] + ny * gt[5], gt[0] + nx * gt[1], gt[3]]
    align(cache / "landcover.tif", out / "lc.tif", te, (nx, ny), "near")
    align(cache / "terrain.tif", out / "dtm.tif", te, (nx, ny), "bilinear")

    chm = np.array(Image.open(cache / "chm.tif")).astype(np.float64)
    lc = np.array(Image.open(out / "lc.tif"))
    dtm = np.array(Image.open(out / "dtm.tif")).astype(np.float64)
    mask = (lc == 10) & (chm >= H_MIN)
    ha = nx * ny * px * px / 10000.0
    print(f"{cache.name}: {nx}x{ny} @ {px:.4f} m = {ha:.2f} ha, "
          f"tree+canopy mask {100*mask.mean():.1f}%")

    ks = [0.05, 0.075, 0.10, 0.15, 0.20, 0.30] if len(sys.argv) > 2 else [0.10]
    print(f"\n{'k':>7}{'trees':>8}{'per ha':>9}{'h mean':>8}{'h p90':>7}"
          f"{'r mean':>8}{'r p10':>7}{'r p90':>7}{'corr(r,h)':>11}")
    for k in ks:
        t, seeds = detect(chm, mask, px, k)
        if not len(t):
            print(f"{k:7.3f}  none"); continue
        lab = watershed(chm, mask, seeds)
        r, counts = crown_radii_from_segments(lab, len(t), px)
        cc = np.corrcoef(t[:, 2], r)[0, 1]
        print(f"{k:7.3f}{len(t):8d}{len(t)/ha:9.1f}{t[:,2].mean():8.2f}"
              f"{np.percentile(t[:,2],90):7.1f}{r.mean():8.2f}"
              f"{np.percentile(r,10):7.2f}{np.percentile(r,90):7.2f}{cc:+11.3f}")
        np.save(out / f"trees_k{int(k*1000)}.npy", np.column_stack([t, r]))
        np.save(out / f"lab_k{int(k*1000)}.npy", lab)
    np.save(out / "chm.npy", chm); np.save(out / "mask.npy", mask); np.save(out / "dtm.npy", dtm)
    (out / "grid.json").write_text(json.dumps({"px": px, "te": te, "nx": nx, "ny": ny}))


if __name__ == "__main__":
    main()
