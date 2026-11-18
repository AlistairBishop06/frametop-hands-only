#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fh_crop {

constexpr int kTiles = 4;

// CLAHE on four by four tiles. The interpolation coordinates are fixed for
// each model input size. Build all histograms before changing any pixels.
template<int N>
void clahe(uint8_t *img, double clip_limit) {
    constexpr int ts = N / kTiles, area = ts * ts;
    static_assert(N % kTiles == 0);
    struct Blend { int lo, hi; float w, inv; };
    static const auto axis = [] {
        std::array<Blend, N> out{};
        for (int i = 0; i < N; ++i) {
            const float f = (i + 0.5f) / (N / kTiles) - 0.5f;
            const int lo = std::clamp(int(std::floor(f)), 0, kTiles - 1);
            const float w = std::clamp(f - lo, 0.f, 1.f);
            out[i] = {lo, std::min(lo + 1, kTiles - 1), w, 1 - w};
        }
        return out;
    }();
    const int clip = std::max(1, int(clip_limit * area / 256));
    uint8_t lut[kTiles][kTiles][256];
    for (int ty = 0; ty < kTiles; ++ty)
        for (int tx = 0; tx < kTiles; ++tx) {
            int hist[256] = {};
            for (int y = ty * ts; y < (ty + 1) * ts; ++y)
                for (int x = tx * ts; x < (tx + 1) * ts; ++x) ++hist[img[y * N + x]];
            int excess = 0;
            for (int &h : hist)
                if (h > clip) excess += h - clip, h = clip;
            const int add = excess / 256, residual = excess - add * 256;
            int sum = 0;
            constexpr float scale = 255.f / area;
            for (int i = 0; i < 256; ++i) {
                sum += hist[i] + add + (i < residual ? 1 : 0);
                lut[ty][tx][i] = uint8_t(std::min(255, int(sum * scale + 0.5f)));
            }
        }

    for (int y = 0; y < N; ++y) {
        const Blend &b = axis[y];
        const auto &top = lut[b.lo], &bottom = lut[b.hi];
        uint8_t *row = img + y * N;
        for (int x = 0; x < N; ++x) {
            const Blend &a = axis[x];
            const uint8_t v = row[x];
            const float t = top[a.lo][v] * a.inv + top[a.hi][v] * a.w;
            const float u = bottom[a.lo][v] * a.inv + bottom[a.hi][v] * a.w;
            row[x] = uint8_t(t * b.inv + u * b.w + 0.5f);
        }
    }
}
}  // namespace fh_crop
