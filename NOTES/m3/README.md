# M3 proposal: tree detection

**Tile:** Hoosier NF, IN — 1198 x 1198 @ 0.9355 m = 125.61 ha, tree+canopy mask 86.8%.
**Prototype:** `tools/m3_detect_proto.py`. numpy only — no scipy, no skimage — so nothing
here depends on a primitive the C++ implementation could not have (SPEC.md 4.6).

## Method

1. **Mask** to WorldCover class 10 and canopy >= 2 m.
2. **Local maxima with a height-scaled window.** A fixed window splits a tall tree's wide
   crown into several detections. Window radius = `k * height`, `k` the one free parameter.
3. **Plateau handling, which the data forces.** G3 measured that canopy v2 is 1 m
   quantised with ~66% of adjacent pixels equal, so maxima arrive as tied plateaus, not
   single pixels. Plateaus are grouped by 8-connectivity and reduced to their centroid:
   one tree per plateau rather than dozens.
4. **Crown size by marker-controlled watershed**, flooding from the maxima in descending
   height. Crown radius is the radius of a circle with the segment's area.
5. **Ground elevation** sampled from the terrain raster at the tree position.

### Why watershed, and not the simpler thing I tried first

The first version measured crown radius by walking outward from each peak until the
surface dropped below half the peak height. **It is broken in closed canopy and the data
said so:** neighbouring crowns are as tall as the peak, so the walk never finds an edge
and simply runs to the search cap. Measured symptoms — `r_p90` pinned at the cap for every
`k`, and crown cover reaching **190%**. Flooding instead partitions the canopy among the
seeds, so every canopy pixel belongs to exactly one crown.

## Parameter sweep

| k | trees | per ha | h mean | r mean | r/h | corr(r,h) | r<1.5 m | r>10 m | implausible |
|---|---|---|---|---|---|---|---|---|---|
| 0.050 | 27409 | 218.2 | 23.29 | 2.76 | 0.119 | +0.287 | 36.6% | 1.4% | 38.0% |
| 0.075 | 15380 | 122.4 | 22.92 | 3.91 | 0.170 | +0.459 | 19.7% | 3.4% | 23.1% |
| **0.100** | **10575** | **84.2** | **22.87** | **4.83** | **0.211** | **+0.538** | 13.6% | 6.8% | **20.4%** |
| 0.150 | 6271 | 49.9 | 23.32 | 6.51 | 0.279 | +0.597 | 8.7% | 16.4% | 25.0% |
| 0.200 | 4821 | 38.4 | 23.55 | 7.49 | 0.318 | +0.619 | 7.6% | 24.8% | 32.4% |
| 0.300 | 3059 | 24.4 | 24.54 | 9.59 | 0.391 | +0.610 | 4.3% | 43.8% | 48.2% |

Three things point at **k = 0.100**:

- **Implausible crowns are minimised there (20.4%).** Small `k` over-segments (36.6% of
  "trees" get crowns under 1.5 m radius — plateau fragments, not trees); large `k` merges
  (43.8% exceed 10 m radius at k=0.30).
- **r/h = 0.211**, inside the usual 0.15–0.25 band for temperate broadleaf.
- **corr(r,h) = +0.538.** Crown width rising with tree height is an allometric
  relationship *emerging from the data*, not imposed. It is near zero (+0.287) where the
  detection over-segments, which is what one would expect if those detections were noise.

Height distribution at k=0.100 is unimodal and peaks at 25–30 m, as a mature closed
stand should.

## What I could not establish, stated plainly

**The data does not pin crown size on its own.** I tried to fix the crown scale
independently of the detection, by high-pass filtering the canopy and taking the first
zero of its autocorrelation. The answer just tracks the filter:

| detrend scale | implied crown diameter |
|---|---|
| 10 m | 4.68 m |
| 15 m | 6.55 m |
| 25 m | 9.36 m |

So the canopy surface has **no dominant spatial scale between ~5 and ~25 m**. "How many
trees is this patch of canopy" is partly a definition, not purely a measurement. `k` is
therefore a real parameter the user owns, and 84 trees/ha should be read as
*canopy-dominant stems visible from above* — a CHM cannot see suppressed understory trees,
and a nadir scanner would not return from them either.

The published 0.15–0.25 r/h band is a reference range asserted here, not measured on this
tile. Real ALS would settle both it and `k` (SPEC.md 12.10).

## A known artefact of my implementation

`zoom_k100.png` (crown boundaries white, stems red) shows thin radial slivers, mostly near
the clearing edge. **These are mine, not the data's.** I checked whether the canopy itself
was striped: directional anisotropy is 1.09 along the diagonals and 0.93 between axes,
i.e. isotropic, and `raw_zoom.png` shows a clean surface with well-separated crowns. The
slivers come from one-pass flooding across tied plateaus, and they are most of the 13.6%
of crowns under 1.5 m radius.

Options: accept them and filter crowns below a minimum radius, or break ties by distance
to the nearest seed. The second is a few lines more and would reduce the implausible
fraction; it is not obviously worth it before the archetype is settled at G4.

---

# M3 as built (2026-10-23)

Approved: **k = 0.100**, scene spec as a **single JSON file**.

Implemented in `src/raster.{h,cpp}` (load and sample cached rasters),
`src/trees.{h,cpp}` (detection), `src/scene_spec.{h,cpp}` (assembly and writing).
`generate` now assembles the scene spec and then stops at the builders, which are M4.

## The C++ reproduces the approved prototype

| | prototype | C++ |
|---|---|---|
| trees / ha | 84.2 | 84.8 |
| crown radius mean | 4.83 m | 4.82 m |
| r/h | 0.211 | 0.211 |
| corr(r, h) | +0.538 | +0.534 |
| crowns < 1.5 m | 13.6% | 13.7% |

The 0.7% difference in tree count comes from the land-cover mask: the prototype warped
land cover onto the canopy grid with `gdalwarp`, while the C++ samples it by coordinate.
Runtime is 0.16 s against minutes for the prototype.

## Results, Hoosier NF (final, 10,600 trees over 125.61 ha)

```
height   mean  22.87  p10 13.0  p50 24.0  p90 30.0  max 39.0
crown r  mean   4.82  p10 1.29  p50 4.29  p90 9.08  max 23.15
ground_z mean 157.23  range 119.61 .. 203.96 m
```

Height distribution is unimodal, peaking at 25–30 m, as a mature closed stand should be.

Fargo, for contrast: 1,589 trees at 12.7/ha, mean height 11.23 m — sparse urban street
trees, which is what that tile is.

## Two bugs found and fixed while building it

**1. Edge- and centre-based pixel coordinates were mixed.** `(x - origin) / px` is zero at
the raster's outer *edge*, but it was bounds-checked as though it were a pixel-*centre*
index, which excluded the outer half pixel on every side. It surfaced as a sample falling
outside the land-cover raster. `to_pixel()` now states the convention explicitly, and
`sample_bilinear` interpolates between pixel centres and clamps inside the outer half
pixel instead of extrapolating. The error message now names the raster, which it did not
before — that omission is why the first diagnosis was guesswork.

**2. Trees could land outside the padded box.** Each cached raster is snapped outward to a
whole number of *its own* pixels, so the canopy raster reaches past the padded box, and
detection over the full raster produced trees up to 0.3 m beyond it. That would have
failed M4's "no local coordinate exceeds the padded tile size". Detection is now confined
to the padded box: 0 of 10,600 trees fall outside, maximum local coordinate 1119.37 m
against the 1120 m box.

## Buildings

Measured before coding rather than assumed: of 795 Fargo buildings, 794 are single-part
and one has two parts; **no building in either test tile has an interior ring**.

- One scene-spec entry per polygon part, all parts sharing the building's id, so a
  multi-part building stays one object (SPEC.md 6.7) without discarding geometry.
  Verified: 796 entries, 795 distinct ids.
- An interior ring **throws**, naming the building. Courtyards would otherwise extrude as
  solid, and silently wrong geometry is worse than a stop (SPEC.md 4.2). None occur in the
  test tiles, so this is a known limit, not a handled case.

## Known limitations, carried forward to G4

- **13.7% of crowns have a radius under 1.5 m** and are watershed slivers along tied
  plateaus rather than trees. Visible as the small circles in
  `final_hoosier_zoom.png`.
- **6.6% exceed 10 m, with a maximum of 23.15 m.** A 23 m radius crown is a watershed
  segment that swallowed a whole canopy patch, not a tree. Voxelised as-is it would
  deposit plant area density over 1,683 m2. This needs addressing before or at G4, since
  G4 settles the crown envelope.
- Crown circles overlap heavily. That is correct in itself — SPEC.md 6.6 has overlapping
  crowns sum their PAD — but the spurious small crowns add density where no tree is.
