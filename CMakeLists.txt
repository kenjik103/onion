cmake_minimum_required(VERSION 3.20)
project(onion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Applied per target, not globally: vendored Polyscope must not be compiled
# under our warning flags.
set(ONION_WARNINGS -Wall -Wextra -Wpedantic)

find_package(GDAL REQUIRED)

# The manifest records the generator's git commit (SPEC.md 6.10).
execute_process(COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE ONION_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT ONION_GIT_COMMIT)
  set(ONION_GIT_COMMIT "unknown")
endif()
add_compile_definitions(ONION_GIT_COMMIT="${ONION_GIT_COMMIT}")

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
  src/builders.cpp
  src/contract_write.cpp
  src/contract_read.cpp
  src/scanner.cpp
  src/las.cpp
  src/manifest.cpp
)
target_include_directories(onion_core PUBLIC src)
target_compile_options(onion_core PRIVATE ${ONION_WARNINGS})
target_link_libraries(onion_core PUBLIC GDAL::GDAL ${DUCKDB_LIBRARY})

add_executable(onion src/main.cpp)
target_link_libraries(onion PRIVATE onion_core)
target_compile_options(onion PRIVATE ${ONION_WARNINGS})

# --- Viewer (SPEC.md 6.11) -------------------------------------------------------
# Polyscope is vendored, not packaged for this distribution. It is cloned out of band
# rather than by FetchContent: FetchContent cannot make submodule clones shallow, and
# Polyscope pulls in imgui, glfw and glm, whose full histories took the better part of
# an hour here against under ten minutes shallow.
set(POLYSCOPE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/polyscope")
if(NOT EXISTS "${POLYSCOPE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Polyscope is missing. Clone it (pinned tag, shallow including submodules):\n"
    "  git clone --depth 1 --branch v2.6.1 --recurse-submodules --shallow-submodules \\\n"
    "      https://github.com/nmwsharp/polyscope.git third_party/polyscope")
endif()
add_subdirectory("${POLYSCOPE_DIR}" EXCLUDE_FROM_ALL)

add_executable(onion_viewer src/viewer.cpp)
target_include_directories(onion_viewer PRIVATE src)
target_link_libraries(onion_viewer PRIVATE onion_core polyscope)
target_compile_options(onion_viewer PRIVATE ${ONION_WARNINGS})
# Polyscope's headers are noisy under -Wall -Wextra; treat them as system headers so
# warnings from our own code stay visible.
get_target_property(_ps_inc polyscope INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(onion_viewer SYSTEM PRIVATE ${_ps_inc})

enable_testing()
add_executable(test_grid tests/test_grid.cpp)
target_link_libraries(test_grid PRIVATE onion_core)
target_compile_options(test_grid PRIVATE ${ONION_WARNINGS})
add_test(NAME grid COMMAND test_grid)

add_executable(test_sources tests/test_sources.cpp)
target_link_libraries(test_sources PRIVATE onion_core)
target_compile_options(test_sources PRIVATE ${ONION_WARNINGS})
add_test(NAME sources COMMAND test_sources)

add_executable(test_scanner tests/test_scanner.cpp)
target_link_libraries(test_scanner PRIVATE onion_core)
target_compile_options(test_scanner PRIVATE ${ONION_WARNINGS})
add_test(NAME scanner COMMAND test_scanner)
