# Gate G4: crown model

The scanner now intercepts through PAD voxels (SPEC.md 6.9). A full 1 km tile takes
18.5 s and 59.8% of its 51,008,164 points are class 5.

## 1. Beer-Lambert interception — synthetic

`tests/test_crown.cpp`, run by ctest. A uniform-PAD slab 10 m deep, 48,400 nadir rays,
G = 0.5, voxel 0.5 m. Measured interception against `1 - exp(-G * PAD * depth)`:

| PAD | measured | expected | difference | 4 s.e. |
|---|---|---|---|---|
| 0.10 | 0.3898 | 0.3935 | −0.0037 | 0.0089 |
| 0.25 | 0.7099 | 0.7135 | −0.0036 | 0.0082 |
| 0.50 | 0.9173 | 0.9179 | −0.0006 | 0.0050 |
| 1.00 | 0.9934 | 0.9933 | +0.0001 | 0.0015 |

Four values, all inside sampling error. The two low-PAD cases both came in slightly low,
so that was checked rather than waved through: over 12 seeds the mean bias is **+0.00039
(0.6 s.e.)** at PAD 0.10 and **−0.000003 (0.0 s.e.)** at PAD 0.25. The single-seed
deviation was noise, not an off-by-one in the column walk.

## 2. Beer-Lambert interception — the real tile

Stronger than the synthetic check, because the real tile has overlapping crowns, varying
depth and real terrain underneath. Optical depth was computed per sample straight from
`crowns.bin` (`tau = G * summed_PAD * voxel_size`) and compared with what the Monte Carlo
scanner actually produced:

```
optical depth under canopy: mean 3.44, median 3.25, p10 1.50, p90 5.75
analytic gap fraction  8.7%
measured gap fraction  8.7%
```

They agree to the printed precision.

## 3. No banding at voxel layers

`g4_onecrown.png` — a 20 m window through the canopy, voxel layers ticked down the left
margin. Point density falls smoothly from the canopy top with no horizontal striping
aligned to the ticks.

Quantitatively, over all 30,515,553 vegetation points, the phase of each hit inside its
own voxel layer:

```
fitted log-linear gradient     exp(+0.1402 * phase)
implied rate*voxel             0.1402  -> summed PAD 1.12 against a single-crown 0.5
residual from that exponential max 4.16%
largest single-bin excess      4.25%
```

**A flat phase distribution would be the wrong null.** A ray attenuates as it crosses a
voxel, so hits must concentrate toward the voxel top; the density should go as
`exp(+rate * voxel * phase)` and it does. A layer artefact would appear as a **spike** at
one phase, not a smooth gradient. There is none above 4.3%. The implied summed PAD of
1.12 against a nominal 0.5 is crown overlap, which M3 already showed is heavy.

## 4. Parameters, and what the data says

| parameter | value | basis |
|---|---|---|
| `g` | 0.5 | SPEC.md's default for randomly oriented leaves (Ross 1981) |
| `voxel_size_m` | 0.50 | measured: 0.25 m costs **6.04 GB** of `crowns.bin` per 1 km tile |
| `pad` | **0.30** | **settled at G4**: gives a 19.6% gap fraction, see below |
| `crown_base_ratio` | 0.4 | placeholder |
| `envelope` | ellipsoid | placeholder |

### PAD sets the canopy gap fraction

Gap fraction is the share of samples under canopy that reach a surface rather than being
intercepted. Computed analytically from this tile's own optical depths, so no re-run is
needed per value:

| PAD | gap fraction |
|---|---|
| 0.10 | 53.0% |
| 0.15 | 39.9% |
| 0.20 | 30.6% |
| **0.30** | **19.0%** (chosen; measured 19.6% after the radius cap) |
| 0.40 | 12.5% |
| **0.50** | 8.7% |
| 0.70 | 4.8% |
| 1.00 | 2.4% |

Published airborne-LiDAR gap fractions for closed leaf-on temperate deciduous forest sit
very roughly in the 10–30% band, but the figure depends heavily on pulse density, scan
angle and how "under canopy" is defined. **That range is asserted from the literature,
not measured here**, and SPEC.md 12.10 lists gap fraction as a metric for validation
against real airborne LiDAR — which is what would actually settle it.

## Still open, and deliberately not decided here

- **Crown radius outliers**, carried from M3: 6.6% of crowns exceed 10 m radius and the
  largest is 23.15 m, a watershed segment that swallowed a canopy patch rather than a
  tree. These inflate both the overlap and the optical depth.
- **Crown base ratio 0.4** puts the crown bottom at 40% of tree height, leaving a tall
  empty trunk space visible in `g4_crosssection.png`. Nothing here measures whether that
  is right; stems are out of scope (SPEC.md 3), so the gap has no returns at all.

---

## Decisions (2026-10-23), applied and re-measured

**PAD = 0.30** and **crown radius capped at 0.35 x tree height**, both confirmed by the
user. Re-run on Hoosier NF:

| | before | after |
|---|---|---|
| canopy gap fraction | 8.7% | **19.6%** |
| vegetation share of points | 59.8% | 47.9% |
| optical depth under canopy (mean) | 3.44 | 2.00 |
| crown radius max | 23.15 m | **12.68 m** |
| crown radius mean / p90 | 4.82 / 9.08 m | 4.64 / 8.75 m |
| crowns over 10 m radius | 6.6% | **3.5%** |
| r/h mean | 0.211 | 0.200 |

11.7% of crowns sit at the cap. The cap bounds crown extent without removing the tree, so
no canopy hole is opened where a runaway segment used to be. The largest crown is now
12.68 m rather than 8.05 m, because the ceiling scales with height and the tallest trees
are ~36 m.

Analytic and Monte Carlo gap fractions still agree exactly (19.6% / 19.6%), and the
banding residual is unchanged at 4.16% with no spike, so neither change disturbed the
interception model.

Two tree-archetype parameters remain placeholders, unmeasured here and recorded as such:
**crown_base_ratio 0.4** and the **ellipsoid envelope**. Validating either needs real
airborne LiDAR (SPEC.md 12.10).
