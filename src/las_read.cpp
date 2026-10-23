#include "las_read.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {

using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

template <typename T>
T at(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

}  // namespace

LasPoints read_las_points(const std::string& path, double origin_x, double origin_y,
                          size_t max_points) {
    const FilePtr f(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!f) throw std::runtime_error("las: cannot open " + path);

    uint8_t h[375];
    if (std::fread(h, 1, sizeof h, f.get()) != sizeof h)
        throw std::runtime_error("las: short header: " + path);
    if (std::memcmp(h, "LASF", 4) != 0) throw std::runtime_error("las: not a LAS file: " + path);

    const uint32_t data_off = at<uint32_t>(h, 96);
    const uint8_t format = at<uint8_t>(h, 104);
    const uint16_t plen = at<uint16_t>(h, 105);
    const double sx = at<double>(h, 131), sy = at<double>(h, 139), sz = at<double>(h, 147);
    const double ox = at<double>(h, 155), oy = at<double>(h, 163), oz = at<double>(h, 171);
    uint64_t n = at<uint64_t>(h, 247);
    if (n == 0) n = at<uint32_t>(h, 107);  // a 1.2-style file would use the legacy count
    if (format != 6) throw std::runtime_error("las: expected point format 6 in " + path);

    LasPoints out;
    out.total = static_cast<size_t>(n);
    out.stride = n > max_points ? static_cast<int>((n + max_points - 1) / max_points) : 1;
    const size_t keep = static_cast<size_t>(n / out.stride) + 1;
    out.xyz.reserve(keep * 3);
    out.cls.reserve(keep);

    if (std::fseek(f.get(), static_cast<long>(data_off), SEEK_SET) != 0)
        throw std::runtime_error("las: cannot seek to the point data in " + path);

    // Read in blocks and take every stride-th record: seeking per point would make a
    // 51 M point file unusably slow to load.
    constexpr size_t BLOCK = 1u << 16;
    std::vector<uint8_t> buf(BLOCK * plen);
    uint64_t i = 0;
    while (i < n) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(BLOCK, n - i));
        const size_t got = std::fread(buf.data(), plen, want, f.get());
        if (got == 0) break;
        for (size_t k = 0; k < got; ++k, ++i) {
            if (i % out.stride) continue;
            const uint8_t* r = buf.data() + k * plen;
            out.xyz.push_back(static_cast<float>(at<int32_t>(r, 0) * sx + ox - origin_x));
            out.xyz.push_back(static_cast<float>(at<int32_t>(r, 4) * sy + oy - origin_y));
            out.xyz.push_back(static_cast<float>(at<int32_t>(r, 8) * sz + oz));
            // Point format 6: byte 15 is the flags byte, 16 is Classification.
            out.cls.push_back(r[16]);
        }
    }
    return out;
}
