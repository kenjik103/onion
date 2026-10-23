#!/usr/bin/env python3
"""Measure candidate tiles for gates G2/G3, so a tile is proposed on evidence.

For each candidate it reads a 1 km window straight out of WorldCover and canopy v2
over /vsicurl/ and reports the class mix and canopy statistics, plus whether the
Copernicus GLO-30 cell G2 needs actually exists.

Gate tooling: reads the live sources and the cached canopy index, nothing else.

usage: probe_tiles.py <canopy-v2-tiles.geojson>
"""
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

WC = "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
CHM2 = "https://dataforgood-fb-data.s3.us-east-1.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3/"
GLO = "https://copernicus-dem-30m.s3.eu-central-1.amazonaws.com/"
GEDTM30 = ("https://s3.opengeohub.org/global/dtm/v1.2/"
           "gedtm_rf_m_30m_s_20060101_20151231_go_epsg.4326.3855_v1.2.tif")

CANDIDATES = [
    ("Hoosier NF, IN",      38.20,  -86.40),
    ("Hoosier NF alt, IN",  38.15,  -86.45),
    ("Shawnee NF, IL",      37.55,  -88.70),
    ("Mark Twain NF, MO",   37.15,  -91.20),
    ("Ouachita NF, AR",     34.55,  -93.60),
    ("Daniel Boone, KY",    37.45,  -83.60),
    ("Talladega NF, AL",    33.30,  -86.40),
    ("Bankhead NF, AL",     34.30,  -87.35),
    ("Olympic Pen, WA",     47.60, -124.00),
    ("Chequamegon, WI",     46.10,  -90.80),
]


def wc_tile(lat, lon):
    la, lo = int(math.floor(lat / 3)) * 3, int(math.floor(lon / 3)) * 3
    return f"{'S' if la < 0 else 'N'}{abs(la):02d}{'W' if lo < 0 else 'E'}{abs(lo):03d}"


def glo_cell(lat, lon):
    la, lo = int(math.floor(lat)), int(math.floor(lon))
    return (f"Copernicus_DSM_COG_10_{'S' if la < 0 else 'N'}{abs(la):02d}_00_"
            f"{'W' if lo < 0 else 'E'}{abs(lo):03d}_00_DEM")


def box(lat, lon, extent_m=1000.0):
    dlat = extent_m / 2 / 111320.0
    dlon = dlat / math.cos(math.radians(lat))
    return lon - dlon, lat - dlat, lon + dlon, lat + dlat


def window(url, te, n, resampling):
    """Reproject a small lon/lat window to a square grid; None if unreadable."""
    with tempfile.NamedTemporaryFile(suffix=".tif", delete=False) as f:
        out = f.name
    r = subprocess.run(["gdalwarp", "-q", "-overwrite", "-t_srs", "EPSG:4326",
                        "-te", *[f"{v:.9f}" for v in te], "-ts", str(n), str(n),
                        "-r", resampling, "-of", "GTiff", "/vsicurl/" + url, out],
                       capture_output=True)
    if r.returncode != 0:
        Path(out).unlink(missing_ok=True)
        return None
    a = np.array(Image.open(out))
    Path(out).unlink(missing_ok=True)
    return a


def canopy_tile(index, te):
    r = subprocess.run(["ogrinfo", "-ro", "-al", index, "-spat",
                        *[f"{v:.9f}" for v in te]], capture_output=True, text=True)
    hits = [l.split("=")[1].strip() for l in r.stdout.splitlines() if "tile (String)" in l]
    return hits[0] if len(hits) == 1 else None


def main():
    index = sys.argv[1]
    print(f"{'candidate':<20}{'tree%':>6}{'open%':>6}{'built%':>7}{'water%':>7}"
          f"{'chm_mean':>9}{'chm_p90':>8}{'chm_max':>8}{'open_chm':>9}{'relief':>8}  GLO-30")
    print("-" * 105)
    rows = []
    for name, lat, lon in CANDIDATES:
        te = box(lat, lon)
        lc = window(WC + f"ESA_WorldCover_10m_2021_v200_{wc_tile(lat, lon)}_Map.tif", te, 110, "near")
        if lc is None:
            print(f"{name:<20}  no WorldCover tile")
            continue
        tile = canopy_tile(index, te)
        chm = window(CHM2 + f"chm/{tile}.tif", te, 256, "near") if tile else None

        tree = float((lc == 10).mean()) * 100
        # Open ground is the internal control for G2: a DSM minus a DTM must be ~0 there.
        open_ = float(np.isin(lc, (30, 40, 60)).mean()) * 100
        built = float((lc == 50).mean()) * 100
        water = float((lc == 80).mean()) * 100

        if chm is None:
            stats = ("    n/a", "   n/a", "   n/a", "     n/a")
            open_chm = float("nan")
        else:
            big = np.array(Image.fromarray(lc).resize((256, 256), Image.NEAREST))
            om = np.isin(big, (30, 40, 60))
            open_chm = float(chm[om].mean()) if om.any() else float("nan")
            stats = (f"{chm.mean():7.2f}", f"{np.percentile(chm, 90):6.1f}",
                     f"{chm.max():6d}", f"{open_chm:7.2f}" if om.any() else "    n/a")

        # GEDTM30 relief over the box. Steep ground inflates a DSM-minus-DTM test
        # through horizontal misregistration, so flatter is a cleaner G2 target.
        dtm = window(GEDTM30, te, 40, "bilinear")
        relief = f"{float(dtm.max() - dtm.min()):7.1f}" if dtm is not None else "    n/a"

        cell = glo_cell(lat, lon)
        has_glo = subprocess.run(["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
                                  f"{GLO}{cell}/{cell}.tif"], capture_output=True,
                                 text=True).stdout.strip() == "200"
        print(f"{name:<20}{tree:6.1f}{open_:6.1f}{built:7.1f}{water:7.1f}"
              f"{stats[0]:>9}{stats[1]:>8}{stats[2]:>8}{stats[3]:>9}{relief:>8}  {'yes' if has_glo else 'NO'}")
        rows.append((name, lat, lon, tree, open_, built, water, open_chm, has_glo))

    print("\nWanted for G2: high tree%, a real open% as internal control (DSM-DTM must be")
    print("~0 there), near-zero built% and water%, and GLO-30 present.")


if __name__ == "__main__":
    main()
