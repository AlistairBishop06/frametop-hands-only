#include "../track/frame_pair.h"

#include <cassert>

int main() {
    using fh_frames::Stamp;
    using Slots = std::array<Stamp, 4>;
    constexpr uint64_t ms = 1'000'000;
    const Slots a{{{9, 99 * ms}, {8, 66 * ms}, {7, 33 * ms}}};
    const Slots b{{{8, 68 * ms}, {7, 33 * ms}}};

    // The camera that is one frame behind still has a recent valid match.
    // The exact timestamp match one tick earlier must not win.
    const auto latest = fh_frames::latest_pair(a, b);
    assert(latest[0].frame == 8 && latest[1].frame == 8);

    const Slots none{{{8, 70 * ms}}};
    assert(!fh_frames::latest_pair(a, none)[0].frame);
    const Slots boundary{{{8, 69 * ms}}};
    assert(fh_frames::latest_pair(a, boundary)[0].frame == 8);
    assert(!fh_frames::latest_pair(a, Slots{})[0].frame);
}
