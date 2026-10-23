#!/usr/bin/env python3
"""Gate G2: is GEDTM30 really a bare-earth DTM?

Test: Copernicus GLO-30 is a surface model (DSM) and GEDTM30 claims to be bare earth.
Their difference should therefore look like canopy height -- large under trees, and
~zero over open ground. The open ground is the control: without it a difference image
has nothing to calibrate against, and a half-pixel misregistration between the two DEMs
would be indistinguishable from a real canopy signal.

Everything is compared on GEDTM30's OWN cached grid, so the DTM is never resampled.
GLO-30 is validation data only and is never written to the cache (SPEC.md 7.5).

usage: g2_terrain.py <cache-dir> <out-dir>
"""
import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

GLO = "https://copernicus-dem-30m.s3.eu-central-1.amazonaws.com/"
# Gate tooling drives the GDAL CLI, which does not inherit the setting onion makes in
# main(). Without it, reads from these buckets take minutes instead of seconds.
os.environ["GDAL_DISABLE_READDIR_ON_OPEN"] = "EMPTY_DIR"


def info(path):
    return json.loads(subprocess.run(["gdalinfo", "-json", str(path)],
                                     check=True, capture_output=True, text=True).stdout)


def glo_cell(lat, lon):
    la, lo = int(math.floor(lat)), int(math.floor(lon))
    return (f"Copernicus_DSM_COG_10_{'S' if la < 0 else 'N'}{abs(la):02d}_00_"
            f"{'W' if lo < 0 else 'E'}{abs(lo):03d}_00_DEM")


def warp_to(src, dst, te, ts, epsg, resampling):
    subprocess.run(["gdalwarp", "-q", "-overwrite", "-t_srs", f"EPSG:{epsg}",
                    "-te", *[f"{v:.9f}" for v in te], "-ts", str(ts[0]), str(ts[1]),
                    "-r", resampling, "-of", "GTiff", str(src), str(dst)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    cache, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    req = json.loads((cache / "request.json").read_text())
    ti = info(cache / "terrain.tif")
    gt, (nx, ny) = ti["geoTransform"], ti["size"]
    epsg = int(ti["coordinateSystem"]["wkt"].split('ID["EPSG",')[-1].rstrip("]]").strip())
    te = [gt[0], gt[3] + ny * gt[5], gt[0] + nx * gt[1], gt[3]]
    print(f"grid: {nx}x{ny} @ {gt[1]:.4f} m, EPSG:{epsg}")

    # The padded tile must sit inside one GLO-30 cell; mosaicking is not implemented.
    cells = {glo_cell(la, lo) for la in (req["lat"] - 0.01, req["lat"] + 0.01)
             for lo in (req["lon"] - 0.01, req["lon"] + 0.01)}
    if len(cells) != 1:
        raise SystemExit(f"G2: padded tile spans {len(cells)} GLO-30 cells: {sorted(cells)}")
    cell = cells.pop()
    print(f"GLO-30 cell: {cell}")

    warp_to(f"/vsicurl/{GLO}{cell}/{cell}.tif", out / "glo.tif", te, (nx, ny), epsg, "bilinear")
    # Canopy aggregated to the DEM grid: a 30 m DSM cannot resolve a single crown, so
    # the fair comparison is the mean and the max canopy height inside each DEM cell.
    warp_to(cache / "chm_v2.tif", out / "chm_mean.tif", te, (nx, ny), epsg, "average")
    warp_to(cache / "chm_v2.tif", out / "chm_max.tif", te, (nx, ny), epsg, "max")
    warp_to(cache / "landcover.tif", out / "lc.tif", te, (nx, ny), epsg, "mode")

    dtm = np.array(Image.open(cache / "terrain.tif")).astype(np.float64)
    dsm = np.array(Image.open(out / "glo.tif")).astype(np.float64)
    cmean = np.array(Image.open(out / "chm_mean.tif")).astype(np.float64)
    cmax = np.array(Image.open(out / "chm_max.tif")).astype(np.float64)
    lc = np.array(Image.open(out / "lc.tif"))

    diff = dsm - dtm
    tree = lc == 10
    openg = np.isin(lc, (30, 40, 60))

    print(f"\nGEDTM30 (DTM) : {dtm.min():7.2f} .. {dtm.max():7.2f} m   mean {dtm.mean():7.2f}")
    print(f"GLO-30  (DSM) : {dsm.min():7.2f} .. {dsm.max():7.2f} m   mean {dsm.mean():7.2f}")
    print(f"DSM - DTM     : {diff.min():7.2f} .. {diff.max():7.2f} m   mean {diff.mean():7.2f}")
    print(f"\ncells: {tree.sum()} tree, {openg.sum()} open, {diff.size} total")

    print("\n--- the test: difference must look like canopy, and vanish on open ground ---")
    for label, m in (("tree cells", tree), ("open cells", openg)):
        if not m.any():
            continue
        print(f"  {label:<12} DSM-DTM mean {diff[m].mean():6.2f} m   median "
              f"{np.median(diff[m]):6.2f} m   canopy mean {cmean[m].mean():5.2f} m")

    if tree.any() and openg.any():
        sep = diff[tree].mean() - diff[openg].mean()
        print(f"\n  separation (tree minus open): {sep:.2f} m")
        print(f"  canopy separation for comparison: "
              f"{cmean[tree].mean() - cmean[openg].mean():.2f} m")

    for name, c in (("mean", cmean), ("max", cmax)):
        r = np.corrcoef(diff.ravel(), c.ravel())[0, 1]
        print(f"  corr(DSM-DTM, canopy {name}) = {r:+.3f}")

    # Images, all on the same grid so they can be compared directly.
    def ramp(a, lo, hi, rgb):
        t = np.clip((a - lo) / (hi - lo), 0, 1)
        return (np.dstack([t * rgb[0], t * rgb[1], t * rgb[2]])).astype(np.uint8)

    big = lambda a: np.array(Image.fromarray(a).resize((nx * 8, ny * 8), Image.NEAREST))
    Image.fromarray(big(ramp(diff, 0, 30, (255, 180, 60)))).save(out / "01_dsm_minus_dtm.png")
    Image.fromarray(big(ramp(cmean, 0, 30, (60, 255, 60)))).save(out / "02_canopy_mean.png")
    Image.fromarray(big(ramp(np.where(tree, 255, np.where(openg, 90, 0)).astype(float),
                             0, 255, (120, 160, 255)))).save(out / "03_classes.png")
    print(f"\nwrote PNGs to {out}")


if __name__ == "__main__":
    main()
