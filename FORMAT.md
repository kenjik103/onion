# Scene geometry contract — format version 1

These files are the **only** scene input any scanner reads (SPEC.md 6.8): the stand-in
scanner in this repository, the viewer, and the user's proprietary sensor simulator.
Nothing here depends on the cache, the scene spec, or the generator's internals.

Three files live together in one output directory:

| file | contents |
|---|---|
| `scene_geometry.json` | metadata, object table, crown archetype |
| `opaque.ply` | every opaque surface, as triangles |
| `crowns.bin` | every vegetation crown, as plant-area-density voxels |

**Any change to any of these files bumps `format_version`.**

All binary data is **little-endian**. There is no padding or alignment anywhere: every
record is tightly packed, and field order below is byte order on disk.

---

## 1. Coordinates

Two frames are in play and mixing them up is the easiest mistake to make here.

- **World**: UTM metres in the EPSG code given by `epsg`, with elevation in metres above
  the **EGM2008 geoid** (not the ellipsoid — see SPEC.md gotcha 6).
- **Local**: what the geometry files actually store.
  - `x` is metres **east** of `local_origin[0]`.
  - `y` is metres **north** of `local_origin[1]`.
  - `z` is **unchanged from world**: metres above the EGM2008 geoid, absolute.

To recover world coordinates:

```
world_x = local_x + local_origin[0]
world_y = local_y + local_origin[1]
world_z = local_z
```

`local_origin` is the south-west corner of the padded tile and is stored as two
**float64** values in JSON. Geometry is **float32**.

**Why x and y are offset but z is not.** A UTM northing near 5,000,000 m has a float32
spacing of about 0.5 m, which would destroy the 14 cm sampling this project targets.
Subtracting the origin brings x and y into roughly [0, 1200], where float32 spacing is
about 0.0001 m. Elevations are already small (roughly -400 to 9000 m), so z needs no
offset and keeps a spacing better than 0.001 m.

Every local coordinate in the files lies inside `padded_bbox_local` in x and y.

---

## 2. `scene_geometry.json`

UTF-8 JSON. Example with the fields elided:

```json
{
  "format_version": 1,
  "epsg": 32616,
  "local_origin": [551975.23744409462, 4227615.9255818594],
  "padded_bbox_local": [0.0, 0.0, 1120.0, 1120.0],
  "crown_archetype": {
    "pad": 0.5,
    "g": 0.5,
    "voxel_size_m": 0.5,
    "crown_base_ratio": 0.4,
    "envelope": "ellipsoid"
  },
  "objects": [
    {"object_id": 0, "class": 2, "source": "gedtm30"},
    {"object_id": 1, "class": 6, "source": "overture:11b987d1-..."},
    {"object_id": 8, "class": 5, "source": "chm_crown"}
  ]
}
```

| field | type | meaning |
|---|---|---|
| `format_version` | int | this document describes version 1 |
| `epsg` | int | EPSG code of the UTM zone for world coordinates |
| `local_origin` | [float64, float64] | world x, y of the local frame's origin |
| `padded_bbox_local` | [float64 x4] | `xmin, ymin, xmax, ymax` in **local** metres; always starts at 0, 0 |
| `crown_archetype` | object | see section 4 |
| `objects` | array | the object table |

Each entry in `objects`:

| field | type | meaning |
|---|---|---|
| `object_id` | uint32 | referenced by faces in `opaque.ply` and records in `crowns.bin` |
| `class` | uint8 | ASPRS classification code |
| `source` | string | provenance of this object; informational, do not parse |

Class codes used:

| code | meaning | appears in |
|---|---|---|
| 2 | ground | `opaque.ply` |
| 5 | high vegetation | `crowns.bin` |
| 6 | building | `opaque.ply` |

`object_id` values are unique across the whole scene, so one id never means a building in
one file and a crown in another. Ids are assigned in this order: `0` is the terrain, then
one per building, then one per crown. **Do not rely on that ordering** — use the table.

---

## 3. `opaque.ply`

Binary little-endian PLY. Every opaque surface in the scene is here, and opaque surfaces
are **triangles only**.

Header, verbatim apart from the two counts:

```
ply
format binary_little_endian 1.0
comment onion scene geometry contract version 1
element vertex <V>
property float x
property float y
property float z
element face <F>
property list uchar uint vertex_indices
property uint object_id
property uchar class
end_header
```

The header ends with the newline after `end_header`. Binary data begins at the next byte.

### Vertex block

`V` records of **12 bytes** each, immediately after the header:

| offset | type | field |
|---|---|---|
| 0 | float32 | x (local) |
| 4 | float32 | y (local) |
| 8 | float32 | z (absolute, EGM2008) |

### Face block

`F` records of **18 bytes** each, immediately after the vertex block:

| offset | type | field |
|---|---|---|
| 0 | uint8 | vertex count, **always 3** |
| 1 | uint32 | vertex index 0 |
| 5 | uint32 | vertex index 1 |
| 9 | uint32 | vertex index 2 |
| 13 | uint32 | `object_id` |
| 17 | uint8 | `class` |

Vertex indices are zero-based into the vertex block.

Because every face is a triangle, the face block is exactly `18 * F` bytes and can be read
in one pass. A general PLY reader that honours the `list uchar uint` property will also
work; the fixed size is a convenience, not a separate encoding.

Winding is counter-clockwise seen from outside, but **do not rely on it for visibility**:
the stand-in scanner treats every triangle as two-sided (SPEC.md 6.9).

---

## 4. `crowns.bin`

Vegetation is **never** triangles. Each crown is a rectangular block of plant area
density (PAD) voxels cut from one tile-wide lattice.

### The lattice

A single lattice is shared by every crown, so blocks from different crowns line up exactly
and overlapping crowns can be combined by **summing PAD per voxel**.

With `s = crown_archetype.voxel_size_m`, voxel index `(i, j, k)` covers the half-open box

```
x in [ i*s, (i+1)*s )      local metres
y in [ j*s, (j+1)*s )      local metres
z in [ k*s, (k+1)*s )      absolute metres above the EGM2008 geoid
```

Note the asymmetry, and it is deliberate: **x and y are local, z is absolute**, matching
section 1. Indices are signed, and `k` in particular is large (an elevation of 157 m at
`s = 0.5` gives `k = 314`).

### File layout

Header, 16 bytes:

| offset | type | value |
|---|---|---|
| 0 | char[8] | `ONIONCRW`, no terminator |
| 8 | uint32 | format version, matches `scene_geometry.json` |
| 12 | uint32 | `N`, number of crown records |

Then `N` records, back to back with no padding. Record header, 28 bytes:

| offset | type | field |
|---|---|---|
| 0 | uint32 | `object_id` |
| 4 | int32 | `ix`, lattice x index of the block's minimum corner |
| 8 | int32 | `iy` |
| 12 | int32 | `iz` |
| 16 | uint32 | `nx`, block size in x |
| 20 | uint32 | `ny` |
| 24 | uint32 | `nz` |

Immediately followed by `nx * ny * nz` **float32** PAD values in m2/m3, ordered with
**x fastest, then y, then z**:

```
value_offset(i, j, k) = ((k * ny) + j) * nx + i     // 0 <= i < nx, etc.
```

so a record occupies `28 + 4 * nx * ny * nz` bytes. Records are variable length; to reach
record `n + 1` you must read record `n`'s dimensions. There is no index.

A voxel's PAD is `0.0` where the crown's envelope does not reach, which is common: the
envelope is an ellipsoid inscribed in the block, so roughly 48% of a block is zero.

### Using PAD

The stand-in scanner (SPEC.md 6.9) treats a voxel as a turbid medium. A ray travelling a
distance `d` through a voxel of density `PAD` is intercepted with probability

```
p = 1 - exp(-G * PAD * d)
```

with `G` from `crown_archetype.g`, the nadir projection coefficient (0.5 for randomly
oriented leaves). Where crowns overlap, sum PAD from every record covering that voxel
before applying the formula.

### Crown archetype fields

| field | meaning |
|---|---|
| `pad` | the PAD value written for every voxel inside an envelope |
| `g` | projection coefficient for the Beer–Lambert interception above |
| `voxel_size_m` | lattice spacing `s`, metres |
| `crown_base_ratio` | crown occupies from this fraction of tree height up to the top |
| `envelope` | envelope shape; `"ellipsoid"` in version 1 |

In version 1 every crown uses one archetype, so every non-zero PAD value in the file is
identical. **Do not depend on that** — read the values.

---

## 5. What is not here

Deliberate omissions, so a reader does not go looking:

- **No tree stems.** Vertical surfaces return essentially nothing to a nadir ray, so stems
  cannot be validated until an off-nadir scanner exists (SPEC.md 3).
- **No normals, colours, or texture coordinates.**
- **No material or reflectance.** The sensor model owns anything wavelength dependent.
- **No interior detail.** Buildings are closed flat-roofed shells.
