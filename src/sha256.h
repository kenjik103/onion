#pragma once

#include <string>

// Lowercase hex SHA-256 of a file's contents, for the provenance record
// (SPEC.md 6.3). Throws std::runtime_error if the file cannot be read.
//
// Implemented here rather than pulled from OpenSSL: SPEC.md 4.6 caps dependencies
// at GDAL, DuckDB and Polyscope, and GDAL exposes no public hashing API.
std::string sha256_file(const std::string& path);
