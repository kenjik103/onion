# Environment report (M0)

Verified **2026-10-23** on the development workstation (Arch Linux, kernel 7.2.6).

| Item | Finding |
|---|---|
| CMake | 4.4.3 |
| Compiler | GCC 16.2.1 |
| GDAL | 3.13.3 "Iowa City" — raster and vector OK, `/vsicurl/` reaches every source |
| DuckDB | **not installed**; `extra/duckdb 1.5.6-1` available |
| Polyscope | not packaged for Arch — vendor via CMake FetchContent at V1 |
| PDAL | `/usr/bin/pdal` present — the independent LAS reader M5 needs |
| EGL / GLFW | `libEGL.so.1`, `libglfw.so.3.5` present — V1 headless screenshots viable |

## M0 required report: can this GDAL read Overture Parquet from S3?

**No.** `ogrinfo --formats | grep -i parquet` returns nothing; only ADBC is listed.

The plugin ships but cannot load:

```
$ pacman -Qo /usr/lib/gdalplugins/ogr_Parquet.so
/usr/lib/gdalplugins/ogr_Parquet.so is owned by gdal 3.13.3-2

$ ldd /usr/lib/gdalplugins/ogr_Parquet.so | grep 'not found'
	libarrow_dataset.so.2500 => not found
	libparquet.so.2500 => not found
	libarrow_compute.so.2500 => not found
	libarrow.so.2500 => not found
```

The `gdal` package was built against **Arrow 25** (`.so.2500`), but Arch's current
`arrow` package is **26.0.0** (`.so.2600`). Installing `arrow` does **not** fix this —
the ABI versions do not match. Getting the Parquet driver would require rebuilding GDAL
against Arrow 26, which pins the project to a custom GDAL build and complicates the
eventual Apptainer image.

### Decision (SPEC.md 11, "whether to drop DuckDB")

**Keep DuckDB.** It is a clean distribution package, SPEC.md 8.3 already sketches the
query, and it runs only inside `fetch` — the offline `generate` path is unaffected
either way, so this choice carries no risk for the compute-cluster constraint (SPEC.md 5).

## Build dependency policy

- **GDAL** — system package, linked now.
- **DuckDB** — system package. Located by CMake (`find_library(... REQUIRED)`) but **not
  linked** until M2, since nothing in M0/M1 uses it. Asserting it at configure time
  makes a missing dependency visible now instead of at M2.
- **Polyscope** — vendored via CMake FetchContent at V1, pinned to an exact tag.
- **No test framework.** SPEC.md 4.6 caps dependencies at GDAL, DuckDB and Polyscope, so
  `tests/test_grid.cpp` uses plain checks driven by CTest.
