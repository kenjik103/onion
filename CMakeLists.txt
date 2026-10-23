cmake_minimum_required(VERSION 3.20)
project(onion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(GDAL REQUIRED)

# DuckDB serves the Overture buildings query (SPEC.md 8.3), and only inside `fetch`.
find_library(DUCKDB_LIBRARY NAMES duckdb REQUIRED)
message(STATUS "DuckDB library: ${DUCKDB_LIBRARY}")

add_library(onion_core STATIC
  src/request.cpp
  src/grid.cpp
  src/sha256.cpp
  src/cache.cpp
  src/sources.cpp
  src/fetch_raster.cpp
  src/fetch.cpp
  src/buildings.cpp
  src/raster.cpp
  src/trees.cpp
  src/scene_spec.cpp
)
target_include_directories(onion_core PUBLIC src)
target_link_libraries(onion_core PUBLIC GDAL::GDAL ${DUCKDB_LIBRARY})

add_executable(onion src/main.cpp)
target_link_libraries(onion PRIVATE onion_core)

enable_testing()
add_executable(test_grid tests/test_grid.cpp)
target_link_libraries(test_grid PRIVATE onion_core)
add_test(NAME grid COMMAND test_grid)

add_executable(test_sources tests/test_sources.cpp)
target_link_libraries(test_sources PRIVATE onion_core)
add_test(NAME sources COMMAND test_sources)
