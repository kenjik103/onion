#!/usr/bin/env python3
"""Gate G1 evidence: do the four sources agree on where things are?

Resamples every cached layer onto ONE common 1 m grid covering the padded tile, so
any misalignment shows up as a visible offset rather than being hidden by differing
resolutions. Writes PNG overlays and prints quantitative agreement statistics.

This is gate tooling, not pipeline code: it reads the cache directly and nothing in
the pipeline depends on it.

usage: g1_overlays.py <cache-dir> <out-dir>
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

RES = 1.0  # metres per pixel on the common grid


def run(*args):
    subprocess.run([str(a) for a in args], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def extent(path):
    info = json.loads(subprocess.run(["gdalinfo", "-json", str(path)],
                                     check=True, capture_output=True, text=True).stdout)
    ul = info["cornerCoordinates"]["upperLeft"]
    lr = info["cornerCoordinates"]["lowerRight"]
    return [min(ul[0], lr[0]), min(ul[1], lr[1]), max(ul[0], lr[0]), max(ul[1], lr[1])]


def common_box(paths):
    """Intersection of every layer's extent, so no layer is missing data anywhere on
    the comparison grid. Each cached raster is snapped outward to a whole number of its
    own pixels, so they end a metre or two apart even though they share an origin."""
    boxes = [extent(p) for p in paths]
    return [max(b[0] for b in boxes), max(b[1] for b in boxes),
            min(b[2] for b in boxes), min(b[3] for b in boxes)]


def warp(src, dst, te, resampling):
    run("gdalwarp", "-q", "-overwrite", "-te", *te, "-tr", RES, RES,
        "-r", resampling, "-of", "GTiff", src, dst)


def read(path):
    return np.array(Image.open(path))


def outline(mask):
    """1-pixel border of a filled mask, via shift-and-AND erosion (no scipy needed)."""
    e = mask.copy()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        e &= np.roll(mask, (dy, dx), (0, 1))
    return mask & ~e


def grey_rgb(a):
    return np.dstack([a, a, a]).astype(np.uint8)


def draw(base, mask, colour):
    out = base.copy()
    out[mask] = colour
    return out


def chm_rgb(chm, vmax=30.0):
    """Black -> green ramp, so bare ground reads as dark and tall canopy as bright."""
    t = np.clip(chm.astype(np.float32) / vmax, 0, 1)
    return np.dstack([(40 * t), (60 + 195 * t), (40 * t)]).astype(np.uint8) * (chm > 0)[..., None]


def main():
    cache, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    layers = [cache / n for n in ("terrain.tif", "landcover.tif", "chm_v1.tif", "chm_v2.tif")]
    te = [f"{v:.9f}" for v in common_box(layers)]
    print(f"common grid: {te} at {RES} m/px")

    # Every layer onto the same grid. Categorical land cover uses nearest; the rest bilinear.
    warp(cache / "terrain.tif", out / "t.tif", te, "bilinear")
    warp(cache / "landcover.tif", out / "lc.tif", te, "near")
    warp(cache / "chm_v1.tif", out / "c1.tif", te, "bilinear")
    warp(cache / "chm_v2.tif", out / "c2.tif", te, "bilinear")
    run("gdaldem", "hillshade", "-q", out / "t.tif", out / "hs.tif")
    run("gdal_translate", "-q", "-expand", "rgb", out / "lc.tif", out / "lc_rgb.tif")
    run("gdal_rasterize", "-q", "-burn", 1, "-ot", "Byte", "-of", "GTiff",
        "-init", 0, "-te", *te, "-tr", RES, RES, cache / "buildings.fgb", out / "b.tif")

    hs, lc, c1, c2 = read(out / "hs.tif"), read(out / "lc.tif"), read(out / "c1.tif"), read(out / "c2.tif")
    lc_rgb, bld = read(out / "lc_rgb.tif"), read(out / "b.tif").astype(bool)
    bo = outline(bld)
    RED, WHITE = (255, 40, 40), (255, 255, 255)

    Image.fromarray(draw(grey_rgb(hs), bo, RED)).save(out / "01_hillshade_buildings.png")
    Image.fromarray(draw(lc_rgb, bo, WHITE)).save(out / "02_landcover_buildings.png")
    Image.fromarray(draw(chm_rgb(c2), bo, RED)).save(out / "03_chm_v2_buildings.png")
    Image.fromarray(draw(chm_rgb(c1), bo, RED)).save(out / "04_chm_v1_buildings.png")

    # 2x2 panel so all four layers are compared at one glance, same grid, same scale.
    tiles = [draw(grey_rgb(hs), bo, RED), draw(lc_rgb, bo, WHITE),
             draw(chm_rgb(c1), bo, RED), draw(chm_rgb(c2), bo, RED)]
    h, w = tiles[0].shape[:2]
    panel = np.zeros((2 * h + 6, 2 * w + 6, 3), np.uint8)
    for i, t in enumerate(tiles):
        panel[(i // 2) * (h + 6):(i // 2) * (h + 6) + h, (i % 2) * (w + 6):(i % 2) * (w + 6) + w] = t
    Image.fromarray(panel).save(out / "05_panel.png")

    # ---- quantitative agreement -------------------------------------------------
    built = lc == 50
    tree = lc == 10
    print(f"\ngrid {lc.shape[0]}x{lc.shape[1]} px, {bld.sum()} building px, "
          f"{built.sum()} built-up px, {tree.sum()} tree px")

    print("\nWorldCover classes present:")
    for v, n in zip(*np.unique(lc, return_counts=True)):
        print(f"  {v:3d}  {100.0 * n / lc.size:5.1f}%")

    print("\nA. Overture footprints vs WorldCover built-up (independent sources):")
    print(f"   building px on built-up class : {100.0 * (bld & built).sum() / max(bld.sum(), 1):5.1f}%")
    print(f"   built-up class share of tile  : {100.0 * built.sum() / lc.size:5.1f}%  (chance level)")

    for name, chm in (("v1", c1), ("v2", c2)):
        print(f"\nB. WorldCover tree mask vs canopy {name} (independent sources):")
        print(f"   mean height on tree class     : {chm[tree].mean():5.2f} m" if tree.any() else "   no tree px")
        print(f"   mean height off tree class    : {chm[~tree].mean():5.2f} m")
        print(f"   tree px with height > 2 m     : {100.0 * (chm[tree] > 2).mean():5.1f}%" if tree.any() else "")
        print(f"   non-tree px with height > 2 m : {100.0 * (chm[~tree] > 2).mean():5.1f}%")
        print(f"   mean height inside footprints : {chm[bld].mean():5.2f} m  (rooftop contamination, G3)")

    print(f"\nwrote {len(list(out.glob('*.png')))} PNGs to {out}")


if __name__ == "__main__":
    main()
