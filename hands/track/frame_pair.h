#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace fh_frames {
constexpr uint64_t kMaxSkewNs = 3'000'000;

struct Stamp {
    uint64_t frame = 0, capture = 0, dequeue = 0;
};

// Choose the most recent complete pair within the stereo tolerance. Choosing
// the smallest skew first can pick a frame several camera ticks behind.
template<std::size_t N>
std::array<Stamp, 2> latest_pair(const std::array<Stamp, N> &a, const std::array<Stamp, N> &b) {
    std::array<Stamp, 2> best{};
    uint64_t newest = 0, skew = kMaxSkewNs + 1;
    for (const Stamp &l : a) {
        if (!l.frame) continue;
        for (const Stamp &r : b) {
            if (!r.frame) continue;
            const uint64_t first = std::min(l.capture, r.capture);
            const uint64_t gap = std::max(l.capture, r.capture) - first;
            if (gap <= kMaxSkewNs && (first > newest || (first == newest && gap < skew))) {
                best = {l, r};
                newest = first;
                skew = gap;
            }
        }
    }
    return best;
}
}  // namespace fh_frames
