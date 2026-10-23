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

# DuckDB serves the Overture buildings query from M2 on (SPEC.md 8.3), and only
# inside `fetch`. Nothing in M0/M1 uses it, so it is located but not linked:
# asserting it here makes a missing dependency visible now rather than at M2.
find_library(DUCKDB_LIBRARY NAMES duckdb REQUIRED)
message(STATUS "DuckDB library: ${DUCKDB_LIBRARY}")

add_library(onion_core STATIC
  src/request.cpp
  src/grid.cpp
)
target_include_directories(onion_core PUBLIC src)
target_link_libraries(onion_core PUBLIC GDAL::GDAL)

add_executable(onion src/main.cpp)
target_link_libraries(onion PRIVATE onion_core)

enable_testing()
add_executable(test_grid tests/test_grid.cpp)
target_link_libraries(test_grid PRIVATE onion_core)
add_test(NAME grid COMMAND test_grid)
