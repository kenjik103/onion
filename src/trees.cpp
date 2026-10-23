#include "trees.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

inline size_t idx(const Raster& r, int row, int col) {
    return static_cast<size_t>(row) * r.w + col;
}

// Square max filter, separable so the cost is O(n*r) rather than O(n*r^2).
std::vector<float> max_filter(const std::vector<float>& a, int w, int h, int r) {
    std::vector<float> tmp(a.size());
    std::vector<float> out(a.size());
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            float m = -1e30f;
            for (int d = -r; d <= r; ++d) {
                const int c = col + d;
                if (c >= 0 && c < w) m = std::max(m, a[static_cast<size_t>(row) * w + c]);
            }
            tmp[static_cast<size_t>(row) * w + col] = m;
        }
    }
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            float m = -1e30f;
            for (int d = -r; d <= r; ++d) {
                const int rr = row + d;
                if (rr >= 0 && rr < h) m = std::max(m, tmp[static_cast<size_t>(rr) * w + col]);
            }
            out[static_cast<size_t>(row) * w + col] = m;
        }
    }
    return out;
}

constexpr int NBR8[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};

}  // namespace

std::vector<Tree> detect_trees(const Raster& chm, const Raster& landcover,
                               const Raster& terrain, const Bbox& padded) {
    const int w = chm.w;
    const int h = chm.h;
    const double px = chm.px;

    // 1. Mask to the WorldCover tree class and a minimum canopy height.
    std::vector<uint8_t> mask(chm.v.size(), 0);
    for (int row = 0; row < h; ++row) {
        const double y = chm.origin_y + (row + 0.5) * chm.py;
        for (int col = 0; col < w; ++col) {
            const double x = chm.origin_x + (col + 0.5) * chm.px;
            const size_t i = idx(chm, row, col);
            const bool inside = x >= padded.xmin && x <= padded.xmax &&
                                y >= padded.ymin && y <= padded.ymax;
            mask[i] = inside &&
                      (static_cast<int>(std::lround(sample_nearest(landcover, x, y))) ==
                       WORLDCOVER_TREE) &&
                      chm.v[i] >= TREE_H_MIN;
        }
    }

    // 2. Local maxima, each pixel tested against the window matching its own height.
    std::vector<int> want(chm.v.size());
    std::set<int> radii;
    for (size_t i = 0; i < chm.v.size(); ++i) {
        const int r = static_cast<int>(std::lround(WINDOW_K * chm.v[i] / px));
        want[i] = std::clamp(r, WINDOW_R_MIN_PX, WINDOW_R_MAX_PX);
        if (mask[i]) radii.insert(want[i]);
    }
    std::vector<uint8_t> is_max(chm.v.size(), 0);
    for (const int r : radii) {
        const std::vector<float> mf = max_filter(chm.v, w, h, r);
        for (size_t i = 0; i < chm.v.size(); ++i)
            if (mask[i] && want[i] == r && chm.v[i] >= mf[i]) is_max[i] = 1;
    }

    // 3. Canopy v2 is quantised to 1 m, so maxima arrive as tied plateaus rather than
    //    single pixels (measured at G3: ~66% of adjacent pixels equal). Group each
    //    plateau by 8-connectivity and keep one tree at its centroid.
    std::vector<int32_t> label(chm.v.size(), 0);
    std::vector<double> sx, sy, sh;
    std::vector<int> scount;
    std::vector<int> stack;
    int32_t next = 0;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const size_t seed = idx(chm, row, col);
            if (!is_max[seed] || label[seed] != 0) continue;
            ++next;
            double ax = 0, ay = 0, ah = 0;
            int n = 0;
            stack.assign(1, static_cast<int>(seed));
            label[seed] = next;
            while (!stack.empty()) {
                const int cur = stack.back();
                stack.pop_back();
                const int cr = cur / w;
                const int cc = cur % w;
                ax += cc; ay += cr; ah += chm.v[cur]; ++n;
                for (const auto& d : NBR8) {
                    const int rr = cr + d[0];
                    const int cc2 = cc + d[1];
                    if (rr < 0 || rr >= h || cc2 < 0 || cc2 >= w) continue;
                    const size_t j = idx(chm, rr, cc2);
                    if (is_max[j] && label[j] == 0) {
                        label[j] = next;
                        stack.push_back(static_cast<int>(j));
                    }
                }
            }
            sx.push_back(ax / n); sy.push_back(ay / n); sh.push_back(ah / n); scount.push_back(n);
        }
    }
    if (next == 0) return {};

    // 4. Marker-controlled watershed: flood the masked canopy from the plateaus,
    //    tallest pixel first, each taking the label of its tallest labelled neighbour.
    //    A radial descent cannot size crowns in closed canopy, where neighbouring
    //    crowns are as tall as the peak (NOTES/m3/README.md).
    std::vector<int> order;
    order.reserve(chm.v.size());
    for (size_t i = 0; i < chm.v.size(); ++i)
        if (mask[i] && label[i] == 0) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return chm.v[a] > chm.v[b]; });

    for (int pass = 0; pass < 64 && !order.empty(); ++pass) {
        std::vector<int> leftover;
        for (const int i : order) {
            const int cr = i / w;
            const int cc = i % w;
            int32_t best = 0;
            float bh = -1e30f;
            for (const auto& d : NBR8) {
                const int rr = cr + d[0];
                const int cc2 = cc + d[1];
                if (rr < 0 || rr >= h || cc2 < 0 || cc2 >= w) continue;
                const size_t j = idx(chm, rr, cc2);
                if (label[j] > 0 && chm.v[j] > bh) { best = label[j]; bh = chm.v[j]; }
            }
            if (best) label[i] = best; else leftover.push_back(i);
        }
        if (leftover.size() == order.size()) break;  // nothing assignable, stop
        order.swap(leftover);
    }

    // 5. Crown radius from segment area; ground elevation from the terrain raster.
    std::vector<int> area(static_cast<size_t>(next) + 1, 0);
    for (const int32_t l : label)
        if (l > 0) ++area[l];

    std::vector<Tree> trees;
    trees.reserve(next);
    for (int32_t t = 0; t < next; ++t) {
        Tree tr{};
        tr.x = chm.origin_x + (sx[t] + 0.5) * chm.px;
        tr.y = chm.origin_y + (sy[t] + 0.5) * chm.py;
        tr.height_m = sh[t];
        tr.ground_z = sample_bilinear(terrain, tr.x, tr.y);
        tr.crown_radius_m =
            std::max(std::sqrt(std::max(area[t + 1], 1) * px * px / M_PI), px);
        // Cap runaway watershed segments (G4). The tree is kept; only its crown extent
        // is bounded, so no canopy hole is opened.
        tr.crown_radius_m =
            std::min(tr.crown_radius_m, CROWN_RADIUS_MAX_RATIO * tr.height_m);
        trees.push_back(tr);
    }
    return trees;
}
