#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <stdexcept>
#include <string>

#include <cpl_conv.h>
#include <gdal_priv.h>

#include "fetch.h"
#include "grid.h"
#include "request.h"

namespace {

const char* const USAGE =
    "usage:\n"
    "  onion fetch    --lat <deg> --lon <deg> --extent <m> --seed <n> --cache <dir>\n"
    "  onion generate --lat <deg> --lon <deg> --extent <m> --seed <n> --cache <dir> --out <dir>\n";

[[noreturn]] void usage_error(const std::string& msg) {
    throw std::runtime_error(msg + "\n" + USAGE);
}

using Flags = std::map<std::string, std::string>;

Flags parse_flags(int argc, char** argv) {
    Flags flags;
    for (int i = 0; i < argc; i += 2) {
        const std::string key = argv[i];
        if (key.rfind("--", 0) != 0 || key.size() <= 2)
            usage_error("expected a --flag, got: " + key);
        if (i + 1 >= argc) usage_error("flag has no value: " + key);
        if (!flags.emplace(key.substr(2), argv[i + 1]).second)
            usage_error("flag given more than once: " + key);
    }
    return flags;
}

// Flags are consumed as they are read so whatever is left over is unknown.
std::string take(Flags& flags, const std::string& name) {
    const auto it = flags.find(name);
    if (it == flags.end()) usage_error("missing required flag: --" + name);
    const std::string value = it->second;
    flags.erase(it);
    return value;
}

double take_double(Flags& flags, const std::string& name) {
    const std::string s = take(flags, name);
    try {
        size_t n = 0;
        const double v = std::stod(s, &n);
        if (n != s.size()) throw std::invalid_argument("trailing characters");
        return v;
    } catch (const std::exception&) {
        usage_error("--" + name + " is not a number: " + s);
    }
}

uint64_t take_uint64(Flags& flags, const std::string& name) {
    const std::string s = take(flags, name);
    // std::stoull accepts a leading '-' and wraps it to a huge value, so reject the
    // sign before parsing rather than letting a negative seed through silently.
    if (!s.empty() && s[0] == '-')
        usage_error("--" + name + " is not a non-negative integer: " + s);
    try {
        size_t n = 0;
        const unsigned long long v = std::stoull(s, &n);
        if (n != s.size()) throw std::invalid_argument("trailing characters");
        return v;
    } catch (const std::exception&) {
        usage_error("--" + name + " is not a non-negative integer: " + s);
    }
}

void reject_unknown(const Flags& flags) {
    if (!flags.empty()) usage_error("unknown flag: --" + flags.begin()->first);
}

TileRequest take_request(Flags& flags) {
    TileRequest r{};
    r.lat = take_double(flags, "lat");
    r.lon = take_double(flags, "lon");
    r.extent_m = take_double(flags, "extent");
    r.seed = take_uint64(flags, "seed");
    return r;
}

void print_grid(const TileRequest& r, const Grid& g) {
    std::printf("request    lat %.6f  lon %.6f  extent %.1f m  seed %llu\n",
                r.lat, r.lon, r.extent_m, static_cast<unsigned long long>(r.seed));
    std::printf("utm_epsg   %d\n", g.utm_epsg);
    std::printf("tile_m     %.3f %.3f  ->  %.3f %.3f\n",
                g.tile_m.xmin, g.tile_m.ymin, g.tile_m.xmax, g.tile_m.ymax);
    std::printf("padded_m   %.3f %.3f  ->  %.3f %.3f\n",
                g.padded_m.xmin, g.padded_m.ymin, g.padded_m.xmax, g.padded_m.ymax);
    std::printf("origin     %.3f %.3f\n", g.origin_x, g.origin_y);
    std::printf("lonlat     %.7f %.7f  ->  %.7f %.7f\n",
                g.lonlat.xmin, g.lonlat.ymin, g.lonlat.xmax, g.lonlat.ymax);
}

void cmd_fetch(Flags& flags) {
    const TileRequest r = take_request(flags);
    const std::string cache = take(flags, "cache");
    reject_unknown(flags);

    validate_request(r);
    print_grid(r, make_grid(r));
    std::printf("cache      %s\n", cache.c_str());
    run_fetch(r, cache);
}

void cmd_generate(Flags& flags) {
    const TileRequest r = take_request(flags);
    const std::string cache = take(flags, "cache");
    const std::string out = take(flags, "out");
    reject_unknown(flags);

    validate_request(r);
    print_grid(r, make_grid(r));
    std::printf("cache      %s\nout        %s\n", cache.c_str(), out.c_str());

    throw std::runtime_error("generate: not implemented until M4");
}

}  // namespace

int main(int argc, char** argv) {
    // Process-global, so both are set once here rather than inside a fetcher (gotcha 4).
    CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
    GDALAllRegister();

    try {
        if (argc < 2) usage_error("no subcommand given");
        const std::string cmd = argv[1];
        Flags flags = parse_flags(argc - 2, argv + 2);

        if (cmd == "fetch") {
            cmd_fetch(flags);
        } else if (cmd == "generate") {
            cmd_generate(flags);
        } else {
            usage_error("unknown subcommand: " + cmd);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
