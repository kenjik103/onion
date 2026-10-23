# V1: viewer, scene geometry

`onion_viewer` is a separate executable built on Polyscope. It keeps SPEC.md 6.11's
three rules: it reads only the scene geometry contract, it launches the CLI as
subprocesses rather than calling into the pipeline, and `--screenshot` renders one frame
headless and exits.

```
onion_viewer [--contract <dir>] [--screenshot <path.png>] [--view top|oblique]
```

## V1 acceptance

| criterion | result |
|---|---|
| empty Polyscope window opens on the workstation | backend `openGL3_glfw`, OpenGL 4.6, window stayed open on display `:1` |
| `--screenshot` writes a PNG headless | `openGL3_egl`, EGL 1.5, with `DISPLAY` and `WAYLAND_DISPLAY` unset — `v1_empty_headless.png`, 1280x720 |
| terrain, buildings and crowns render with toggles | three structures, each with Polyscope's enable toggle |
| screenshots match the G1 overlays | `v1_fargo_top.png` against `NOTES/g1/02_landcover_buildings.png` |
| Run produces the same outputs as the CLI | byte-identical on all four output files |

### Screenshots match G1

`v1_fargo_top.png` is the Fargo tile from near-nadir, the same view the G1 overlays use.
The street grid, the large commercial footprints through the centre and right, and the
crown cluster in the residential north-west all line up with
`NOTES/g1/02_landcover_buildings.png`.

`v1_hoosier_obl.png` is Hoosier NF obliquely: terrain relief under a near-continuous
canopy, with the clearing right of centre showing as bare ground — the same clearing that
appears in the G2 difference image and the canopy map.

**Display-side decimation, stated as SPEC.md requires:** the Hoosier tile holds
91,209,364 non-empty crown voxels and the viewer draws **1,982,813 of them, 1 in 46**.
This is display only: it is never written back, and the viewer prints the factor on load
and into its log panel.

### Run really is the CLI

`do_run()` composes two command strings and runs them with `popen` on a detached thread,
so a slow fetch never blocks the window. Running those exact strings by hand reproduces
`opaque.ply`, `crowns.bin`, `scene_geometry.json` and `scene_spec.json` byte for byte
against the CLI-produced tile. That is true by construction — the viewer runs the same
binary — and it is checked rather than assumed.

## Two bugs found by looking at the output

1. **The terrain rendered standing on its edge.** Polyscope defaults to Y-up, while the
   contract stores z as elevation (FORMAT.md section 1). Fixed with an explicit
   `setUpDir(ZUp)`. This is exactly the kind of error a screenshot catches and a unit
   test does not.
2. **The default home view was a grazing angle** that flattened the tile to a sliver.
   The camera is now derived from the contract's own bounding box, so screenshots are
   reproducible and independent of Polyscope's automatic framing.

Polyscope also warns that sphere impostors do not scale to point counts this large; the
crowns use `PointRenderMode::Quad`.

## Polyscope is vendored, not fetched

`third_party/polyscope`, pinned to **v2.6.1**, and git-ignored. CMake fails with the
exact clone command if it is missing.

**Why not `FetchContent`:** it cannot make submodule clones shallow. `GIT_SHALLOW`
applies only to the top-level repository, and Polyscope pulls in imgui, glfw, glm and
others. Measured here, the full-history submodule clone was still running after 30
minutes at about 50 KB/s; the shallow recursive clone finished in 9m46s and is 80 MB.

```
git clone --depth 1 --branch v2.6.1 --recurse-submodules --shallow-submodules \
    https://github.com/nmwsharp/polyscope.git third_party/polyscope
```

Polyscope's headers are included as SYSTEM headers, so its warnings under
`-Wall -Wextra -Wpedantic` do not drown our own.

## Not yet done (V2)

The point cloud is not loaded — there is no LAS until M5. Layer toggles cover terrain,
buildings and crowns only.
