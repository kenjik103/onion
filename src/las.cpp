#include "las.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr double LAS_SCALE = 0.001;      // metres
constexpr uint16_t HEADER_SIZE = 375;    // LAS 1.4
constexpr uint16_t POINT_LENGTH = 34;    // format 6 (30) + one uint32 extra byte
constexpr size_t BATCH = 1u << 20;  // output buffer, bytes

struct Buf {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }
    template <typename T>
    void put(T v) { raw(&v, sizeof(T)); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void text(const std::string& s, size_t n) {
        for (size_t i = 0; i < n; ++i) b.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    }
};

int32_t quantise(double world, double offset, const char* axis) {
    const double v = std::round((world - offset) / LAS_SCALE);
    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
        throw std::runtime_error(std::string("las: ") + axis + " does not fit in int32");
    return static_cast<int32_t>(v);
}

// LAS 1.4 Extra Bytes descriptor, 192 bytes, declaring one uint32 named object_id.
void extra_bytes_record(Buf& v) {
    v.zeros(2);        // reserved
    v.put<uint8_t>(5); // data type 5 = unsigned long (uint32)
    v.put<uint8_t>(0); // options: no no_data, min, max, scale or offset
    v.text("object_id", 32);
    v.zeros(4);        // unused
    v.zeros(24);       // no_data[3]
    v.zeros(24);       // min[3]
    v.zeros(24);       // max[3]
    v.zeros(24);       // scale[3]
    v.zeros(24);       // offset[3]
    v.text("scene geometry object id", 32);
}

void vlr_header(Buf& v, const std::string& user, uint16_t record_id, uint16_t len,
                const std::string& desc) {
    v.put<uint16_t>(0);  // reserved
    v.text(user, 16);
    v.put<uint16_t>(record_id);
    v.put<uint16_t>(len);
    v.text(desc, 32);
}

}  // namespace

LasOut las_begin(const std::string& path, double offset_x, double offset_y,
                 const std::string& wkt, uint64_t n_points, const double bounds[6]) {
    Buf vlrs;
    // OGC WKT coordinate system, required for point formats 6 and up (gotcha 11).
    const uint16_t wkt_len = static_cast<uint16_t>(wkt.size() + 1);
    vlr_header(vlrs, "LASF_Projection", 2112, wkt_len, "OGC WKT coordinate system");
    vlrs.raw(wkt.data(), wkt.size());
    vlrs.put<uint8_t>(0);  // null terminated, as the spec requires

    Buf eb;
    extra_bytes_record(eb);
    vlr_header(vlrs, "LASF_Spec", 4, static_cast<uint16_t>(eb.b.size()), "Extra Bytes Record");
    vlrs.raw(eb.b.data(), eb.b.size());

    Buf h;
    h.text("LASF", 4);
    h.put<uint16_t>(0);   // file source id
    h.put<uint16_t>(16);  // global encoding: bit 4 set = WKT (gotcha 11)
    h.zeros(16);          // project id GUID
    h.put<uint8_t>(1);
    h.put<uint8_t>(4);
    h.text("onion", 32);
    h.text("onion generate", 32);
    h.put<uint16_t>(1);     // creation day: fixed, so two runs stay byte-identical
    h.put<uint16_t>(2026);  // creation year
    h.put<uint16_t>(HEADER_SIZE);
    h.put<uint32_t>(HEADER_SIZE + static_cast<uint32_t>(vlrs.b.size()));
    h.put<uint32_t>(2);  // number of VLRs
    h.put<uint8_t>(6);   // point data record format
    h.put<uint16_t>(POINT_LENGTH);
    // Legacy counts stay 0: format 6 is beyond what they can describe, and the 1.4
    // 64-bit fields below are authoritative.
    h.put<uint32_t>(0);
    for (int i = 0; i < 5; ++i) h.put<uint32_t>(0);
    h.put<double>(LAS_SCALE); h.put<double>(LAS_SCALE); h.put<double>(LAS_SCALE);
    h.put<double>(offset_x); h.put<double>(offset_y); h.put<double>(0.0);
    h.put<double>(bounds[1]); h.put<double>(bounds[0]);
    h.put<double>(bounds[3]); h.put<double>(bounds[2]);
    h.put<double>(bounds[5]); h.put<double>(bounds[4]);
    h.put<uint64_t>(0);  // start of waveform data
    h.put<uint64_t>(0);  // start of first EVLR
    h.put<uint32_t>(0);  // number of EVLRs
    h.put<uint64_t>(n_points);
    h.put<uint64_t>(n_points);                       // points by return [1]
    for (int i = 1; i < 15; ++i) h.put<uint64_t>(0); // returns 2..15
    if (h.b.size() != HEADER_SIZE)
        throw std::runtime_error("las: header is " + std::to_string(h.b.size()) +
                                 " bytes, expected 375");

    LasOut o;
    o.path = path;
    o.offset_x = offset_x;
    o.offset_y = offset_y;
    o.declared = n_points;
    o.f = std::fopen(path.c_str(), "wb");
    if (!o.f) throw std::runtime_error("las: cannot write " + path);
    if (std::fwrite(h.b.data(), 1, h.b.size(), o.f) != h.b.size() ||
        std::fwrite(vlrs.b.data(), 1, vlrs.b.size(), o.f) != vlrs.b.size())
        throw std::runtime_error("las: write failed: " + path);
    o.batch.reserve(BATCH + POINT_LENGTH);
    return o;
}

void las_point(LasOut& o, const LasPoint& p) {
    uint8_t rec[POINT_LENGTH];
    size_t n = 0;
    const auto emit = [&](const void* src, size_t len) {
        std::memcpy(rec + n, src, len);
        n += len;
    };
    const int32_t xi = quantise(p.x, o.offset_x, "x");
    const int32_t yi = quantise(p.y, o.offset_y, "y");
    const int32_t zi = quantise(p.z, 0.0, "z");
    emit(&xi, 4);
    emit(&yi, 4);
    emit(&zi, 4);
    const uint16_t intensity = 0;
    emit(&intensity, 2);
    rec[n++] = 0x11;    // return number 1, number of returns 1
    rec[n++] = 0;       // classification flags, scanner channel, direction, edge
    rec[n++] = p.cls;
    rec[n++] = 0;       // user data
    const int16_t scan_angle = 0;
    emit(&scan_angle, 2);
    const uint16_t source_id = 0;
    emit(&source_id, 2);
    const double gps = 0.0;
    emit(&gps, 8);
    emit(&p.object_id, 4);

    o.batch.insert(o.batch.end(), rec, rec + POINT_LENGTH);
    ++o.written;
    if (o.batch.size() >= BATCH) {
        if (std::fwrite(o.batch.data(), 1, o.batch.size(), o.f) != o.batch.size())
            throw std::runtime_error("las: write failed: " + o.path);
        o.batch.clear();
    }
}

void las_end(LasOut& o) {
    if (!o.batch.empty() &&
        std::fwrite(o.batch.data(), 1, o.batch.size(), o.f) != o.batch.size())
        throw std::runtime_error("las: write failed: " + o.path);
    std::fclose(o.f);
    o.f = nullptr;
    // The header already declares the count, so a mismatch would produce a file that
    // readers silently truncate or over-read.
    if (o.written != o.declared)
        throw std::runtime_error("las: wrote " + std::to_string(o.written) +
                                 " points but the header declares " +
                                 std::to_string(o.declared));
}
