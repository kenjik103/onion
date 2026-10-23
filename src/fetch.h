#pragma once

#include <string>

#include "request.h"

// SPEC.md 6: the only stage that touches the network.
void run_fetch(const TileRequest& r, const std::string& cache);
