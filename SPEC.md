# SPEC: Real-world airborne LiDAR scene generator

## 1. Goal

Given a tile request (center latitude/longitude, extent in meters, random seed), produce a labeled airborne LiDAR point cloud (LAS) of that real location. The scene is built from global open geodata. Procedural generation fills only detail finer than the datasets resolve, and it is parameterized by those datasets, never arbitrary.

The scanner in this codebase is a deliberately simple stand-in (section 6.9). The user's own sensor simulator is proprietary, lives outside this codebase, and will later read the same scene geometry (section 6.8).

Long-term, a user asks for a region ("North Dakota") plus an extent, and the program picks a tile inside it. That sampler is out of scope for the MVP (section 12).

Research context: the output supports a fidelity study, meaning how much real-data conditioning matters for synthetic point clouds, and comparison against real airborne LiDAR. This is the reason for the strict reproducibility and no-fallback rules below.

## 2. Decisions already made

These are settled. Do not relitigate them without new evidence.

| Decision | Reason |
|---|---|
| Airborne LiDAR only | The sensor sees the whole tile from above, so tile-wide fidelity matters. |
| Generator inputs must have global coverage | Rules out US-only data: USGS 3DEP, USFS TreeMap, USDA CDL, NLCD. Real airborne LiDAR may be used for **validation only**. |
| Fully separate from proc3d | No code reuse. Procedural code is written fresh to fit these datasets. |
| C++ with CMake | User's choice. |
| GDAL for rasters, projections, geometry; DuckDB for Overture | One library per job. |
| Scanner = simple nadir raycaster, a stand-in | The user's sensor simulator is proprietary and will replace it. The stand-in exists to build and validate everything upstream of the sensor. |
| Scene geometry contract is the boundary between scene and sensor | Any scanner, the stand-in or the user's, reads only these documented files. |
| Two primitive types: opaque triangles and vegetation voxels | Terrain is triangulated rather than kept as a raster, so every opaque surface is handled one way. |
| Tree crowns as voxels with plant area density (PAD), not leaf meshes | Airborne footprints don't resolve leaves. Beer–Lambert interception through PAD voxels is the standard turbid-medium model in LiDAR and radiative-transfer simulation (section 13). |
| No species library | No global dataset provides species. Trees are structural archetypes. |
| Terrain = GEDTM30 v1.2 | Bare earth, one global Cloud-Optimized GeoTIFF, CC BY 4.0. |
| FABDEM rejected | Distributed as 10°×10° zip archives (no windowed reads), and non-commercial license. |
| Copernicus GLO-30 and SRTM rejected as terrain | They are surface models (DSMs) that include canopy and roofs, so placing trees and buildings on them double-counts height. GLO-30 is still used as validation data (gate G2). |
| No fallbacks | Each stage has fixed required inputs. Missing coverage or bad data is an error, never silent degradation. |
| Target point spacing 14 cm (~51 pts/m²) | User's requirement. Sets the scanner's sample grid and the required mesh fidelity (features of roughly 30 cm and up are resolved). |
| Output = uncompressed LAS 1.4 with ASPRS class codes | Written directly with no dependency. Standard codes (2 ground, 5 high vegetation, 6 building) keep output readable by any LiDAR tool. |
| Viewer = Polyscope (C++, MIT; bundles Dear ImGui and GLFW) | Approved by the user. Registers meshes and point clouds with a built-in UI panel, and supports headless EGL rendering with `polyscope::screenshot()` on Linux, so the agent can verify its own rendering. |

## 3. Out of scope for the MVP

- Region-to-polygon lookup and tile sampling
- Roads, crops, water bodies, soil, surface reflectance
- Leaf type, leaf-on/leaf-off, season-dependent PAD
- Procedural terrain micro-relief
- Per-building heights and roof shapes
- Fidelity tiers and ablation tooling
- Viewer: click-on-a-map location picker (deferred, section 12)
- Tree stems. Vertical surfaces get essentially no returns from nadir rays, so stems can't be verified until an off-nadir scanner exists.
- LAZ compression (needs a library)
- Multithreading in the scanner, until a measurement shows it's needed

**Rejected outright (do not build):**
- Photorealistic textures in the viewer. There's no imagery fetcher, and visual realism isn't what this product measures.
- Editing the scene in the viewer. Edits that aren't captured by the request and seed break reproducibility.
- Live regeneration while inputs change. Runs take seconds to minutes; the viewer uses an explicit Run button.
- Stable Diffusion, or any image model, applied to rendered scenes. It edits pixels, not geometry: there's nothing to scan, no ground-truth labels, and outputs aren't reproducible across hardware.
- Generative 3D models as a pipeline component. They are allowed only as an offline, last-resort authoring source for the object library (section 12), behind the same validation as every other asset.
- Replacing real building footprints with generated buildings. Real data is never swapped for invented data.
- **Any sensor physics in this codebase:** off-nadir rays, multiple returns, noise, intensity, beam footprint, waveform. The user's proprietary simulator owns all of these. The stand-in scanner stays exactly as specified in 6.9.

## 4. Hard constraints

1. **Fail fast.** Any unmet precondition throws with a message naming the source, the tile, and what was wrong. Examples: no coverage, a nodata pixel inside the tile, an unexpected dtype or scale, a failed HTTP read.
2. **No fallbacks or alternative paths.** One correct path per stage. If a source can't serve a tile, the run fails.
3. **Reproducibility.** Tile request + seed + cached raw pulls + pinned dataset versions must regenerate a byte-identical point cloud. Every source of randomness takes its seed from the request seed. The scanner draws its random numbers from a counter-based hash, so output never depends on evaluation order.
4. **Two interfaces, no shortcuts.** The scene spec is the only input to the builders; they never read cached raw pulls directly. The scene geometry contract is the only scene input to the scanner and the viewer.
5. **Never guess a dataset's format.** Inspect with `gdalinfo` (rasters) or a schema query (Parquet) and record the result in the source's notes. Hardcode only what you've verified.
6. **No new dependencies** beyond GDAL, DuckDB, and Polyscope without asking the user.

## 5. Environment

- The production compute cluster's **compute nodes have no internet access.** The pipeline is therefore split into two subcommands:
  - `fetch`: network required. Runs on a networked machine and writes everything to a cache directory.
  - `generate`: fully offline. Reads only the cache and writes the point cloud.
- The cache directory is the only thing that moves between machines. `generate` must never open a network connection.
- The cluster runs containers with Apptainer/Singularity. Containerizing comes after the MVP works natively.
- The viewer runs on the user's workstation. Screenshot mode must also work headless through Polyscope's EGL backend (Linux only).

## 6. Architecture

```
fetch:     Tile request → Shared grid → Fetchers → cache
generate:  cache → Scene spec → Builders → Scene geometry (contract) → Scanner → LAS + manifest
viewer:    reads the scene geometry and LAS; launches fetch and generate
```

`fetch` is the only stage that touches the network. The scene geometry contract (6.8) is the boundary between scene and sensor.

### 6.1 Tile request
Center latitude/longitude, extent in meters (MVP: 1000), and seed. Validated before anything else runs: latitude within the intersection of all sources' coverage (GEDTM30: −65° to 85°; other sources' bounds to be added once verified) and extent > 0. Invalid requests exit non-zero.

### 6.2 Shared grid
Defines the coordinate frame:
- UTM zone `floor((lon + 180) / 6) + 1`; EPSG `32600 + zone` (north) or `32700 + zone` (south).
- Tile bounding box in meters, plus a padded bounding box (a few source pixels of margin) for resampling.
- **Local origin:** the padded box's southwest corner, stored as doubles. All geometry in the contract is float32 relative to it (gotcha 9).
- Longitude/latitude bounding box for vector queries.
- 1 m terrain mesh resolution and 0.14 m scanner sample spacing.

### 6.3 Fetchers
One per source, independent of each other. Each one:
1. does a windowed read covering only the padded bounding box
2. reprojects into the grid's UTM CRS at the source's native resolution
3. writes the result to the cache
4. records provenance: URL, dataset version, acquisition date or vintage if known, and a SHA-256 of the cached file

Raster fetchers use one shared function (section 8.1). Use nearest-neighbor resampling for categorical data and bilinear for continuous data.

### 6.4 Scene spec
The canonical description of the tile, written to disk. Assembly also derives the **tree list** from the canopy height raster, masked to the WorldCover tree class (code 10). Rough schema; the on-disk format is an open decision:

```
grid:       utm_epsg, bbox_m, local_origin
rasters:    terrain (m, EGM2008), landcover (WorldCover codes), chm (m)  — paths into cache
buildings:  [{id, polygon_utm, height_m}]
trees:      [{x, y, ground_z, height_m, crown_radius_m}]
provenance: per source {url, version, vintage, sha256}
request:    lat, lon, extent_m, seed
```

### 6.5 Terrain builder
Bicubically upsamples the terrain raster to a 1 m grid over the padded tile and triangulates it: two triangles per cell, with a fixed diagonal direction. The result goes into the opaque mesh with class 2 (ground). Terrain is not a separate primitive.

### 6.6 Tree builder
Crowns only for the MVP (stems are out of scope, section 3). Each crown is an envelope sized from the tree's height and crown radius, voxelized on a **tile-wide lattice** aligned to the local origin. Every voxel inside the envelope gets the archetype's PAD. Because all crowns share one lattice, overlapping crowns combine exactly by summing PAD per voxel.

The MVP uses one archetype for every tree: a single PAD, nadir projection coefficient G, crown-base ratio, envelope shape, and voxel size.

### 6.7 Building builder
Extrudes each footprint into a flat-roofed solid. The base sits at the minimum terrain elevation under the footprint. **One height rule (a constant) for every footprint.** Overture's optional height attribute is deliberately ignored, so the scene never mixes sourced and guessed heights. Class 6 (building), one object ID per building.

### 6.8 Scene geometry contract
The files any scanner reads. **The user's proprietary simulator will be built against this, so it must be exact and documented.**

- `scene_geometry.json`: format version, EPSG code, local origin, padded bounding box, the object table `{object_id, class, source}`, and the crown archetype parameters (PAD, G, voxel size).
- `opaque.ply`: binary little-endian PLY. Vertices are float32 `x, y, z` in local coordinates. Faces are uint32 index triples, each with a uint32 `object_id` and a uint8 `class`.
- `crowns.bin`: one record per crown. A header (object_id, integer lattice origin, dimensions) is followed by float32 PAD values in a documented order.
- `FORMAT.md`: byte-level documentation of every file above. Written by the agent; this is the deliverable the user's simulator is built against.

Rules:
- Opaque surfaces are triangles only. Vegetation is PAD voxels only.
- Every primitive carries an object ID and a class.
- Any change to any file bumps the format version.

### 6.9 Scanner (stand-in)
Reads only the contract. Treats every sample as a ray cast straight down, which is equivalent to an orthographic depth buffer rendered from above.

- **Sampling:** a grid of 0.14 m cells over the tile (not the padded box), with one sample per cell at a seeded random offset inside the cell. That gives exactly 1/0.14² samples per m² without a perfect lattice.
- **Opaque hits:** rasterize every triangle onto the samples inside its xy bounding box, using an inclusive point-in-triangle test. Keep the maximum z. Break exact ties by the smallest object ID.
- **Vegetation:** for each sample, walk the voxel column from the top down to the opaque z. A voxel with PAD > 0 intercepts the ray with probability `p = 1 − exp(−G · PAD · voxel_size)`. On interception, sample the hit depth inside the voxel from the truncated exponential distribution with rate `G · PAD`. The point gets class 5 and the crown's object ID. If no voxel intercepts, the point is the opaque hit.
- **One return per sample.** No noise, intensity, off-nadir rays, or multiple returns (section 3).
- **Randomness:** every draw comes from a counter-based hash (e.g. SplitMix64) of (seed, sample index, draw index).
- **Single-threaded.** Parallelize only if a measurement shows it's too slow. Thanks to the counter-based hash, parallelizing won't change the output.

### 6.10 Output
- **LAS 1.4, point format 6, written directly:**
  - coordinates as int32 with scale 0.001 m and offset = local origin
  - an OGC WKT CRS VLR, with the WKT bit set in the global encoding
  - ASPRS classification codes
  - an extra-bytes VLR declaring a uint32 `object_id`
  - fields the stand-in doesn't model written as 0, except return number = number of returns = 1

  That's about 34 bytes per point, roughly 1.7 GB per 1 km tile.
- **`manifest.json`:** tile request, seed, Overture release, all provenance from the scene spec, contract format version, and the generator's git commit.

### 6.11 Viewer
A separate executable built on Polyscope. Three rules:

1. **No scene logic.** The viewer reads only the scene geometry contract and the LAS, which is exactly what the scanner reads and writes. It never rebuilds geometry from the scene spec.
2. **It launches the CLI; it doesn't call into the pipeline.** The UI panel has fields for latitude, longitude, extent, and seed, plus a Run button. Run executes `fetch` then `generate` as subprocesses, shows their output, and displays errors verbatim. GUI and CLI runs are therefore byte-identical, and slow fetches never block the window.
3. **`--screenshot <path>` mode.** Loads a run's outputs, renders one frame to a PNG, and exits. It must work headless via the EGL backend. This is how the agent checks its own rendering work, and the PNGs serve as gate evidence.

Layer toggles: terrain, buildings, crowns (voxel centers colored by PAD), and (V2) the point cloud colored by class.

## 7. Data sources

### 7.1 Terrain: GEDTM30 v1.2
- **URL:** `https://s3.opengeohub.org/global/dtm/v1.2/gedtm_rf_m_30m_s_20060101_20151231_go_epsg.4326.3855_v1.2.tif`
- **Verified with `gdalinfo` on the 30 m file:**
  - EPSG:4326, pixel size 0.000277777777777778° (1 arcsecond), latitude −65° to 85°
  - `LAYOUT=COG`, DEFLATE compression, `PREDICTOR=2`, 2048×2048 blocks, overviews present
  - Float32, NoData `3.4028235e+38`, Offset 0, **Scale 0.1 — this tag is wrong, see below**
  - `VERTICAL_DATUM=EGM2008/EPSG:3855` (geoid heights), `AREA_OR_POINT=Area`
- **Verified at G0 (2026-10-23): the scale tag is wrong. Read this source unscaled.** `gdallocationinfo -wgs84 <URL> -96.79 46.88` returns a raw value of **280.3**, and Fargo sits at roughly 274–280 m (EGM2008). Applying scale 0.1 would give 28 m. **Stored values are already meters**, so no scale is applied when loading.
- **Status:** the Zenodo record labels it a first release, "testing purposes only."
- **Docs:** Zenodo record 18887460; documentation at codeberg.org/openlandmap/GEDTM30; methods paper in PeerJ, doi:10.7717/peerj.19673.

### 7.2 Land cover: ESA WorldCover v200 (2021), 10 m
- COG tiles on AWS Open Data (`s3://esa-worldcover`), public, no authentication.
- **Verified 2026-10-23.** Tiles are 3°×3°, named by their **southwest corner**, served at `https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_<TILE>_Map.tif`. `N45W099` has corners 45–48 N / 99–96 W, so it contains the Fargo test tile.
- **Verified with `gdalinfo`:** EPSG:4326, 36000×36000, pixel 0.0000833333° (0.3 arcsec), **Byte, ColorInterp=Palette, no NoData declared, no scale**, `LAYOUT=COG`, DEFLATE, 1024×1024 blocks. The class legend is embedded in the file metadata and matches the list below.
- No global latitude bound was verified, so none is hardcoded in request validation; a tile outside coverage fails at `fetch` when the read fails.
- MVP use: class 10 (tree cover) as the tree mask. All other classes are pulled but don't drive anything yet.

### 7.3 Canopy height: Meta/WRI high-resolution canopy height maps, ~1 m
- v1: `s3://dataforgood-fb-data/forests/v1/alsgedi_global_v6_float/`
- v2 (CHMv2, DINOv3-based): `s3://dataforgood-fb-data/forests/v2/global/dinov3_global_chm_v2_ml3/`
- Both are public (`--no-sign-request`); HTTPS base `https://dataforgood-fb-data.s3.us-east-1.amazonaws.com/forests/...`.
- **Index mechanism verified 2026-10-23.** Each version has a `tiles.geojson` at its root: EPSG:4326 polygons with a single `tile` (String) attribute. An OGR spatial filter returns exactly one covering tile. Rasters are at `<BASE>/chm/<TILE>.tif`. Fargo → v1 `021330310` (9-digit quadkey, z9), v2 `0213303100` (10-digit, z10).
- **Verified with `gdalinfo`, both versions: Byte (uint8) in meters, EPSG:3857, pixel 1.1943 m, no NoData and no scale.** Two consequences:
  - Despite "float" in the v1 path name, canopy height is **uint8, so tree heights are quantized to 1 m.** This constrains tree detection (M3) and the crown archetype (G4).
  - EPSG:3857 pixels are not ground meters: at latitude 47 the 1.1943 m pixel is ≈ `1.1943·cos φ` = **0.81 m on the ground.** The fetch resolution must account for Mercator distortion.
- **Layout differs:** v1 is **not a COG** (65536×1 strips, `PREDICTOR=2`, no overviews); v2 **is** a COG (512×512 blocks, 6 overview levels, `PER_DATASET` mask band in place of a NoData value). Measured at G3, the read-cost penalty is real but modest — a 1 km windowed read took 0.92 s for v1 against 0.50 s for v2 — so this is a minor practical argument, not a decisive one.
- Source imagery vintage varies by location, so expect disagreements with the other layers where land has changed.
- Which version to use is decided at gate G3.

### 7.4 Buildings: Overture Maps
- `s3://overturemaps-us-west-2/release/<RELEASE>/theme=buildings/type=building/*`: GeoParquet, EPSG:4326, with `bbox` struct columns for row-group pruning.
- Get releases with `aws s3 ls --no-sign-request s3://overturemaps-us-west-2/release/`. Pin one release and record it in the manifest. **Verified 2026-10-23:** releases `2026-09-23.0`, `2026-09-23.1`, `2026-10-21.0` (latest).
- **Verified:** the partition is **flat** — keys are `part-NNNNN-<uuid>-c000.zstd.parquet` with no geographic partitioning — so `bbox` row-group pruning is the only way to narrow the read.
- MVP uses geometry only.
- Completeness varies, and rural recall in particular can be poor. A building missing from the data doesn't mean it's missing on the ground.

### 7.5 Validation-only data (never generator inputs)
- **Copernicus GLO-30 DSM** (AWS Open Data) for gate G2.
- **Real airborne LiDAR tiles** (e.g. USGS 3DEP) for post-MVP scan validation.

## 8. Reference code

These are sketches, not final code. They encode verified gotchas.

### 8.1 Raster fetch

At program startup, not inside the fetcher:

```cpp
CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
```

```cpp
/**
 * @brief Fetches one raster source for a tile, reprojected onto the tile's UTM grid.
 *
 * Opens a remote Cloud-Optimized GeoTIFF over HTTP (/vsicurl/) and warps the
 * region covering @p b into @p utm_epsg at resolution @p res. GDAL range-reads
 * only the internal blocks that overlap the bounding box. The result is written
 * to @p out as a GeoTIFF and becomes the cached raw pull for this source.
 *
 * Pixel values are written as stored in the source. Scale and offset are NOT
 * applied. No verified source needs a scale (section 8.2), so the loader applies
 * none either.
 *
 * @param url         HTTPS URL of the source COG, without the /vsicurl/ prefix.
 * @param utm_epsg    EPSG code of the tile's UTM zone (e.g. 32614).
 * @param b           Padded bounding box in UTM meters.
 * @param res         Output pixel size in meters (the source's native resolution).
 * @param resampling  "near" for categorical data, "bilinear" for continuous data.
 * @param out         Path of the output GeoTIFF.
 *
 * @throws std::runtime_error if the source cannot be opened or the warp fails.
 */
void fetch_raster(const std::string& url, int utm_epsg, Bbox b,
                  double res, const char* resampling, const std::string& out) {
    GDALDatasetH src = GDALOpen(("/vsicurl/" + url).c_str(), GA_ReadOnly);
    if (!src) throw std::runtime_error("open failed: " + url);

    CPLStringList argv;
    for (const std::string& a : {
            std::string("-t_srs"), "EPSG:" + std::to_string(utm_epsg),
            std::string("-te"), std::to_string(b.xmin), std::to_string(b.ymin),
                                std::to_string(b.xmax), std::to_string(b.ymax),
            std::string("-tr"), std::to_string(res), std::to_string(res),
            std::string("-r"), std::string(resampling)})
        argv.AddString(a.c_str());

    GDALWarpAppOptions* opts = GDALWarpAppOptionsNew(argv.List(), nullptr);
    int usage_err = 0;
    GDALDatasetH dst = GDALWarp(out.c_str(), nullptr, 1, &src, opts, &usage_err);
    GDALWarpAppOptionsFree(opts);
    GDALClose(src);
    if (!dst || usage_err) throw std::runtime_error("warp failed: " + url);
    GDALClose(dst);
}
```

### 8.2 Loading a cached raster

**No verified source needs a scale,** so `load()` takes none: GEDTM30 stores meters (its
scale tag is wrong, section 7.1), WorldCover stores palette codes, and both canopy
versions store uint8 meters. Threading a constant 1.0 through would be an abstraction
with no second use.

**The nodata check is per source, not universal.** Only GEDTM30 declares a NoData value.
WorldCover and canopy declare none, and canopy v2 carries a `PER_DATASET` mask band
instead — so a loader that requires a declared NoData would wrongly accept those two.

```cpp
std::vector<float> load(const std::string& path, int& w, int& h) {
    GDALDatasetUniquePtr ds(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
    if (!ds) throw std::runtime_error("open failed: " + path);
    GDALRasterBand* band = ds->GetRasterBand(1);
    w = band->GetXSize(); h = band->GetYSize();
    std::vector<float> v(size_t(w) * h);
    if (band->RasterIO(GF_Read, 0, 0, w, h, v.data(), w, h, GDT_Float32, 0, 0) != CE_None)
        throw std::runtime_error("read failed: " + path);
    int has_nd = 0;
    const float nd = static_cast<float>(band->GetNoDataValue(&has_nd));
    for (const float& x : v) {
        if (has_nd && x == nd) throw std::runtime_error("nodata in tile: " + path);
    }
    return v;
}
```

### 8.3 Overture query (DuckDB C++)

```cpp
duckdb::DuckDB db(nullptr);
duckdb::Connection con(db);
con.Query("INSTALL spatial; LOAD spatial; INSTALL httpfs; LOAD httpfs; SET s3_region='us-west-2';");
auto res = con.Query(
    "SELECT id, ST_AsText(geometry) FROM read_parquet("
    "'s3://overturemaps-us-west-2/release/" + RELEASE + "/theme=buildings/type=building/*') "
    "WHERE bbox.xmin < " + e + " AND bbox.xmax > " + w +
    " AND bbox.ymin < " + n + " AND bbox.ymax > " + s);
if (res->HasError()) throw std::runtime_error(res->GetError());
// rows: res->GetValue(1, i).ToString() → OGRGeometryFactory::createFromWkt → transform to UTM
```

### 8.4 Known gotchas

1. **Nodata comparison:** cast the double from `GetNoDataValue` to float before comparing. Otherwise `3.4028235e+38` never matches a Float32 pixel and the nodata check silently passes.
2. **Scale isn't applied by GDAL reads,** and the warped output may not carry scale metadata. That is harmless here because **no verified source needs a scale** — but never trust a file's scale tag without checking it against a known value. GEDTM30 declares `Scale: 0.1` and the tag is simply wrong (section 7.1, gate G0).
3. **EPSG:4326 axis order:** it is officially latitude-first (`gdalinfo` shows "Data axis to CRS axis mapping: 2,1"). Call `SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)` on every `OGRSpatialReference`, or coordinates swap silently.
4. **`GDAL_DISABLE_READDIR_ON_OPEN` is process-global.** Set it once at startup.
5. **GEDTM30 uses 2048×2048 blocks,** so each 1 km fetch pulls 1–4 compressed blocks (megabytes, not kilobytes). The cache is what makes this cheap.
6. **Vertical datum is EGM2008 (geoid).** Never mix with ellipsoidal heights (GEDI, many LiDAR products) without converting.
7. **Data vintages differ across layers** (canopy imagery, WorldCover 2021, current Overture). Disagreements are expected where land has changed. Surface them, don't paper over them.
8. **DuckDB `INSTALL` needs network.** That's fine because only `fetch` uses DuckDB.
9. **Float32 at UTM magnitudes.** Northings around 5,000,000 m have a float32 spacing of 0.5 m. Never store absolute UTM coordinates in float32. Contract geometry is relative to the double-precision local origin.
10. **Rasterization holes.** Use an inclusive edge test with max-z wins, not a top-left fill rule. A sample on a shared edge then hits both triangles, which is harmless, instead of neither.
11. **LAS 1.4 point formats 6 and up require a WKT CRS VLR** and the WKT bit in the header's global encoding. Readers may reject or misread files without them.

## 9. Milestones and gates

**At every GATE, stop.** Present the evidence (numbers, images, command output) to the user and wait for approval before continuing.

| Step | Work | Acceptance |
|---|---|---|
| M0 | CMake skeleton; GDAL and DuckDB found; `fetch` and `generate` subcommands; request validation | Invalid requests exit non-zero with a clear message. Also report whether this GDAL build has the Parquet driver (`ogrinfo --formats \| grep -i parquet`) and can read Overture from S3. If it can, the user may decide to drop DuckDB. **Done 2026-10-23: no Parquet driver** — `ogr_Parquet.so` ships with GDAL 3.13.3 but needs `libarrow.so.2500` while the system has Arrow 26, so it cannot load. **DuckDB is kept.** See `NOTES/environment.md`. |
| M1 | Shared grid | A test tile centered near Fargo, ND (46.88, −96.79) → EPSG:32614; UTM↔lon/lat round-trip error < 1 m |
| **G0** | GEDTM30 scale check: `gdallocationinfo -wgs84 -valonly /vsicurl/<URL> -96.79 46.88` | Raw ≈ 2750 confirms decimeters. Raw ≈ 275 means the metadata is wrong; report it, don't apply it. **PASSED 2026-10-23: raw = 280.3**, so the second branch holds — the scale tag is wrong and GEDTM30 is read unscaled (section 7.1) |
| M2 | Four fetchers, cache, provenance | `fetch` on the test tile produces 3 GeoTIFFs + a building file + provenance; a second run makes no network requests; `gdalinfo` results recorded per source |
| **G1** | Alignment | PNG overlays on one extent (DTM hillshade, land cover, canopy height, building outlines) agree on where things are |
| **G2** | Terrain source | On a forested tile (agent proposes one with high WorldCover tree fraction; user confirms), GLO-30 minus GEDTM30 looks like canopy height and roughly matches the canopy map |
| **G3** | Canopy version | v1 vs. v2 on the same tile, compared against the WorldCover tree mask; also report canopy heights inside Overture footprints (the rooftop contamination the mask removes); the user picks a version |
| M3 | Scene spec + tree detection | Detected crowns overlaid on the canopy map (PNG), with counts and height distribution; the agent proposes the detection method with evidence, the user approves |
| M4 | Builders → scene geometry contract | Contract files for the test tile plus `FORMAT.md`; a separate reader written only from `FORMAT.md` loads every file; terrain covers the padded tile with no gaps; every primitive has an object ID and class; no local coordinate exceeds the padded tile size |
| V1 | Viewer: scene geometry | First, an empty Polyscope window opens on the user's workstation and `--screenshot` writes a PNG headless; then terrain, buildings, and crowns render with toggles; screenshots of the test tile match the G1 overlays; Run on the test tile produces the same outputs as the CLI |
| M5 | Scanner (opaque) + LAS output | Synthetic flat plane: point count equals cell count exactly, all class 2, all z on the plane. Synthetic box building: roof points at roof height, none on walls. Every point within 1 mm of a contract surface. LAS opens in an independent reader the user approves (e.g. `pdal info`). Two runs give identical hashes. Measured density on open ground is exactly 1/0.14² pts/m² |
| **G4** | Crown model | A synthetic uniform-PAD voxel block: measured interception fraction matches `1 − exp(−G·PAD·depth)` within sampling error, for at least three PAD values. A vertical cross-section of vegetation points through one test-tile crown shows no banding at voxel layers. The user approves the archetype parameters |
| V2 | Viewer: point cloud | The LAS renders colored by class, overlaid on the scene geometry; a screenshot shows points sitting on the corresponding surfaces. A tile is ~50M points, so display-side decimation is allowed if it is shown on screen, never written back, and stated on any screenshot used as gate evidence |

## 10. Debugging protocol

Treat every bug as an investigation:
1. State a theory of the cause.
2. Collect evidence with minimal, targeted logging or a measurement.
3. Fix only once the evidence confirms the theory.

Fix root causes, not symptoms. Make surgical changes. Never stack speculative fixes.

## 11. Open decisions (the user owns these)

- **Canopy height map version** (G3)
- **Tree archetype:** PAD, G (default 0.5, the random leaf orientation value), crown-base ratio, envelope shape, voxel size (default 0.25 m, as in prior work). Settled at G4.
- **Building height constant**
- **Tree detection method** (the agent proposes at M3)
- **Scene spec on-disk format**
- ~~**Whether to drop DuckDB** in favor of GDAL's Parquet driver (based on the M0 report)~~ — **resolved 2026-10-23: keep DuckDB.** This GDAL build cannot load its Parquet plugin (Arrow 25 vs 26 ABI mismatch), and getting it would mean a custom GDAL build. DuckDB runs only inside `fetch`, so the offline `generate` path is unaffected either way.

## 12. Post-MVP, in rough order

1. **Region sampling.** Region name → polygon (Census TIGER for US states; Natural Earth or OSM globally) → tile. Uniform sampling first, restricted to areas where every required layer exists. Constraints (stratified by land cover, "must contain a road") only after seeing what uniform draws look like.
2. **Fidelity tiers for the ablation.** Fully procedural → statistics-conditioned → real placements, each tier with a fixed set of required layers.
3. **Leaf type and phenology.** Copernicus Global Land Service LC100 (100 m) or ESA CCI land cover (300 m) for needleleaf/broadleaf and evergreen/deciduous; Sentinel-2 or Dynamic World for leaf-on vs. leaf-off.
4. **PAD from global LAI** (MODIS or Copernicus LAI). This is a hypothesis to validate, not established practice.
5. **Terrain micro-relief** conditioned on local roughness statistics from the DTM. At 14 cm spacing, its absence (no furrows, ditches, or field edges) will be visible in the output, so this is higher priority than its position suggests.
6. **Building heights** (GHSL built height, 100 m) and roof shapes.
7. **Other layers:** roads (Overture transportation), crops (ESA WorldCereal), soil (SoilGrids), reflectance at the sensor wavelength (Sentinel-2 NIR).
8. **Object library and placement.** Off-road contexts need no new layers and go first. Road and parking contexts need item 7.
   - **One library, one interface.** Every object is a mesh plus metadata: class, metric dimensions, ground-contact origin, forward axis, provenance, license. The pipeline never knows how a mesh was authored.
   - **Authoring sources, in order of preference:**
     1. the user's lab's existing vehicle models
     2. real mesh collections with per-object licenses (Objaverse, ShapeNet, commercial marketplaces)
     3. parametric families for within-class variety (dimensions sampled from real measured 3D-box distributions, simple articulation such as turret rotation)
     4. generative 3D, only for a class with no available real mesh
   - **Ingestion validation, required for every asset.** Checks:
     - dimensions within the class's reference range
     - orientation normalized
     - the outer surface returns points when scanned by the project's scanner
     - geometry accurate at roughly 30 cm scale, since that is what 14 cm spacing resolves
     - a thumbnail via the viewer's `--screenshot`
     Failing assets are rejected, never patched.
   - **Placement principle.** No global dataset gives object positions, so placement is a procedural tier. Two parts are kept separate:
     - **hard constraints** decide where an object physically *can* be; they always hold and come from data
     - **per-context densities** decide how many objects go where; they are explicit parameters, recorded in the manifest, so context bias is controlled and ablatable rather than accidental
   - **Contexts.** Each is a mask on the 1 m grid. Area contexts have densities per hectare; linear contexts have densities per km of length.

     | Context | Kind | Source | Orientation |
     |---|---|---|---|
     | Open ground | area | WorldCover grass, crop, bare | random yaw |
     | Under canopy | area | WorldCover tree + crown base from the tree list | random yaw |
     | Tree line | linear | band within a set distance of the tree-mask boundary | along the edge |
     | Near buildings | area | buffer around Overture footprints | random yaw |
     | Road | linear | Overture transportation segments (item 7) | along the road tangent |
     | Parking | area | wherever the data has it; check Overture's schema first (item 7) | random yaw |

   - **Density table.** One row per (class, context): a density in objects per hectare or per km. It lives in a versioned data file whose hash goes in the manifest. Densities for military classes come from the user's lab, not from the agent.
   - **Counts.** For each (class, context), the count is drawn from a Poisson distribution with mean = density × the context's measured area or length in the tile, using the seeded RNG. A context absent from the tile has measure zero and gets zero objects; that is the definition, not a fallback.
   - **Hard constraints, checked for every candidate pose:**
     - sits on terrain, with pitch and roll from a plane fitted under the footprint
     - terrain deviation from that plane under a tolerance (nothing floats or sinks)
     - slope below a per-class limit
     - footprint doesn't overlap buildings or already-placed objects (exact oriented-box tests)
     - not on water (WorldCover class 80)
     - fully inside the tile
     - under canopy: the object's top is below the local crown base
   - **Algorithm.** Sequential rejection sampling, single-threaded, in a fixed order (class, then context, then index). For each object, draw a pose from the context's mask (linear contexts: uniform position along the line, lateral offset within the band, yaw along the tangent plus a small seeded jitter) and test the hard constraints, retrying up to a fixed budget. **An exhausted budget is an error** naming the class and context: it means the density is too high for the available space. It is never a silent skip.
   - **Scene spec.** Placements are written as `{object_id, class, asset_id, x, y, z, yaw, pitch, roll}`. Placement is assembly on the data side; builders only instantiate library meshes at those poses.
   - **Labels.** Exact per-object labels and 3D boxes come from the known poses.
   - **Verification:**
     - zero overlaps by exact test; every object inside the tile and within its slope limit
     - per tile, the number placed equals the number drawn
     - over many seeds on a fixed tile, mean count per (class, context) matches density × measure within sampling error
     - PNG overlays of placements on context masks, for the user's review
   - **Deferred until evidence requires it:** convoys, parking rows, clustering around buildings. The trigger is a measured failure, e.g. a detector trained on independent placements underperforming on clustered real vehicles.
   - **Classes are worth modeling down to motorcycle size** at 14 cm spacing (about 90 returns). At these returns per object, mesh shape errors are visible, which is why validation is mandatory.
9. **Map-click location picker in the viewer.** Needs a basemap tile service; evaluate its dependency and licensing first.
10. **Scan validation against real airborne LiDAR first returns.** Compare voxel crowns against simple opaque spheroid crowns on:
   - first-return height percentiles
   - canopy gap fraction (share of ground points under canopy)
   Multi-return metrics wait for the user's simulator.
11. **Integration with the user's sensor simulator.** It reads the scene geometry contract. This codebase never contains its logic. When it exists, tree stems come back into scope (they need off-nadir rays to be visible).

## 13. References

- **GEDTM30:** Zenodo 18887460; codeberg.org/openlandmap/GEDTM30; Ho et al. 2025, PeerJ, doi:10.7717/peerj.19673
- **Turbid-medium vegetation and the G-function:** Ross 1981, *The Radiation Regime and Architecture of Plant Stands* (G = 0.5 for random leaf orientation)
- **Background on voxel vegetation in LiDAR simulation:** Winiwarter et al., "Virtual laser scanning with HELIOS++," Remote Sensing of Environment (arXiv 2101.09154)
- **Voxel trees:** Weiser et al. 2021, "Opaque voxel-based tree models for virtual laser scanning in forestry applications"
- **Synthetic forest scans:** Schäfer et al. 2023, forest inventory + tree point clouds + HELIOS++
- **LAS 1.4 specification:** ASPRS (point formats, classification codes, extra-bytes VLR)
- **Polyscope:** polyscope.run, including the headless rendering page (polyscope.run/features/headless_rendering)
- **Meta/WRI canopy height:** registry.opendata.aws/dataforgood-fb-forests and registry.opendata.aws/dataforgood-fb-forestsv2
- **Conceptual prior art:** Microsoft Flight Simulator / Blackshark.ai. They do attribute extraction then procedural reconstruction for *visual* fidelity; this project targets *measurement* fidelity.
