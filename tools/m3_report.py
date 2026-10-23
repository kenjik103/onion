#!/usr/bin/env python3
"""M3 deliverable: detected crowns overlaid on the canopy map, with counts and
height distribution. Reads scene_spec.json -- the pipeline's real output, not the
prototype's -- so what is shown is what the C++ actually produced.

usage: m3_report.py <scene_spec.json> <cache-dir> <out.png>
"""
import json, sys
import numpy as np
from PIL import Image

spec = json.load(open(sys.argv[1]))
cache, outpng = sys.argv[2], sys.argv[3]
trees = spec["trees"]
h = np.array([t["height_m"] for t in trees])
r = np.array([t["crown_radius_m"] for t in trees])
gz = np.array([t["ground_z"] for t in trees])

import subprocess
gi = json.loads(subprocess.run(["gdalinfo", "-json", f"{cache}/chm.tif"],
                               capture_output=True, text=True).stdout)
gt, (nx, ny) = gi["geoTransform"], gi["size"]
px = gt[1]
ha = nx * ny * px * px / 1e4

print(f"trees {len(trees)}   area {ha:.2f} ha   density {len(trees)/ha:.1f} /ha")
print(f"height   mean {h.mean():6.2f}  p10 {np.percentile(h,10):5.1f}  p50 {np.percentile(h,50):5.1f}"
      f"  p90 {np.percentile(h,90):5.1f}  max {h.max():5.1f}")
print(f"crown r  mean {r.mean():6.2f}  p10 {np.percentile(r,10):5.2f}  p50 {np.percentile(r,50):5.2f}"
      f"  p90 {np.percentile(r,90):5.2f}  max {r.max():5.2f}")
print(f"ground_z mean {gz.mean():6.2f}  range {gz.min():.2f} .. {gz.max():.2f} m")
print(f"r/h {r.mean()/h.mean():.3f}   corr(r,h) {np.corrcoef(r,h)[0,1]:+.3f}")
print(f"crowns under 1.5 m radius: {100*(r<1.5).mean():.1f}%   over 10 m: {100*(r>10).mean():.1f}%")
print("\nheight distribution:")
hist, edges = np.histogram(h, bins=np.arange(0, 45, 5))
for c, e in zip(hist, edges):
    print(f"  {e:2.0f}-{e+5:2.0f} m  {c:6d}  {'#'*int(55*c/max(hist.max(),1))}")

chm = np.array(Image.open(f"{cache}/chm.tif")).astype(float)
img = np.dstack([chm*0.2, np.clip(chm/35, 0, 1)*255, chm*0.2]).astype(np.uint8)
# crown circles from the detected radius, drawn as rings so the canopy stays visible
cx = (np.array([t["x"] for t in trees]) - gt[0]) / gt[1]
cy = (np.array([t["y"] for t in trees]) - gt[3]) / gt[5]
yy, xx = np.indices(chm.shape)
for i in range(len(trees)):
    rad = r[i] / px
    x0, y0 = int(cx[i]), int(cy[i])
    s = int(rad) + 2
    ys, xs = slice(max(y0-s, 0), min(y0+s+1, ny)), slice(max(x0-s, 0), min(x0+s+1, nx))
    d = np.hypot(yy[ys, xs]-cy[i], xx[ys, xs]-cx[i])
    img[ys, xs][(d <= rad) & (d > rad-1.0)] = (255, 255, 255)
    img[max(y0,0):y0+1, max(x0,0):x0+1] = (255, 40, 40)
Image.fromarray(img).save(outpng)
Image.fromarray(img[300:700, 300:700]).resize((800, 800), Image.NEAREST).save(
    outpng.replace(".png", "_zoom.png"))
print(f"\nwrote {outpng}")
