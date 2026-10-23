# M4: builders and the scene geometry contract

`generate` now runs the full chain: cache -> scene spec -> builders -> contract.
Runtime for a 1 km tile is **0.99 s** (Hoosier) and **0.26 s** (Fargo).

## Output, Hoosier NF

| file | size | contents |
|---|---|---|
| `opaque.ply` | 60.2 MB | 1,256,705 vertices, 2,508,882 triangles |
| `crowns.bin` | 769.6 MB | 10,600 crowns, 192,317,305 voxels |
| `scene_geometry.json` | 859 KB | 10,608 objects, archetype, grid |

## M4 acceptance

| criterion | result |
|---|---|
| contract files plus `FORMAT.md` | yes |
| a separate reader written only from `FORMAT.md` loads every file | yes — `tools/contract_reader.py` |
| terrain covers the padded tile with no gaps | 2,508,800 ground faces = exactly 2 x 1120 x 1120 |
| every primitive has an object id and a class | 0 faces and 0 crowns mismatched against the object table |
| no local coordinate exceeds the padded tile size | vertices 0.000–1120.000; crown blocks 0.00–1120.00 |

Two runs produce byte-identical `opaque.ply`, `crowns.bin` and `scene_geometry.json`.

`tools/contract_reader.py` consults no generator source. It parses the PLY header
generically rather than assuming the fixed 18-byte face record, so it exercises the
documentation rather than the implementation.

## Two bugs the acceptance checks caught

Both are the same mistake in different places: **geometry derived from something that
legitimately extends past the padded tile**.

1. **Crown blocks ran from -8.00 to 1132.50.** A crown centred inside the tile reaches
   past the edge by up to its radius. Crown blocks are now clipped to the padded tile in
   x and y. Nothing sampled is lost: the scanner samples only the tile proper, 60 m
   inside the padded edge.
2. **804 building vertices outside the box on Fargo, x -56.9 to 1204.5.** Overture's
   `bbox` filter is an overlap test, so an edge-straddling footprint comes back whole —
   which is correct for the cache and wrong for the contract. Footprints are now clipped
   with Sutherland-Hodgman against the padded rectangle; 759 of 796 survive and 37 fall
   entirely outside.

**The second bug only appeared on the second tile.** Hoosier has 7 rural buildings, none
near an edge, and it passed clean. Running the urban tile as well is what found it.

## Checks worth keeping

- **Ear clipping is exact.** Footprints are often concave (L and U shapes), where a
  triangle fan would put roof geometry outside the building. Across Fargo's 796
  footprints the builder emits 12,832 triangles against `sum(3n-2) = 12,832` predicted
  from the vertex counts, so every ring triangulated fully and none silently bailed.
- Envelope fill is 0.27–0.53 of each crown's block, consistent with an ellipsoid
  inscribed in its bounding box (pi/6 = 0.52) and lower where clipping trimmed a block.

## Crown voxel size: 0.50 m, not SPEC.md's 0.25 m

Measured against the real tree list before choosing:

| voxel size | voxels | dense `crowns.bin` |
|---|---|---|
| 0.25 m | 1,511 M | **6.04 GB** |
| 0.50 m | 203 M | 0.81 GB |
| 1.00 m | 29 M | 0.12 GB |

SPEC.md 11 offers 0.25 m "as in prior work". At 6 GB for a single 1 km tile — against a
~1.7 GB point cloud — that is impractical, so 0.50 m is used provisionally and the
trade-off is recorded for **G4**, which settles the archetype.

Worth knowing for that discussion: in version 1 every crown uses one archetype, so every
non-zero PAD value in the file is the same number. The format stores float32 per voxel
because overlapping crowns sum and future archetypes will vary, but today the file is a
constant times a binary mask. If size becomes the binding constraint, that redundancy is
where to look — though changing it is a contract change, so it is the user's call, not
a silent optimisation.

## Still carried from M3

Crown radius outliers are untouched: 6.6% exceed 10 m and the largest is 23.15 m, a
watershed segment that swallowed a canopy patch. At 0.50 m voxels that one crown is
0.37 M voxels. G4 should settle whether to cap crown radius.

## Archetype values used (all provisional, for G4)

| parameter | value | source |
|---|---|---|
| `pad` | 0.5 m2/m3 | placeholder |
| `g` | 0.5 | SPEC.md default, randomly oriented leaves |
| `crown_base_ratio` | 0.4 | placeholder |
| `voxel_size_m` | 0.5 | measured, see above |
| `envelope` | ellipsoid | placeholder |
