# Verified source formats

SPEC.md 4.5 forbids hardcoding anything about a dataset that has not been inspected.
Everything below was verified on **2026-10-23** with the command shown. Nothing here is
inferred from documentation.

---

## 1. Terrain — GEDTM30 v1.2

**URL**
`https://s3.opengeohub.org/global/dtm/v1.2/gedtm_rf_m_30m_s_20060101_20151231_go_epsg.4326.3855_v1.2.tif`

Verified with `gdalinfo /vsicurl/<URL>`:

| Property | Value |
|---|---|
| CRS | EPSG:4326 (`Data axis to CRS axis mapping: 2,1` — latitude first) |
| Size | 1296009 x 540009 |
| Pixel size | 0.000277777777778 deg (1 arcsec) |
| Extent | lon -180.00125 .. 180.00125, **lat -65.00125 .. 85.00125** |
| Type | Float32 |
| NoData | 3.4028235e+38 |
| Offset / Scale | 0 / **0.1 — THIS TAG IS WRONG, see below** |
| Layout | `LAYOUT=COG`, DEFLATE, `PREDICTOR=2`, 2048x2048 blocks, 10 overview levels |
| Vertical datum | `VERTICAL_DATUM=EGM2008/EPSG:3855` (geoid heights, gotcha 6) |
| `AREA_OR_POINT` | Area |

### G0: the scale tag is wrong — do not apply it

```
$ gdallocationinfo -wgs84 /vsicurl/<URL> -96.79 46.88
  Band 1:
    Value: 280.299987792969
    Descaled Value: 28.0299987792969
```

Fargo, ND sits at roughly 274-280 m (EGM2008 orthometric). The **raw stored value is
already metres**; applying `Scale: 0.1` gives 28 m, which is impossible.

SPEC.md 9 anticipated both outcomes: "Raw ~ 2750 confirms decimetres. Raw ~ 275 means the
metadata is wrong; report it, don't apply it." The measured value is 280.3, so the
**second branch holds: GEDTM30 is read with no scale applied.**

The latitude bounds above are the only source bounds hardcoded in `validate_request()`
(`src/request.cpp`).

---

## 2. Land cover — ESA WorldCover v200 (2021)

**URL pattern** (verified by S3 `list-type=2` listing and an HTTP 200 on the Fargo tile):
`https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_<TILE>_Map.tif`

`<TILE>` is the **south-west corner** of a 3 x 3 degree tile, e.g. `N45W099`. Confirmed:
`N45W099` has corners 45-48 N / 99-96 W, so it contains the Fargo test tile (46.88, -96.79).

Verified with `gdalinfo` on `N45W099`:

| Property | Value |
|---|---|
| CRS | EPSG:4326 |
| Size | 36000 x 36000 |
| Pixel size | 0.000083333333333 deg (0.3 arcsec, ~10 m) |
| Type | **Byte**, `ColorInterp=Palette` |
| NoData | **none declared** |
| Scale | none |
| Layout | `LAYOUT=COG`, DEFLATE, 1024x1024 blocks |

Legend is embedded in the file's metadata and matches SPEC.md 7.2:
`10` Tree cover, `20` Shrubland, `30` Grassland, `40` Cropland, `50` Built-up,
`60` Bare/sparse, `70` Snow and ice, `80` Permanent water, `90` Herbaceous wetland,
`95` Mangroves, `100` Moss and lichen. **MVP uses class 10 as the tree mask.**

No global latitude bound was verified, so none is hardcoded.

---

## 3. Canopy height — Meta/WRI

**The tile index mechanism SPEC.md 7.3 flagged as unverified now works end to end.**

Each version has a `tiles.geojson` at its root: EPSG:4326 polygons with a single `tile`
(String) attribute. An OGR spatial filter returns exactly one covering tile:

```
$ ogrinfo -ro -al /vsicurl/<BASE>/tiles.geojson -spat -96.80 46.87 -96.78 46.89
  tile (String) = 021330310        # v1
  tile (String) = 0213303100       # v2
```

Rasters live at `<BASE>/chm/<TILE>.tif`, where `<BASE>` is
`https://dataforgood-fb-data.s3.us-east-1.amazonaws.com/forests/...`:

| | v1 `v1/alsgedi_global_v6_float` | v2 `v2/global/dinov3_global_chm_v2_ml3` |
|---|---|---|
| tiles in index | 56,145 | 213,109 |
| index lat extent | -56.944974 .. 83.277705 | -56.752723 .. 83.236426 |
| Fargo tile | `021330310` (9-digit quadkey, z9) | `0213303100` (10-digit quadkey, z10) |
| Size | 65536 x 65536 | 32768 x 32768 |
| CRS | EPSG:3857 | EPSG:3857 |
| Pixel size | 1.194328566955875 m | 1.194328566955846 m |
| Type | **Byte** | **Byte** |
| Units | `UNITS=METERS` | (same raster convention) |
| NoData | none declared | none declared, **PER_DATASET mask band** |
| Scale | none | none |
| Layout | **not a COG** — 65536x1 strips, PREDICTOR=2, no overviews | **COG** — 512x512 blocks, 6 overview levels |

Three consequences:

1. **Canopy height is uint8 metres**, despite "float" in the v1 path name. Tree heights
   are therefore **quantised to 1 m**. This constrains M3 tree detection and the G4
   archetype discussion.
2. **EPSG:3857 pixels are not ground metres.** At latitude 47 the 1.1943 m pixel is
   about `1.1943 * cos(47)` = **0.81 m on the ground**. The fetch resolution for this
   source must account for Mercator distortion.
3. **v1's 65536x1 strip layout makes windowed reads expensive** (a small window forces
   full-width strip decompression); v2 is a proper COG. A practical argument for v2 at
   G3, to be presented there alongside the quality comparison.

---

## 4. Buildings — Overture Maps

Releases present (S3 listing of `release/`): `2026-09-23.0`, `2026-09-23.1`,
**`2026-10-21.0`** (latest). One release must be pinned and recorded in the manifest.

Buildings live at:
`release/<RELEASE>/theme=buildings/type=building/part-NNNNN-<uuid>-c000.zstd.parquet`

The partition is **flat — there is no geographic partitioning in the key names**, so
`bbox` struct row-group pruning is the only way to narrow the read, exactly as
SPEC.md 8.3 assumes.

---

## M2: measured fetch behaviour (2026-10-23)

### Output resolution is derived, not hardcoded

SPEC.md 6.3 says each fetcher reprojects "at the source's native resolution". A source
pixel is not square on the ground, so `fetch_raster()` measures it: it projects the tile
centre into the source CRS, steps one pixel along each axis, projects both back into UTM,
and takes the **finer** of the two distances. The coarser axis is then merely
interpolated and no detail the source carries is discarded.

Measured for the Fargo tile (latitude 46.88, so cos φ = 0.6833), each matching the
prediction from the source's own geometry:

| Source | Native | Predicted ground res | Measured output |
|---|---|---|---|
| GEDTM30 | 1 arcsec, EPSG:4326 | 30.87 · cos φ = 21.1 m | **21.1728 m** |
| WorldCover | 0.3 arcsec, EPSG:4326 | 9.26 · cos φ = 6.33 m | **6.3518 m** |
| Canopy v1/v2 | 1.1943 m, EPSG:3857 | 1.1943 · cos φ = 0.816 m | **0.8152 m** |

EPSG:4326 sources shrink east-west by cos φ; EPSG:3857 sources are inflated by 1/cos φ.
The two corrections run in opposite directions, which is why they cannot share a constant.

### Cached raster check, Fargo tile

All four share the padded north-west corner `(667829.644455594, 5194760.116763453)`.

| File | Size | Type | Range |
|---|---|---|---|
| `terrain.tif` | 53 x 53 | Float32 | 262.65 – 288.42 m, mean 276.16 |
| `landcover.tif` | 176 x 176 | Byte (palette) | 10 – 60 |
| `chm_v1.tif` | 1374 x 1374 | Byte | 0 – 29 m, mean 0.74 |
| `chm_v2.tif` | 1374 x 1374 | Byte | 0 – 20 m, mean 1.01 |

**The terrain range independently re-confirms G0.** Fargo sits at roughly 274–280 m and
the raster reads 262–288 m with mean 276. Had the `Scale: 0.1` tag been applied the tile
would read 26–29 m.

**v1 and v2 disagree on this tile** (max 29 m vs 20 m, mean 0.74 vs 1.01). That
disagreement is G3's subject, not something to reconcile here.

### Overture query cost

The buildings partition holds **512 part files** with no geographic partitioning, so
DuckDB must read the footer of every one over S3 before `bbox` row-group pruning can
narrow anything. The first `fetch` therefore spends minutes in the buildings stage. This
is inherent to the layout, not a defect, and the cache makes it a one-time cost.

### Buildings, Fargo tile

795 footprints, written as FlatGeobuf in EPSG:32614 with an `id` string field. The layer
extent `(667772.78, 5193579.77) - (669034.14, 5194818.69)` slightly exceeds the padded
box, which is correct: the `bbox` filter is an overlap test, so a footprint straddling
the edge comes back whole rather than clipped.

**File format choice.** FlatGeobuf, not GeoJSON (which mandates WGS84, so UTM
coordinates in it would mislead every tool that opens the file) and not GeoPackage
(SQLite, which embeds timestamps and would change the provenance SHA-256 every run).
The layer is written with `SPATIAL_INDEX=NO` and the query carries `ORDER BY id`, so the
bytes are a straight function of the data.

### Two GDAL traps found while building this

Both have the same root cause: **GDAL drivers select behaviour from the output file
extension, and an unrecognised extension fails quietly.** Fetchers write to a temporary
name and rename on success, and the obvious temporary name `<final>.partial` broke both:

- **GTiff wrote nothing and reported no error.** `GDALWarp` returned a usage error with
  an empty `CPLGetLastErrorMsg()`. Reproduced with the `gdalwarp` CLI: the same command
  writing `probe.tif` succeeds and writing `probe.tif.partial` silently produces no file.
- **FlatGeobuf wrote a directory**, not a file: for an unrecognised extension it creates
  a directory holding one `.fgb` per layer. The failure only surfaced later, as
  `sha256: read failed`, because the "file" was a directory.

Fixes, both kept: `-of GTiff` is passed explicitly, and `partial_path()` inserts the
marker **before** the extension (`terrain.partial.tif`) so the driver still sees it.

### Reproducibility and offline operation

**A second `fetch` makes no network requests — proven, not timed.** Running it inside a
network namespace with no connectivity (`unshare -rn`) succeeds in 1.1 s and exits 0,
with all five sources reported cached. A control `curl` in the same namespace fails.

**Two independent fetches into clean cache directories are byte-identical** across all
five cached files and `provenance.json`, which is what SPEC.md 4.3 requires of the
cached raw pulls.

## Corrections this record implied for SPEC.md — all applied 2026-10-23

1. **7.1** — "Not yet verified: that scale 0.1 means stored decimetres" is now verified
   **false**. Values are metres; the scale tag must not be applied.
2. **9, gate G0** — the measured raw value is 280.3, so the "metadata is wrong" branch fired.
3. **8.2** — drop the `scale` parameter from `load()`. No verified source needs one
   (GEDTM30 metres, WorldCover palette codes, canopy uint8 metres), so threading a
   constant 1.0 through would be an abstraction with no second use. Also, the
   unconditional nodata check does not fit WorldCover or canopy: neither declares
   NoData, and canopy v2 carries a PER_DATASET mask band instead.
4. **7.2** — tile naming and the HTTPS endpoint are confirmed; record Byte/Palette and
   the absence of a declared NoData.
5. **7.3** — record uint8 metres in EPSG:3857, v1's non-COG layout, and `tiles.geojson`
   as the verified lookup mechanism.

All five were written into SPEC.md after the user approved G0 on 2026-10-23.
