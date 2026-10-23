#!/usr/bin/env python3
"""Gate G3: canopy height v1 vs v2, so the user can pick a version.

Compares both versions against the WorldCover tree mask on the same tile, and reports
canopy height inside Overture building footprints -- the rooftop contamination the tree
mask is there to remove.

v1 and v2 are already pixel-aligned in the cache: fetch_raster derives the same output
resolution for both, so no resampling is needed to compare them.

usage: g3_canopy.py <cache-dir> [<cache-dir> ...]
"""
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

os.environ["GDAL_DISABLE_READDIR_ON_OPEN"] = "EMPTY_DIR"
TH = 2.0  # metres; the usual threshold separating canopy from ground clutter


def info(p):
    return json.loads(subprocess.run(["gdalinfo", "-json", str(p)], check=True,
                                     capture_output=True, text=True).stdout)


def main():
    out = Path("/tmp/g3")
    out.mkdir(parents=True, exist_ok=True)

    for cache in [Path(c) for c in sys.argv[1:]]:
        req = json.loads((cache / "request.json").read_text())
        ci = info(cache / "chm_v2.tif")
        gt, (nx, ny) = ci["geoTransform"], ci["size"]
        te = [gt[0], gt[3] + ny * gt[5], gt[0] + nx * gt[1], gt[3]]
        tag = cache.name

        # Land cover and buildings onto the canopy grid (the finest layer).
        for src, dst, r in ((cache / "landcover.tif", out / f"{tag}_lc.tif", "near"),):
            subprocess.run(["gdalwarp", "-q", "-overwrite", "-te", *[f"{v:.9f}" for v in te],
                            "-ts", str(nx), str(ny), "-r", r, "-of", "GTiff",
                            str(src), str(dst)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["gdal_rasterize", "-q", "-burn", "1", "-ot", "Byte", "-of", "GTiff",
                        "-init", "0", "-te", *[f"{v:.9f}" for v in te], "-ts", str(nx), str(ny),
                        str(cache / "buildings.fgb"), str(out / f"{tag}_b.tif")], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        v1 = np.array(Image.open(cache / "chm_v1.tif")).astype(np.float64)
        v2 = np.array(Image.open(cache / "chm_v2.tif")).astype(np.float64)
        lc = np.array(Image.open(out / f"{tag}_lc.tif"))
        bld = np.array(Image.open(out / f"{tag}_b.tif")).astype(bool)
        tree = lc == 10

        print("=" * 72)
        print(f"{tag}  lat {req['lat']} lon {req['lon']}  {nx}x{ny} @ {gt[1]:.4f} m")
        print(f"  tree mask {100*tree.mean():.1f}%   building px {bld.sum()} "
              f"({100*bld.mean():.1f}%)")

        print(f"\n  {'':<10}{'mean':>7}{'p50':>6}{'p90':>6}{'p99':>6}{'max':>6}"
              f"{'>2m%':>7}{'tree>2m%':>10}{'off>2m%':>9}{'enrich':>8}")
        for nm, v in (("v1", v1), ("v2", v2)):
            on = 100 * (v[tree] > TH).mean() if tree.any() else float("nan")
            off = 100 * (v[~tree] > TH).mean() if (~tree).any() else float("nan")
            print(f"  {nm:<10}{v.mean():7.2f}{np.percentile(v,50):6.1f}"
                  f"{np.percentile(v,90):6.1f}{np.percentile(v,99):6.1f}{v.max():6.0f}"
                  f"{100*(v>TH).mean():7.1f}{on:10.1f}{off:9.1f}{on/max(off,1e-9):8.1f}")

        # The tree mask as ground truth, canopy>2m as the prediction.
        print(f"\n  as a tree detector against the WorldCover mask (>{TH:.0f} m):")
        for nm, v in (("v1", v1), ("v2", v2)):
            p = v > TH
            tp, fp, fn = (p & tree).sum(), (p & ~tree).sum(), (~p & tree).sum()
            prec = tp / max(tp + fp, 1); rec = tp / max(tp + fn, 1)
            f1 = 2 * prec * rec / max(prec + rec, 1e-9)
            print(f"    {nm}  precision {prec:.3f}  recall {rec:.3f}  F1 {f1:.3f}")

        if bld.any():
            print(f"\n  ROOFTOP CONTAMINATION -- canopy height inside {bld.sum()} "
                  f"footprint px:")
            for nm, v in (("v1", v1), ("v2", v2)):
                print(f"    {nm}  mean {v[bld].mean():5.2f} m  p90 {np.percentile(v[bld],90):5.1f} m"
                      f"  p99 {np.percentile(v[bld],99):5.1f} m  >2m {100*(v[bld]>TH).mean():5.1f}%"
                      f"  >5m {100*(v[bld]>5).mean():5.1f}%")
            kept = 100 * (tree & bld).sum() / max(bld.sum(), 1)
            print(f"    {100-kept:.1f}% of footprint px fall OUTSIDE the tree mask, so the"
                  f" mask discards them; {kept:.1f}% survive into the tree list")
            surv = tree & bld
            if surv.any():
                for nm, v in (("v1", v1), ("v2", v2)):
                    print(f"      residual after masking, {nm}: mean {v[surv].mean():5.2f} m"
                          f"  >2m {100*(v[surv]>TH).mean():5.1f}%"
                          f"  = {100*(surv & (v>TH)).sum()/v.size:.3f}% of the whole tile")

        d = v2 - v1
        print(f"\n  v2 minus v1: mean {d.mean():+.2f} m  RMS {np.sqrt((d**2).mean()):.2f} m"
              f"  corr {np.corrcoef(v1.ravel(), v2.ravel())[0,1]:+.3f}")
        print(f"    v2 taller on {100*(d>0).mean():.1f}% of px, v1 taller on "
              f"{100*(d<0).mean():.1f}%, equal on {100*(d==0).mean():.1f}%")

        ramp = lambda a: (np.dstack([a*0.2, np.clip(a/30,0,1)*255, a*0.2])).astype(np.uint8)
        pair = np.concatenate([ramp(v1), np.zeros((ny,8,3),np.uint8), ramp(v2)], axis=1)
        Image.fromarray(pair).save(out / f"{tag}_v1_v2.png")
    print("=" * 72)
    print(f"\nPNGs in {out} (v1 left, v2 right)")


if __name__ == "__main__":
    main()
