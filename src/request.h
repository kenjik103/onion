#pragma once

#include <cstdint>

// SPEC.md 6.1. The seed does not affect anything `fetch` writes (fetching is
// deterministic); it is carried on the request so the manifest can record the
// request and the seed together, as SPEC.md 6.10 requires.
struct TileRequest {
    double lat;
    double lon;
    double extent_m;
    uint64_t seed;
};

// Throws std::runtime_error naming the source, the tile and what was wrong
// (SPEC.md 4.1). NaN fails every bound, so no separate NaN check is needed.
void validate_request(const TileRequest& r);
