# M5: stand-in scanner and LAS output

`generate` now runs the whole chain to a point cloud. **3.69 s** for a 1 km tile,
single-threaded, producing 51,008,164 points in a 1.62 GiB LAS.

The scanner reads the contract files back off disk rather than reusing the in-memory
mesh, because SPEC.md 4.4 makes the contract the only scene input to the scanner. That
exercises the write/read round trip on every run.

## Sampling

7142 x 7142 cells of 0.14 m over the **tile**, not the padded box, one sample per cell at
a seeded random offset inside it. 1000 m is not a whole number of 0.14 m cells, so the
grid is `floor(1000 / 0.14) = 7142` cells spanning 999.88 m; the leftover strip under one
cell wide is not sampled.

Randomness is counter-based (SplitMix64 of seed, sample index, draw index), so a draw
depends only on those three numbers and never on evaluation order. Parallelising later
cannot change the output.

## M5 acceptance

| criterion | result |
|---|---|
| flat plane: point count equals cell count exactly | 10,000 points from 10,000 cells |
| flat plane: all class 2, all z on the plane | 0 wrong class, 0 further than 1 mm |
| box: roof points at roof height, none on walls | 0 wall points, 0 roof-height points outside the footprint |
| every point within 1 mm of a contract surface | worst 0.645 mm (Hoosier), 0.567 mm (Fargo), 400 points each |
| LAS opens in an independent reader | `pdal info` reads format 6, 34 B records, CRS, extra bytes |
| two runs give identical hashes | `points.las` and every other output byte-identical |
| density on open ground is exactly 1/0.14^2 | **51.0204081633**, equal to 1/0.14^2 to machine precision |

Synthetic tests are `tests/test_scanner.cpp`, run by ctest. The real-tile surface check is
`tools/m5_verify.py`, which parses the LAS from the format spec and the mesh from
FORMAT.md, then for each sampled point finds every triangle whose xy box contains it and
compares the highest against the point's z — the scanner's defining property, rechecked
without reusing the scanner's code.

Walls get no points because a vertical triangle has exactly zero signed area in xy and is
skipped. That falls out of the geometry rather than being special-cased.

## pdal reads it

```
dataformat_id    6          point_length  34        header_size  375
global_encoding  16         (WKT bit set)           count        51008164
scale            0.001      offset_x 551975.237444095  offset_y 4227615.92558186
srs              EPSG:32616 resolved from the WKT VLR
schema           ... object_id unsigned:4   <- the extra-bytes VLR
ReturnNumber     min 1 max 1      NumberOfReturns min 1 max 1
Classification   min 2 max 6      object_id       min 0 max 757 (Fargo)
```

## Two things worth recording

**The LAS writer streams.** A `std::vector<LasPoint>` for 51 M points would be about
1.6 GB of structs before a byte reached disk, and a 34-byte `fwrite` per point would
dominate the runtime. Points are written through a fixed record into a 1 MiB batch
buffer. `las_end` checks the number written against the count declared in the header,
since a mismatch produces a file readers silently truncate or over-read.

**A bug in the verification script, not the writer.** The first run reported 400 of 400
class disagreements. LAS point format 6 puts Classification at byte **16**; byte 15 is the
flags byte. The script was reading 15. `pdal` had already reported Classification 2–6
correctly, which is what made it clear the writer was fine and the checker was not.

## Build hygiene fixed here

`-Wall -Wextra -Wpedantic` was set globally and so applied to vendored Polyscope, which
emitted hundreds of warnings from `glad.c` and friends. The flags are now attached per
target, so only our own code is held to them.

## Not yet done

The scanner is **opaque only**. Vegetation interception through PAD voxels is gate G4,
which also settles the crown archetype. No point in the cloud is class 5 yet.
