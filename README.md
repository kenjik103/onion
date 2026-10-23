# onion — real-world airborne LiDAR scene generator

Given a latitude, longitude and extent, builds a labelled airborne LiDAR point cloud of
that real location from global open geodata. See `SPEC.md` for the design and the
reasoning behind it, and `FORMAT.md` for the scene geometry contract.

The scanner in this repository is a deliberately simple nadir raycaster, a stand-in for a
proprietary sensor simulator that will read the same contract files.

---

## Build

Needs **GDAL**, **DuckDB** and a C++20 compiler. Polyscope is vendored and must be cloned
first — it is not fetched by CMake, because `FetchContent` cannot make submodule clones
shallow and the full histories take the better part of an hour:

```sh
git clone --depth 1 --branch v2.6.1 --recurse-submodules --shallow-submodules \
    https://github.com/nmwsharp/polyscope.git third_party/polyscope

cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

Produces `build/onion` (the CLI) and `build/onion_viewer`.

---

## Running

The pipeline is split in two because production compute nodes have no network.
**`fetch` needs the network; `generate` never opens a connection.** The cache directory
is the only thing that has to move between machines.

### 1. Fetch — on a networked machine

```sh
build/onion fetch --lat 38.20 --lon -86.40 --extent 1000 --seed 1 --cache ./cache
```

Pulls four sources for the tile and writes them to `./cache`, creating it if absent:

| file | source |
|---|---|
| `terrain.tif` | GEDTM30 v1.2 bare-earth DTM |
| `landcover.tif` | ESA WorldCover v200 (2021) |
| `chm.tif` | Meta/WRI canopy height v2 |
| `chm_tiles.geojson` | canopy tile index — a lookup aid, not a source; cached so a second run stays offline |
| `buildings.fgb` | Overture buildings, reprojected to the tile's UTM zone |
| `provenance.json` | per source: URL, version, vintage, SHA-256 |
| `request.json` | the tile request |

A second `fetch` against the same cache makes **no network requests**. First run takes a
few minutes, almost all of it the Overture query, which must read the footer of 512
parquet files.

### 2. Generate — offline

```sh
build/onion generate --lat 38.20 --lon -86.40 --extent 1000 --seed 1 \
    --cache ./cache --out ./out
```

A few seconds to about 20 s for a 1 km tile, depending on how much canopy there is
to trace through — measured 5 s on a half-open tile and 19 s on closed forest.
Creates `./out` if absent and writes:

| file | contents |
|---|---|
| `scene_spec.json` | the tile's canonical description: grid, raster paths, buildings, tree list, provenance |
| `scene_geometry.json` | contract metadata, object table, crown archetype |
| `opaque.ply` | every opaque surface as triangles, with per-face object id and class |
| `crowns.bin` | vegetation as plant-area-density voxels |
| `points.las` | the scan: LAS 1.4, point format 6, ~51 M points, ~1.6 GiB |
| `manifest.json` | request, seed, Overture release, provenance, contract version, git commit |

The three contract files are documented byte by byte in **`FORMAT.md`**. They are the
only scene input the scanner and viewer read, and the interface any other sensor
simulator should be built against.

**Reproducibility:** the same request, seed and cache regenerate byte-identical output.

### 3. Viewer

```sh
build/onion_viewer --contract ./out
```

Four toggleable layers — terrain, buildings, crowns, point cloud — with fields for
latitude, longitude, extent and seed plus a **Run** button that executes `fetch` then
`generate` as subprocesses, so a GUI run and a CLI run produce identical bytes.

Headless screenshots, through EGL, with no display (Linux only):

```sh
build/onion_viewer --contract ./out --screenshot shot.png --view closeup
```

Views: `top` (near-nadir), `oblique` (whole tile), `closeup` (points on surfaces),
`profile` (near-horizontal, shows canopy above ground).

The point cloud and crowns are **decimated for display only**. The factor is printed on
load and shown in the UI; nothing is written back.

---

## Classes and object ids

ASPRS codes throughout: **2** ground, **5** high vegetation, **6** building. Every
primitive and every point also carries a `uint32` object id — the terrain is 0, then one
per building, then one per crown. `scene_geometry.json` holds the table.

---

## Checking the output

```sh
pdal info --stats out/points.las              # independent LAS reader
python3 tools/contract_reader.py out          # parses the contract from FORMAT.md alone
python3 tools/m5_verify.py out 400            # points lie within 1 mm of a contract surface
python3 tools/g4_crown.py out /tmp/g4         # gap fraction, voxel banding, cross-section
```

`tools/` also holds the gate tooling used to settle the design decisions:
`probe_tiles.py` (choose a tile on measured evidence), `g1_overlays.py` (layer
alignment), `g2_terrain.py` (is the DTM really bare earth). None of it is on the
pipeline's path.

`g3_canopy.py` is the exception: it compares the two Meta canopy versions, and `fetch`
now pulls only v2, so it needs a cache holding both and says so if run without one. It
is kept for the record, and in case that decision is reopened.

---

## Limits worth knowing

- **No sensor physics.** One return per sample, no off-nadir rays, noise, intensity or
  beam footprint. That belongs to the user's simulator (`SPEC.md` section 3).
- **No tree stems.** Vertical surfaces return nothing to a nadir ray, so they cannot be
  validated until an off-nadir scanner exists.
- **One building height for every footprint** (6 m), deliberately, so the scene never
  mixes sourced and guessed heights.
- **Building footprints with interior rings throw.** None occur in the test tiles, and
  extruding a courtyard as solid would be silently wrong.
- **Crown base ratio and envelope shape are unmeasured placeholders.** So is the
  canopy-height disagreement between the Meta v1 and v2 products. All three need real
  airborne LiDAR to settle (`SPEC.md` section 12.10).

Gate evidence for every decision is in `NOTES/`, one directory per gate.
