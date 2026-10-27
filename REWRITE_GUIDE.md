# Rewrite guide: onion from scratch

This guide comes from a full read of the current code (`src/`, `tests/`, `CMakeLists.txt`) and
the gate records in `NOTES/`, checked against the outputs in `out/`, `boston/` and `cache/`.
It is meant to let you rewrite the generator in your own style without having to rediscover
anything.

It has four parts:

1. **What to carry over unchanged.** Facts, formats and traps that were each paid for with a
   measurement. Losing them means hitting the same bugs again.
2. **Stage-by-stage implementation spec.** The exact algorithm, constants and on-disk layout
   for every stage, so the rewrite can reproduce today's output byte for byte where you want it to.
3. **What not to repeat.** Bugs and style problems in the current code, with evidence.
4. **Build order and verification.** How to rewrite incrementally, using the current binary as
   an oracle.

`SPEC.md`, `FORMAT.md`, `CLAUDE.md` and `NOTES/` stay as they are. This guide adds to them and
does not replace them. Where it says "SPEC x.y" it means that section of `SPEC.md`.

---

## Part 0 — Before you delete anything

1. **Freeze an oracle.** Build the current tree and copy `build/onion` to somewhere outside the
   repo as `onion_ref`. Every later step compares against it.
2. **Produce reference outputs** for three tiles with deliberately different content:

   | tile | lat, lon | why |
   |---|---|---|
   | Hoosier NF | 38.20, −86.40 | closed forest, few buildings, real relief |
   | Fargo | 46.88, −96.79 | flat, sparse street trees, ~800 buildings, one multi-part building |
   | Boston (Beacon Hill) | 42.36, −71.06 | 42.5 m of relief, 1307 buildings, 8 with courtyards |

   Use **one cache directory per tile.** This matters: see bug B1. Record `sha256sum` of every
   file in each cache and each output directory.
3. **Keep these tools.** They are independent oracles, written from the format docs and not from
   the C++:
   - `tools/contract_reader.py` parses the three contract files from `FORMAT.md` alone.
   - `tools/m5_verify.py` checks that every LAS point lies within 1 mm of a contract surface.
   - `tools/g4_crown.py` checks gap fraction and voxel banding.
   - `pdal info --stats` is the independent LAS reader.
4. **Clean the repo first** (part 3, R1): about 1.1 GB of `.git` is committed output.

---

## Part 1 — Carry over unchanged

### 1.1 Data source facts (verified 2026-10-23, see `NOTES/sources.md`)

| source | URL / location | CRS | type | nodata | gotcha |
|---|---|---|---|---|---|
| GEDTM30 v1.2 | `https://s3.opengeohub.org/global/dtm/v1.2/gedtm_rf_m_30m_s_20060101_20151231_go_epsg.4326.3855_v1.2.tif` | 4326 | Float32 | `3.4028235e+38` | **`Scale 0.1` tag is wrong. Values are already metres.** Lat −65.00125 … 85.00125 |
| WorldCover v200 | `https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_<TILE>_Map.tif` | 4326 | Byte (palette) | none | 3°×3° tiles named by their **SW corner**: `%c%02d%c%03d` |
| Canopy v2 | base `https://dataforgood-fb-data.s3.us-east-1.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3/`, index `tiles.geojson` (field `tile`), raster `chm/<tile>.tif` | **3857** | **Byte** (1 m quantised) | none, PER_DATASET mask | 3857 pixel 1.1943 m ≈ 0.81 m on the ground at 47°. The index is 58 MB of GeoJSON, so cache it |
| Overture | `s3://overturemaps-us-west-2/release/2026-10-21.0/theme=buildings/type=building/*` | 4326 | GeoParquet | — | Flat partition (512 files); `bbox` struct pruning only; first query takes minutes |

### 1.2 Traps found during the build. Each one cost a debugging session.

| # | trap | rule |
|---|---|---|
| T1 | Without `GDAL_DISABLE_READDIR_ON_OPEN=EMPTY_DIR`, opening GEDTM30 takes over 300 s instead of 11 s | Set it once in `main` before `GDALAllRegister()`. Any Python or CLI tooling must export it too |
| T2 | GDAL picks a driver from the file extension. A `foo.tif.partial` temp name makes GTiff write **nothing, with no error**, and makes FlatGeobuf write **a directory** | The temp name keeps the extension: `foo.partial.tif`. Also pass `-of GTiff` explicitly |
| T3 | `gdalwarp -te … -tr …` rounds the pixel count to the nearest integer, so a raster can end up 2 m **short** of the padded box | Compute `nx = ceil(width/res − 1e-9)`, extend `xmax = xmin + nx·res`, and pass `-ts nx ny` |
| T4 | Native resolution is not one constant: 4326 shrinks east-west by cos φ, 3857 inflates by 1/cos φ | Measure it. Project the tile centre into the source CRS, step one source pixel in x and in y, project back to UTM, and take the **smaller** of the two distances |
| T5 | Pixel coordinates `(x − origin)/px` are **edge-based**, with domain `[0, w]`. Bounds-checking them as centre-based drops the outer half pixel | Bounds-check edge coordinates; interpolate at `col − 0.5` |
| T6 | Every cached raster is snapped outward to its own pixel grid, so the canopy raster extends past the padded box, and trees were detected up to 0.3 m outside it | Restrict detection to pixel centres inside the padded box |
| T7 | Crown blocks reached past the padded box (−8 … 1132.5 m) | Clip the crown block's x/y index range to the padded box |
| T8 | The Overture bbox filter is an overlap test, so edge buildings come back whole (vertices up to x −56.9 m and 1204.5 m) | Clip each footprint to the padded box with Sutherland–Hodgman |
| T9 | A roof placed at `min terrain + H` is buried on slopes: on Boston, 129 of 1307 buildings were swallowed whole | Base at the **min** and roof at the **max** of terrain under the footprint, sampled at the vertices **and** at every 1 m lattice node inside |
| T10 | Hole bridging duplicates vertices, and an ear test that compares **indices** vetoes every ear | The point-in-ear test compares **coordinates** |
| T11 | The CHM is 1 m quantised, and about 66% of adjacent pixels are equal, so maxima arrive as plateaus | Group 8-connected maxima and keep one tree per plateau, at its centroid |
| T12 | A radial "walk out until height drops by half" crown measure never terminates in closed canopy (190% cover) | Use a marker-controlled watershed |
| T13 | Watershed segments can swallow a canopy patch (23 m radius) | `crown_radius = min(r, 0.35·h)` |
| T14 | Crown voxels at 0.25 m are 6.04 GB per tile | 0.50 m voxels |
| T15 | GeoJSON mandates WGS84; GeoPackage embeds timestamps, so the hash changes on every run | Buildings cache as **FlatGeobuf**, `SPATIAL_INDEX=NO`, query `ORDER BY id` |
| T16 | LAS format 6 puts Classification at byte **16**; byte 15 is the flags byte | — |
| T17 | Polyscope is Y-up, which stands the terrain on its edge | `setUpDir(ZUp)`, `setFrontDir(NegYFront)` |
| T18 | `polyscope::screenshot(std::string)` is ambiguous between overloads | Pass `.c_str()` |
| T19 | Polyscope sphere impostors don't scale to millions of points | `PointRenderMode::Quad` |
| T20 | The viewer blocks in `show()`, so buffered stdout is lost | `fflush(stdout)` after the load summary |
| T21 | Polyscope `FetchContent` can't do shallow submodules (over 30 min to clone) | Vendor it with a shallow clone of tag v2.6.1 into `third_party/` (command in `README.md`) |
| T22 | GDAL's Parquet plugin can't load (Arrow 25/26 ABI mismatch) | DuckDB for Overture, `fetch` only |
| T23 | Wall triangles project to zero xy area | Skip `area == 0.0` exactly. That is why walls get no points; no special case is needed |
| T24 | `std::stoull("-1")` wraps silently | Reject a leading `-` before parsing the seed |
| T25 | Longitude 180 maps to UTM zone 61 | Validate `lon ∈ [−180, 180)` |

### 1.3 Settled parameters

| name | value | where it was settled |
|---|---|---|
| `PAD_M` (padding) | 60.0 m | two GEDTM30 pixels plus bicubic support |
| terrain lattice | 1.0 m | SPEC 6.2 |
| scan spacing | 0.14 m | SPEC 2 |
| `WINDOW_K` | 0.100 | M3 |
| `TREE_H_MIN` | 2.0 m | M3 |
| window radius clamp | 1 … 12 px | M3 |
| WorldCover tree class | 10 | SPEC 7.2 |
| `CROWN_RADIUS_MAX_RATIO` | 0.35 | G4 |
| `CROWN_PAD` | 0.30 m²/m³ | G4 |
| `CROWN_G` | 0.5 | G4 |
| `CROWN_VOXEL_SIZE_M` | 0.50 | M4/G4 |
| `CROWN_BASE_RATIO` | 0.4 | **placeholder** |
| envelope | ellipsoid | **placeholder** |
| `BUILDING_HEIGHT_M` | 6.0 | **open (SPEC 11)** |
| Overture release | `2026-10-21.0` | M2 |
| contract format version | 1 | M4 |

---

## Part 2 — Stage-by-stage implementation spec

### 2.0 Shape of the program

```
onion fetch    --lat --lon --extent --seed --cache
onion generate --lat --lon --extent --seed --cache --out
onion_viewer   [--contract DIR] [--screenshot PNG] [--view top|oblique|closeup|profile]
```

Suggested files. These are namespaces and free functions, with one struct set in one header:

```
src/types.h        every plain struct below; constants
src/cli.cpp        main(): flag parsing, dispatch, the two stage sequences
src/grid.cpp       validate_request, make_grid, crs helpers (load_crs, make_transform)
src/fetch.cpp      fetch_raster, fetch_buildings, the per-source fetchers, provenance
src/cache.cpp      atomic_write (temp + rename), sha256_file, read/write request & provenance JSON
src/raster.cpp     load_raster, sample_nearest, sample_bilinear, sample_bicubic
src/trees.cpp      detect_trees
src/spec.cpp       write_scene_spec, read_scene_spec          ← round-trip it (see S3)
src/terrain.cpp    build_terrain
src/buildings.cpp  clip_to_rect, terrain_range_under, eliminate_holes, ear_clip, build_buildings
src/crowns.cpp     build_crowns
src/contract.cpp   write_/read_ opaque.ply, crowns.bin, scene_geometry.json
src/scan.cpp       rand01, sample_xy, scan_opaque, scan_vegetation
src/las.cpp        las_write (streaming), las_read_decimated
src/viewer.cpp     viewer main, explicit ViewerState passed by reference
tests/check.h      the one shared check()/report() helper
```

Data flow for `generate`, with every arrow a function call in `cmd_generate`:

```
Request ─► Grid
Grid + cache ─► SceneSpec (written to scene_spec.json, then READ BACK)
SceneSpec ─► Mesh (terrain + buildings) + Crowns + ObjectTable
            ─► opaque.ply, crowns.bin, scene_geometry.json   (then READ BACK)
Contract ─► ScanGrid ─► Hits (opaque, then vegetation in place)
Hits ─► points.las ;  Request + provenance ─► manifest.json
```

### 2.1 Types (one canonical set)

Today there are duplicates: `Mesh`/`ContractMesh`, `Crown`/`CrownBlock`, and two spacing
constants. Keep one of each:

```cpp
struct Request  { double lat, lon, extent_m; uint64_t seed; };
struct Bbox     { double xmin, ymin, xmax, ymax; };
struct Grid     { int utm_epsg; Bbox tile_m, padded_m; double origin_x, origin_y; Bbox lonlat; };
struct Raster   { int w, h; double origin_x, origin_y, px, py; std::string path; std::vector<float> v; };
struct Tree     { double x, y, ground_z, height_m, crown_radius_m; };
struct Footprint{ std::string id; double height_m; std::vector<std::vector<double>> rings; }; // rings[0] exterior, xy pairs, not closed
struct Provenance { std::string name, url, version, vintage, file, sha256; };
struct SceneSpec{ Request request; Grid grid; std::string terrain_path, landcover_path, chm_path;
                  std::vector<Footprint> buildings; std::vector<Tree> trees; std::vector<Provenance> provenance; };
struct Mesh     { std::vector<float> xyz; std::vector<uint32_t> indices, object_id; std::vector<uint8_t> cls; };
struct Crown    { uint32_t object_id; int32_t ix, iy, iz; uint32_t nx, ny, nz; std::vector<float> pad; };
struct ObjectRow{ uint32_t object_id; uint8_t cls; std::string source; };
struct Archetype{ double pad, g, voxel_size_m, crown_base_ratio; };  // read from the contract by the scanner
struct ContractMeta { int format_version, epsg; double origin_x, origin_y; Bbox padded_local; Archetype arch; std::vector<ObjectRow> objects; };
struct ScanGrid { double x0, y0, spacing; int nx, ny; uint64_t seed; };
struct Hits     { std::vector<float> z; std::vector<uint32_t> object_id; std::vector<uint8_t> cls; size_t hit_count; };
```

Vertex and face counts are `xyz.size()/3` and `indices.size()/3`. Make them free functions
(`vertex_count(const Mesh&)`), not members as today.

### 2.2 Request validation (SPEC 6.1)

Every violation throws, and the message names the field and the full request:
- `lat ∈ [−65.00125, 85.00125]` (GEDTM30, the only verified global bound). Write it as
  `!(lo <= lat && lat <= hi)` so NaN fails.
- `lon ∈ [−180, 180)`.
- `extent_m > 0`.
- Flags: `--key value` pairs, each given once, all required, none unknown. Numbers must parse
  completely (`stod` consumed length == size). The seed must have no leading `-`.

### 2.3 Grid (SPEC 6.2)

```
zone     = floor((lon + 180) / 6) + 1
epsg     = (lat >= 0 ? 32600 : 32700) + zone
(cx, cy) = transform(4326 → epsg, lon, lat)        // OAMS_TRADITIONAL_GIS_ORDER on BOTH srs
tile_m   = [cx − e/2, cy − e/2, cx + e/2, cy + e/2]
padded_m = tile_m grown by 60 m on every side
origin   = (padded_m.xmin, padded_m.ymin)          // no snapping
lonlat   = min/max over ALL FOUR padded corners transformed back to 4326
```

Use all four corners because meridian convergence tilts the box. Acceptance: Fargo maps to
32614 and Sydney (−33.87, 151.21) to 32756; the round trip at the centre and the padded corners
is under 1 m.

### 2.4 Fetch (SPEC 6.3)

For each source: if the final file exists, report it as cached. Otherwise write to
`partial_path(final)` (T2), rename on success, and unlink on any failure. Hash the final file
with SHA-256 (hand-rolled, FIPS 180-4, ~100 lines; no new dependency). **Fix B1 here** (part 3).

**`fetch_raster(url, resampling, out, grid)`**
1. `GDALOpen("/vsicurl/" + url)`. Throw with `CPLGetLastErrorMsg()` if it fails.
2. Require a geotransform with `gt[2] == gt[4] == 0`, and a CRS.
3. `res = native_ground_res_m(...)` (T4).
4. `nx = ceil((pxmax − pxmin)/res − 1e-9)`, likewise `ny`; `xmax = pxmin + nx·res`,
   `ymax = pymin + ny·res` (T3).
5. `GDALWarp` with `-of GTiff -t_srs EPSG:<utm> -te pxmin pymin xmax ymax -ts nx ny -r <resampling>`.
   Format numbers with `%.17g`.

Per source:

| name | file | resampling | version string | vintage string |
|---|---|---|---|---|
| terrain | `terrain.tif` | bilinear | `GEDTM30 v1.2` | `2006-01-01/2015-12-31 (encoded in the file name)` |
| landcover | `landcover.tif` | **near** | `ESA WorldCover v200 (2021), tile <T>` | `2021-01-01/2021-12-31` |
| chm | `chm.tif` | bilinear | `Meta/WRI canopy height v2, tile <T>` | `varies by location; not published per tile for v2` |
| buildings | `buildings.fgb` | — | `Overture release 2026-10-21.0` | `2026-10-21.0` |

- WorldCover tile: compute the name for the SW and the NE corner of `lonlat`. If they differ,
  throw, because mosaicking isn't implemented.
- Canopy tile: cache `tiles.geojson` as `chm_tiles.geojson`, then apply an OGR spatial filter
  with `lonlat`. Exactly one feature must match; otherwise throw and list the matches.
- Buildings:
  1. DuckDB: `INSTALL spatial; LOAD spatial; INSTALL httpfs; LOAD httpfs; SET s3_region='us-west-2';`
     then `SELECT id, ST_AsText(geometry) … WHERE bbox.xmin < E AND bbox.xmax > W AND bbox.ymin < N AND bbox.ymax > S ORDER BY id`.
  2. Parse each row's WKT with an SRS in traditional order, `transformTo(utm)`, and
     `forceToMultiPolygon`.
  3. Write FlatGeobuf with layer `buildings`, the UTM SRS, `wkbMultiPolygon`, `SPATIAL_INDEX=NO`,
     and one string field `id`.
  4. Close the dataset **before** hashing.
- Write `request.json` (`lat, lon, extent_m, seed`) and `provenance.json`
  (`{name: {url, version, vintage, file, sha256}}`) with CPLJSON.

### 2.5 Raster loading and sampling (SPEC 8.2)

- `load_raster`: read band 1 as Float32. **No scale.** If NoData is declared, cast it to float
  (gotcha 1) and throw if any pixel equals it. Store the north-west origin, `px > 0` and `py < 0`.
- `to_pixel`: `col = (x − ox)/px`, `row = (y − oy)/py`. Throw unless both are within
  `[−1e-6, w + 1e-6]` (T5), and name the raster in the message.
- `sample_nearest`: floor, then clamp to `[0, w−1]`.
- `sample_bilinear`: `cc = clamp(col − 0.5, 0, w−1)`, `c0 = min(floor(cc), max(w−2, 0))`,
  `c1 = min(c0+1, w−1)`, likewise for rows, then the standard 4-tap formula in double.
- `sample_bicubic` (terrain only): Catmull-Rom with **clamped** indices, in exactly this form
  (needed for byte identity):
  ```
  cubic(p0,p1,p2,p3,t) = p1 + 0.5*t*(p2 - p0 + t*(2*p0 - 5*p1 + 4*p2 - p3 + t*(3*(p1 - p2) + p3 - p0)))
  c = (x-ox)/px - 0.5 ; r = (y-oy)/py - 0.5 ; c0=floor(c), r0=floor(r), fx=c-c0, fy=r-r0
  col[j] = cubic(at(r0-1+j, c0-1..c0+2), fx)  for j=0..3 ;  z = cubic(col[0..3], fy)
  ```
  **Add the same bounds check `to_pixel` does.** Today it has none, which is half of bug B1.

### 2.6 Tree detection (M3, `NOTES/m3/README.md`)

Inputs are the chm, landcover and terrain rasters and the padded box. All arrays are
row-major on the chm grid.

1. **Mask:** for each pixel with centre `(x, y)`,
   `mask = inside(padded) && lround(sample_nearest(lc, x, y)) == 10 && chm >= 2.0`.
2. **Window radius per pixel:** `want = clamp(lround(0.1·chm/px), 1, 12)`. Collect the set of
   radii used by masked pixels.
3. **Local maxima:** for each radius `r` in that set, compute a separable square max filter
   (out-of-raster pixels ignored, initial value −1e30). A pixel is a maximum if it is masked,
   `want == r`, and `chm >= filtered`. Ties count as maxima.
4. **Plateaus:** scan row-major. For each unlabelled maximum, flood-fill 8-connected maxima
   with a stack (LIFO). Label `1..N` and record the mean col, mean row and mean height.
5. **Watershed:** take the masked, unlabelled pixels and `stable_sort` them by height,
   descending. Each pixel takes the label of its tallest labelled 8-neighbour (strict `>`, so
   the first in NBR8 order wins ties). Labels are updated **in place** during a pass. Repeat
   passes on the leftovers until nothing changes (today the cap is 64 passes; see B6).
   `NBR8 = {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}` as (drow, dcol).
6. **Trees:**
   - `x = ox + (mean_col + 0.5)·px`, `y = oy + (mean_row + 0.5)·py`
   - `h = mean height`
   - `ground_z = sample_bilinear(terrain, x, y)`
   - `r = max(sqrt(max(area, 1)·px²/π), px)`, then `r = min(r, 0.35·h)`, where `area` counts
     every pixel carrying the label, plateau included.

Expected: Hoosier 10,600 trees (84.8/ha); Fargo 1,589.

### 2.7 Scene spec (SPEC 6.4, `scene_spec.json`)

```json
{ "grid": {"utm_epsg":…, "padded_bbox_m":[xmin,ymin,xmax,ymax], "local_origin":[x,y]},
  "rasters": {"terrain":path, "landcover":path, "chm":path},
  "buildings": [{"id":…, "polygon_utm":[[[x,y],…] /*exterior*/, [[x,y],…] /*holes*/], "height_m":6.0}],
  "trees": [{"x","y","ground_z","height_m","crown_radius_m"}],
  "provenance": <provenance.json verbatim>,
  "request": {"lat","lon","extent_m","seed"} }
```

- There is one `buildings` entry per **polygon part**, and parts share the Overture `id`.
- The ring's closing vertex is dropped.
- `height_m` is the constant; Overture's own height is ignored on purpose (SPEC 6.7).
- **Read it back** before building (S3).

### 2.8 Terrain builder (SPEC 6.5)

- `nx = llround(padded_width / 1.0)`, likewise `ny`. Vertices are laid out row-major, `j`
  outer, from `0..ny` × `0..nx`.
- Vertex values: `x = float(i·1.0)`, `y = float(j·1.0)` (computed as `float(world − origin)`),
  `z = float(bicubic(world_x, world_y))`.
- Per cell: `(i,j)(i+1,j)(i+1,j+1)` and `(i,j)(i+1,j+1)(i,j+1)`. Object id 0, class 2, object
  row `{0, 2, "gedtm30"}`.
- Check: exactly `2·nx·ny` faces (2,508,800 on a 1120 m padded tile).

### 2.9 Building builder (SPEC 6.7)

Object ids start at 1. For each building:

1. Clip the exterior ring to `padded_m` with Sutherland–Hodgman (half-planes `x ≥ xmin`,
   `x ≤ xmax`, `y ≥ ymin`, `y ≤ ymax`, in that order; inside test `≥ 0`). If fewer than 3
   vertices remain, skip the building.
2. Keep the holes only if the exterior was **not** clipped. Test this by coordinate equality,
   **not vertex count** (see B5).
3. `lo`/`hi` = min/max over every ring of `terrain_range_under(ring)`, which covers:
   - bicubic samples at every vertex, plus
   - bicubic samples at every 1 m lattice node `origin + k·1.0` inside the ring's bbox that
     passes the even-odd test against **that ring**.
4. `zb = float(lo)`, `zt = float(hi + height_m)`.
5. Walls: for each ring and each vertex, add a bottom and a top vertex. Each edge `i→j` becomes
   two faces, `bot_i bot_j top_j` and `bot_i top_j top_i`.
6. Roof: if there are holes, eliminate them:
   - force the exterior CCW (shoelace area2 > 0) and the holes CW;
   - process holes in descending order of max x;
   - from each hole's rightmost vertex, cast a ray in +x and bridge to the farther-right
     endpoint of the nearest crossed exterior edge.
7. Ear-clip the result (T10): force CCW and take the first convex ear with no other vertex
   inside or on it. Roof vertices are separate from wall vertices.
8. Class 6 and one object row `{oid, 6, "overture:" + id}` **per building**, not per part (B4).

Check: on Fargo, 12,832 roof triangles, matching `Σ(n−2)`. `tests/test_builders.cpp`
(square, one courtyard, two courtyards, an L shape with a courtyard, and a 20% slope) carries
over as-is.

### 2.10 Crown builder (SPEC 6.6)

Object ids continue after the buildings. For each tree, with `s = 0.5`:

```
top = gz + h ; base = gz + 0.4h ; cz = (top+base)/2 ; rz = (top-base)/2 ; rx = crown_radius
skip unless rz > 0 && rx > 0
lx = x − ox ; ly = y − oy
i_hi = ceil(padded_w / s) − 1 ; j_hi likewise
i0 = max(floor((lx−rx)/s), 0) ; i1 = min(floor((lx+rx)/s), i_hi)   (same for j)
iz = floor((cz−rz)/s) ; nz = floor((cz+rz)/s) − iz + 1               ← z is ABSOLUTE
voxel (kx,ky,kz) has PAD 0.30 if ((vx²+vy²)/rx² + vz²/rz²) <= 1, where v = centre − (lx,ly,cz)
```

Use the order x fastest, then y, then z. Object row `{oid, 5, "chm_crown"}`.

### 2.11 Contract files (SPEC 6.8)

`FORMAT.md` is authoritative and byte-exact. The three files:
- `opaque.ply`: ASCII header verbatim, then 12-byte vertices and 18-byte faces.
- `crowns.bin`: `ONIONCRW`, u32 version, u32 count, then per record a 28-byte header and PAD
  floats.
- `scene_geometry.json`: `format_version, epsg, local_origin, padded_bbox_local [0, 0, w, h],
  crown_archetype {pad, g, voxel_size_m, crown_base_ratio, envelope:"ellipsoid"}, objects[]`.

Write one shared crowns.bin header reader. Today it exists twice (S2).

### 2.12 Scanner (SPEC 6.9)

**RNG.** Reproduce this exactly if you want byte-identical LAS output:
```
splitmix64(x): x += 0x9E3779B97F4A7C15; x = (x^(x>>30))*0xBF58476D1CE4E5B9; x = (x^(x>>27))*0x94D049BB133111EB; return x^(x>>31)
rand01(seed, sample, draw) = (splitmix64(splitmix64(seed ^ (sample * 0x2545F4914F6CDD1D)) ^ draw) >> 11) * 2^-53
sample = j*nx + i
```

**Grid.** `x0 = tile.xmin − ox` (= 60), `y0` likewise, and `nx = ny = floor(extent/0.14)`
(7142 for 1000 m). A sample's position is `x0 + (i + rand01(s, 0))·0.14` and
`y0 + (j + rand01(s, 1))·0.14`.

**Opaque pass.** Loop over faces in file order, with vertices in double.
- `area = (bx−ax)(cy−ay) − (by−ay)(cx−ax)`; skip the face if it is exactly 0.
- Cell range: `floor((lo − x0)/sp) − 1 … floor((hi − x0)/sp) + 1`, clamped to the grid.
- Edge weights:
  - `w0 = ((bx−ax)(sy−ay) − (by−ay)(sx−ax))/area`
  - `w1 = ((cx−bx)(sy−by) − (cy−by)(sx−bx))/area`
  - `w2 = ((ax−cx)(sy−cy) − (ay−cy)(sx−cx))/area`
- Reject the sample if any weight is `< 0`.
- `z = w1·az + w2·bz + w0·cz`, rounded to float.
- Keep the hit if `z > best || (z == best && oid < best_oid)`.

**Vegetation pass.**
- For each sample, sum PAD per absolute `k` over every crown whose block contains voxel
  `(floor(sx/s), floor(sy/s))`, keeping the **min** object id per voxel.
- Walk `k` from the top down. Stop as soon as `bot < opaque_z` (see B7).
- Interception: `p = 1 − exp(−G·PAD·s)`, hit if `rand01(s, d_hit) < p`; then
  `depth = −log(1 − u·p)/(G·PAD)` with `u = rand01(s, d_depth)`, `z = top − depth`, class 5.
- Draw indices today: `d_hit = 2 + 2·step` and `d_depth = 3 + 2·step`, where `step` counts from
  the **global** top `kz_hi` (see B8).

Acceptance (`tests/test_scanner.cpp`, `tests/test_crown.cpp`):
- a flat plane gives exactly the cell count and density `1/0.14²`;
- a box gives no wall points;
- ties go to the smaller id;
- four PAD values each match Beer–Lambert within 4 s.e.;
- the within-voxel phase is flat.

### 2.13 LAS 1.4 writer (SPEC 6.10)

Header: 375 bytes, little-endian.

| off | type | value |
|---|---|---|
| 0 | char[4] | `LASF` |
| 4 | u16 | 0 (file source id) |
| 6 | u16 | **16** (WKT bit) |
| 8 | 16 B | 0 GUID |
| 24 / 25 | u8 / u8 | 1 / 4 |
| 26 | char[32] | `onion` |
| 58 | char[32] | `onion generate` |
| 90 / 92 | u16 / u16 | day 1 / year 2026 (fixed, for reproducibility) |
| 94 | u16 | 375 |
| 96 | u32 | 375 + VLR bytes |
| 100 | u32 | 2 VLRs |
| 104 | u8 | 6 |
| 105 | u16 | 34 |
| 107 | u32 + 5×u32 | 0 (legacy counts) |
| 131 | 3×f64 | scale 0.001 |
| 155 | 3×f64 | offset `ox, oy, 0` |
| 179 | 6×f64 | maxX minX maxY minY maxZ minZ |
| 227 / 235 / 243 | u64 / u64 / u32 | 0 / 0 / 0 |
| 247 | u64 | point count |
| 255 | 15×u64 | by return: `[count, 0 × 14]` |

VLR header (54 B): u16 0, char[16] user id, u16 record id, u16 payload length, char[32]
description.
- VLR 1: `LASF_Projection` / 2112. Payload is the WKT from `OGRSpatialReference::exportToWkt`
  of the UTM EPSG, plus a NUL.
- VLR 2: `LASF_Spec` / 4, a 192 B extra-bytes descriptor with:
  - bytes 0–1 reserved;
  - byte 2 = 5 (uint32);
  - byte 3 options = 0;
  - name `object_id` in 32 bytes;
  - 4 unused bytes, then 5×24 B of zeros;
  - description `scene geometry object id` in 32 bytes.

Point (34 B):

| off | type | field |
|---|---|---|
| 0 / 4 / 8 | i32 | `round((world − offset)/0.001)` (z offset 0); throw on int32 overflow |
| 12 | u16 | 0 |
| 14 | u8 | `0x11` |
| 15 | u8 | 0 |
| 16 | u8 | class |
| 17 | u8 | 0 |
| 18 | i16 | 0 |
| 20 | u16 | 0 |
| 22 | f64 | 0 |
| 30 | u32 | object_id |

- Bounds are computed in a first pass over the hits, because the header needs them before
  streaming.
- Stream through a 1 MiB buffer.
- Assert `written == declared` at the end.
- **Check the return value of `fclose`.**

### 2.14 Manifest

`manifest.json`: `request`, `utm_epsg`, `overture_release`, `contract_format_version`,
`git_commit`, `point_count`, and `provenance` (verbatim). Fix the git commit capture (B9).

### 2.15 Viewer (SPEC 6.11)

- Reads `scene_geometry.json`, `opaque.ply` and `crowns.bin` (non-empty voxel centres,
  decimated to 2 M by a two-pass stride), plus `points.las` if present (decimated to 3 M by
  stride).
- Splits the mesh by class into `terrain` and `buildings` surface meshes, coloured
  (0.72,0.70,0.66) and (0.58,0.56,0.60).
- Crowns: quad points of radius `s/2` with a scalar `PAD` quantity. They start hidden when a
  LAS is loaded.
- Points: radius 0.07 and class colours: 2 = (0.85,0.55,0.18), 5 = (0.13,0.80,0.22),
  6 = (0.95,0.20,0.25).
- Print the decimation factors on load and in the panel.
- Cameras, all derived from the contract bbox (`size` = bbox max side, `cz` = mid z,
  `gz` = z of the vertex nearest the tile centre):

  | view | eye | target |
  |---|---|---|
  | `top` | `(cx, cy − 0.02·size, cz + 1.35·size)` | `(cx, cy, cz)` |
  | `oblique` | `(cx − 0.75·size, cy − 0.75·size, cz + 0.55·size)` | `(cx, cy, cz)` |
  | `closeup` | `(cx − 70, cy − 70, gz + 58)` | `(cx, cy, gz + 8)` |
  | `profile` | `(cx − 165, cy − 18, gz + 26)` | `(cx + 60, cy, gz + 14)` |

- Headless mode: `allowHeadlessBackends = true` when `--screenshot` is given.
- Run button: start `fetch` then `generate` as subprocesses on a worker thread. Fix B2 and B3.

---

## Part 3 — What not to repeat

### 3.1 Bugs (correctness), most severe first

**B1. A stale cache is silently reused, and provenance is then falsified.** *(verified)*

`fetch` decides "cached" by file existence alone and never compares the request against the
cache's `request.json`. `generate` never checks either. The live `cache/` directory shows the
damage:
- `request.json` says Boston (42.36, −71.06), and `provenance.json` names Boston's WorldCover
  tile `N42W072`.
- The rasters and `buildings.fgb` (7 buildings, dated Oct 23 15:58) are **Hoosier's**, hashed
  under Boston URLs.

Running `generate --lat 46.88 --lon -96.79` (Fargo) against that cache **exited 0**:
- 0 trees, 0 buildings;
- the terrain was a constant 158.472 m across every vertex, because `sample_bicubic` clamps
  instead of bounds-checking.

The viewer makes this easy to hit, since it defaults to a single `/tmp/onion_cache` for every
request.

**Fix:**
- `fetch` throws if `request.json` exists and differs from the request (or key the cache
  directory by request).
- `generate` reads `request.json` and throws on mismatch.
- `sample_bicubic` bounds-checks.

**B2. The viewer's Run sends a different request than the CLI would.** *(verified)* The panel
stores lat/lon as `float` and formats them with `%.6f`, so the default 38.20 / −86.40 goes out
as `--lat 38.200001 --lon -86.400002`. A GUI run is not the same tile as `onion … --lat 38.20`,
which defeats SPEC 6.11 rule 2. The seed is a signed `int` (the CLI takes uint64), and the
extent is a float.

**Fix:** keep the fields as text, or as `double` printed with `%.17g`, and pass the user's
string through unchanged.

**B3. The viewer's Run never reloads the scene.**
- `g_reload` is set at `viewer.cpp:136` and never read anywhere, so after a run the window keeps
  showing the old scene.
- `g_contract_dir` is written from the detached thread without the mutex, which is a data race.
- `run_cli` ignores the exit status, so `generate` runs even after `fetch` failed.
- `cli_path()` silently falls back to `"onion"` on `$PATH`.

**Fix:**
- Check `pclose`'s status and stop the chain on failure.
- Hand the result back through the mutex-protected state.
- Reload in the UI callback.
- Throw if `/proc/self/exe` can't be read.

**B4. Multi-part buildings become several objects.** The scene spec emits one entry per polygon
part, sharing the Overture id, and its comment says this keeps them "one object". But
`build_buildings` assigns a new object id and object row **per entry** (`builders.cpp:352`).
SPEC 6.7 says one object ID per building. This doesn't happen in the stored Boston and Hoosier
outputs, but Fargo has one two-part building. **Fix:** group parts by id, or make `Footprint`
hold a list of polygons.

**B5. "Was the exterior clipped?" is tested by vertex count** (`builders.cpp:300`). A
rectangle straddling one edge loses 2 vertices and gains 2 intersection points: it is clipped,
but the count is unchanged. Its holes are then kept and may lie partly outside the padded box,
breaking the contract's bounds rule. **Fix:** compare coordinates, or clip the holes too.

**B6. Silent partial results in geometry code.** Each of these violates the fail-fast rule and
should throw instead:
- `eliminate_holes` drops a hole with no ray crossing (`continue`) and skips holes with fewer
  than 3 vertices.
- `triangulate` stops after 10,000 iterations or when no ear is found, and emits a partial
  roof.
- The watershed stops after 64 passes and leaves pixels unlabelled.

**B7. The voxel containing the ground is ignored.** `scan.cpp:155` breaks out of the walk when
`bot < opaque_z`, so the voxel straddling the surface contributes nothing, even when it is
mostly above ground. SPEC says to walk "down to the opaque z". **This is a decision for you:**
- truncate the path length to `top − opaque_z` (a correct turbid medium, but it changes the
  output), or
- keep the current behaviour and document it in `FORMAT.md`/SPEC.

**B8. Vegetation draw indices depend on the whole tile.** `step` counts down from the global
`kz_hi`, the highest voxel of *any* crown. Adding one tall tree anywhere shifts the draws of
every sample under canopy. The output is still deterministic, but a local change has global
effects, which is bad for ablations. **Fix:** use the absolute lattice index,
`d_hit = 2 + 2·(k − K_MIN)` with a fixed `K_MIN`, or hash `(seed, sample, k, purpose)`. This is
an output change, so decide it once.

**B9. The manifest's git commit is stale.** `execute_process(git rev-parse HEAD)` runs at
**CMake configure time** only. Evidence:
- `boston/manifest.json` records `1408978` (Oct 23), but it was produced on Oct 27 by a binary
  that includes `0e3777a`'s courtyard code.
- `out/manifest.json` records `05e7913`, which is older than the code that wrote it.
- Dirty trees are never flagged.

**Fix:** generate a header at **build** time (`add_custom_target` running `git describe --always --dirty`).

**B10. The contract reader defaults silently.**
- `read_scene_geometry` uses `GetDouble("g", 0)` and `GetDouble("pad", 0)`.
- `scan_vegetation` then returns early when `g ≤ 0` (`scan.cpp:87`).

So a contract missing `g` produces a scan with **no vegetation and no error**. The same goes
for `GetInteger("epsg", 0)`. **Fix:** a required-field getter that throws, and `scan_vegetation`
throws on non-positive parameters.

### 3.2 Structure and style (against `CLAUDE.md`)

**S1. Member functions and OOP bits.**
- `Mesh::vertex_count/face_count` are members (`contract.h:40`).
- `Buf` in `las.cpp` has methods and a member template.

Write free functions over plain data.

**S2. Duplicate types and constants.**
- `Mesh` ≡ `ContractMesh`.
- `Crown` ≡ `CrownBlock`.
- `SAMPLE_SPACING_M` (`grid.h`, unused) and `SCAN_SPACING_M` (`scanner.h`).
- crowns.bin header parsing is written twice (`read_crowns`, `read_crown_points`).

Keep one of each.

**S3. The scene spec is written but never read.** SPEC 4.4 makes it the builders' only input,
yet `generate` hands the builders the in-memory struct ("in spirit", `scene_spec.h:19`). The
contract, by contrast, is read back to prove the round trip. Do the same here:
`write_scene_spec` then `read_scene_spec`. That also untangles the include cycle in which
`scene_spec.h` includes `builders.h` for `Footprint` and defines `BUILDING_HEIGHT_M` above its
includes.

**S4. Real repetition that the abstraction rule allows you to extract** (each exists 2+ times
today):

| helper | copies | where |
|---|---|---|
| `TransformPtr` + deleter | 3 | `grid`, `fetch_raster`, `test_grid` |
| `load_crs(epsg)` with traditional order | 4 | `grid`, `fetch_raster`, `buildings`, `main` |
| `num()` `%.17g` | 2 | `fetch_raster`, `buildings` |
| `FilePtr` | 3 | `contract_write`, `contract_read`, `las_read` |
| temp + rename + unlink | 3 | `fetch` ×2, `cache` |
| request → JSON | 3 | `cache`, `scene_spec`, `manifest` |
| load `provenance.json` | 2 | `scene_spec`, `manifest` |
| drop the ring's closing vertex | 2 | `scene_spec` |
| test `check()`/`fmt()` | 5 | every test |

**S5. `cmd_generate` contains pipeline logic.** LAS bounds, WKT export and the point loop
(`main.cpp:199-235`) belong in `las.cpp`, as `las_write(path, grid, scan_grid, hits)`. The entry
point should read as a list of stage calls.

**S6. Small items.**
- `scan_opaque(mesh, mesh.object_id, g)` passes a field of its own argument.
- The literal `5` is used in the scanner and in `main` instead of `CLASS_HIGH_VEGETATION`.
- `M_PI` is non-standard; use `std::numbers::pi`.
- Dead code: `in_face` / `(void)in_face`, and `ContractMeta::crown_pad` / `crown_base_ratio`,
  which are read and never used.
- `test_sources.cpp` includes `<cstdio>` twice.
- The viewer keeps its state in globals (`g_run`, `g_meta`, `g_scene_mid_z`, …). Pass one
  `ViewerState&` into the callback through a lambda capture.

**S7. Performance.** `scan_vegetation` builds `std::vector<std::vector<uint32_t>>` with one entry
per **sample**: 51 M vectors × 24 B ≈ 1.2 GB of empty headers before any crown is indexed. Index
per **voxel column** instead: 2240² ≈ 5 M columns for a 1120 m tile at 0.5 m. Use a CSR layout
(offsets plus a flat list). The output doesn't change, because the per-sample candidate set is
still filtered by its voxel.

### 3.3 Repository hygiene

**R1. Data is committed.** `out/`, `boston/` and `cache/` (two 1.7 GB LAS files, a 687 MB
`crowns.bin`, and a 58 MB GeoJSON) are tracked, and `.git` is 1.1 GB. Add them to
`.gitignore` and remove them from the index. Purging history is a separate, destructive step,
so decide it deliberately.

**R2. `imgui.ini` is tracked** and modified on every viewer run. Ignore it, and set
`polyscope::options::imguiConfigPath` or disable the ini.

**R3. Stale docs to fix as you go.**
- `README.md` still says "Building footprints with interior rings throw" (courtyards are now
  extruded).
- `NOTES/m3/README.md` says the same.
- The `FORMAT.md` example shows `"pad": 0.5`; the settled value is 0.30.

---

## Part 4 — Build order and verification

Each step ends with a check against `onion_ref` or an oracle. "Byte-identical" is achievable for
every stage whose formulas you copy from part 2 in the same operation order. Where you
intentionally fix something (B4, B7, B8), expect a diff and verify it with the oracles instead.

| step | build | verify |
|---|---|---|
| 1 | CMake (GDAL, DuckDB, per-target warnings, build-time git hash), `types.h`, CLI parsing, validation, grid | `test_grid` checks carried over; invalid requests exit 1 with a clear message |
| 2 | cache helpers, SHA-256, `fetch_raster`, the 4 fetchers, request guard (B1) | the cache is **byte-identical** to the reference caches; a second run under `unshare -rn` succeeds; a mismatched request throws |
| 3 | raster load/sample, tree detection | tree count and statistics match M3 (10,600 Hoosier / 1,589 Fargo); the `trees` array in `scene_spec.json` is identical |
| 4 | scene spec write **and read** | `scene_spec.json` is byte-identical (CPLJSON output is stable) |
| 5 | terrain, buildings (with B4/B5/B6 fixed), crowns | `contract_reader.py` passes; `test_builders` passes; `opaque.ply` and `crowns.bin` are identical on Hoosier and Boston, and any Fargo diff is explained by B4/B5 |
| 6 | contract writers and readers (required-field getters, B10) | round trip; `scene_geometry.json` is identical |
| 7 | scanner, opaque pass | `test_scanner`; `m5_verify.py` worst error < 1 mm |
| 8 | scanner, vegetation pass (column index, S7) | `test_crown`; Hoosier gap fraction 19.6%; with B7/B8 unchanged, `points.las` is identical |
| 9 | LAS writer and manifest | `pdal info` reads format 6, the CRS and `object_id`; two runs hash the same |
| 10 | viewer (B2, B3, no globals) | `--screenshot` headless works for all four views; Run then reload shows the new tile; the GUI request strings equal what you typed |
| 11 | decide B7 and B8 | if changed: bump nothing in the contract (the scanner isn't the contract), rerun `g4_crown.py`, and record it in `NOTES/` |

### Decisions you should make up front

1. **B7** (the ground-straddling voxel) and **B8** (draw indexing): both change `points.las`.
   Decide them before step 8 so you only re-baseline once.
2. **Cache identity (B1):** throw on a mismatch, or key cache subdirectories by request.
   Throwing is simpler and matches "no fallbacks".
3. **Building height constant**: still open in SPEC 11.
4. **Purge history (R1)**, or only stop tracking data from now on.
