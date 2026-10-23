# V2: viewer, point cloud

The viewer loads `points.las` alongside the contract and renders it coloured by ASPRS
class, over the scene geometry. Four toggleable layers: **terrain**, **buildings**,
**crowns**, **point cloud**.

```
onion_viewer --contract <dir> [--screenshot <path.png>] [--view top|oblique|closeup|profile]
```

Class colours follow FORMAT.md section 2: **2 ground** orange, **5 high vegetation**
green, **6 building** red. The terrain and building meshes are deliberately muted grey so
the class-coloured cloud reads against them.

## V2 acceptance

| criterion | result |
|---|---|
| the LAS renders coloured by class | three classes, distinct colours |
| overlaid on the scene geometry | mesh layers stay on; crowns start hidden so they do not obscure the cloud cast through them |
| a screenshot shows points sitting on the corresponding surfaces | `v2_fargo_closeup.png` |
| decimation shown on screen, never written back, stated on evidence | printed on load and into the UI log panel |

### Points sit on surfaces

`v2_fargo_closeup.png` is the clearest evidence. Red class-6 points cover each flat roof
in a regular grid; orange class-2 points cover the terrain between buildings; a few green
class-5 points appear at the left. **The vertical walls are completely clean**, which is
M5's "no points on walls" visible rather than merely asserted — a wall triangle has
exactly zero signed area in xy, so no nadir sample can land on it.

The L-shaped building in the foreground also shows the ear-clipping triangulation
working: its roof is filled correctly across a concave footprint.

`v2_hoosier_profile.png` is the forested equivalent, viewed near-horizontally: vegetation
points form a canopy layer above the ground points, with a small building visible right of
centre. A top-down or steeply oblique view is **not** useful on a forested tile — at
1120 m across, a 7 cm point is sub-pixel and the canopy reads as faint dust
(`v2_hoosier_closeup.png` shows this limit).

### Decimation, stated as SPEC.md requires

```
points 2833787 of 51008164 shown (1 in 18), display decimation only
```

Printed on load and shown in the UI panel. The point radius is **0.07 m, half the 0.14 m
sample spacing**, so points are at their true physical size rather than inflated to be
visible. Nothing is written back.

## Notes

- The LAS reader parses the format from the spec and reads in 64 Ki-record blocks,
  taking every stride-th point. Seeking per point would make a 51 M point file unusably
  slow to open.
- `fflush` after the load summary: the window then blocks in `show()`, and without it the
  summary sits in the buffer and is lost whenever the viewer's output is piped or the
  process is killed. This surfaced as an apparently missing log line during testing.
- The close-up camera is anchored on the **ground height at the tile centre**, not the
  midpoint of the whole z range. On a tile with relief the latter puts the camera
  underneath the terrain, which is what the first attempt did.
