#include "../helper/pointer_rotation.h"

#include <cassert>
#include <iostream>

using namespace pointer_pose;

static Quaternion Roll(double radians) {
    return {std::cos(radians / 2), 0, 0, std::sin(radians / 2)};
}

int main() {
    const Quaternion neutral = Roll(0);
    const PointerPose start{{0, 0, 0}, neutral};
    const double frameTime = 0.012;
    GrabPose grab(start, neutral);

    // Alternating samples simulate jitter while holding a stationary grab.
    double peakShift = 0, peakTilt = 0;
    for (int frame = 0; frame < 60; ++frame) {
        const double sign = frame % 2 ? -1 : 1;
        const auto pose = grab.Step({{sign * 0.008, 0, 0}, neutral}, Roll(sign * 0.05), frameTime);
        peakShift = std::max(peakShift, Length(pose.origin));
        peakTilt = std::max(peakTilt, RotationDistance(neutral, pose.rotation));
    }
    assert(peakShift < 0.002);
    assert(peakTilt < 0.012);

    // A deliberate move and tilt remain responsive and reach the target.
    const PointerPose target{{0.25, 0.12, -0.05}, AimRotation({0.36, 0.48, -0.8})};
    const Quaternion tilt = Roll(0.65);
    const Quaternion finalRotation = tilt * target.rotation;
    for (int frame = 0; frame < 35; ++frame)
        grab.Step(target, tilt, frameTime);
    assert(Length(grab.Pose().origin - target.origin) < 0.01);
    assert(RotationDistance(grab.Pose().rotation, finalRotation) < 0.02);

    // Mouse-up can be followed immediately by upright hover or tracking loss.
    // The drag's final pose must survive SteamVR's deferred read.
    const PointerPose last = grab.Pose();
    const int64_t mouseUp = 1'000'000'000;
    grab.Release(mouseUp);
    assert(grab.HoldingRelease(mouseUp + 150'000'000));
    const auto frozen = grab.Step(start, std::nullopt, frameTime);
    assert(Length(frozen.origin - last.origin) == 0);
    assert(RotationDistance(frozen.rotation, last.rotation) < 1e-6);
    assert(!grab.HoldingRelease(mouseUp + 250'000'000));

    GrabPose tap(start, neutral);
    tap.Release(mouseUp);
    assert(!tap.HoldingRelease(mouseUp));
    std::cout << "Grab smoothing and release pose checks passed\n";
}
