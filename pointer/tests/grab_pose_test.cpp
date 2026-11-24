#include "../helper/pointer_aim.h"

#include <cassert>
#include <iostream>

using namespace pointer_pose;

static Quaternion Roll(double radians) {
    return {std::cos(radians / 2), 0, 0, std::sin(radians / 2)};
}

static void CheckLaser(PointerPose device, PointerPose laser) {
    const auto offset = RelativePose(device, laser);
    const auto origin = device.origin + Rotate(device.rotation, offset.origin);
    const auto rotation = device.rotation * offset.rotation;
    assert(Length(origin - laser.origin) < 1e-8);
    assert(RotationDistance(rotation, laser.rotation) < 1e-6);
}

static void CheckAim() {
    HandAim aim;
    const V stillHand{0.17, -0.16, -0.52};
    const V headRight{1, 0, 0};
    int64_t ns = 1'000'000'000;
    const auto initial = aim.Step(stillHand, {}, headRight, true, 1, true, ns, ns, 50).value();
    // A head turn and small head translation should leave a stationary room-space hand aimed.
    ns += 33'000'000;
    const auto headTurn = aim.Step(stillHand, {0.03, 0.02, -0.05}, {0, 0, 1}, true, 1, true, ns, ns, 50).value();
    assert(Length(headTurn.origin - initial.origin) < 1e-8);
    assert(Length(headTurn.direction - initial.direction) < 1e-8);

    double peakOrigin = 0, peakDirection = 0;
    for (int i = 0; i < 50; ++i) {
        ns += 33'000'000;
        const auto jitter = V{i % 2 ? 0.006 : -0.006, 0, 0};
        const auto ray = aim.Step(stillHand + jitter, {}, headRight, true, 1, true, ns, ns, 50).value();
        if (i >= 10) {
            peakOrigin = std::max(peakOrigin, Length(ray.origin - initial.origin));
            peakDirection = std::max(peakDirection, Length(ray.direction - initial.direction));
            assert(!AimMoved(initial, ray));
        }
    }
    assert(peakOrigin < 0.003);
    assert(peakDirection < 0.006);

    const V offset{0.12, 0.03, 0};
    aim.Step(stillHand, {}, headRight, true, 1, true, ns, ns + 21'000'000, 50);
    ns += 33'000'000;
    const auto sweep = aim.Step(stillHand + offset, {}, headRight, true, 1, true, ns, ns, 50).value();
    assert(sweep.origin.x - initial.origin.x > 0.10);
    assert(AimMoved(initial, sweep));
    // Between camera frames the aim still advances toward the last sample.
    // Polling the same frame cannot introduce a new target or new velocity.
    const auto repeated = aim.Step(stillHand, {}, headRight, true, 1, true, ns, ns + 12'000'000, 50).value();
    assert(repeated.origin.x > sweep.origin.x);
    assert(repeated.origin.x - initial.origin.x < 0.13);

    ns += 33'000'000;
    const auto refound = aim.Step(stillHand + offset + V{0.04, 0, 0}, {}, headRight, true, 2, false, ns, ns, 50).value();
    assert(Length(refound.origin - repeated.origin) < 0.017);
    assert(Length(refound.direction - repeated.direction) < 0.035);
}

int main() {
    CheckAim();
    const Quaternion neutral = Roll(0);
    const PointerPose start{{0, 0, 0}, neutral};
    const V handStart{0.17, -0.16, -0.52};
    GrabPose grab(start, handStart, neutral);

    // The grab follows its room-space hand displacement and wrist with no filter.
    const V movement{0.25, 0.12, -0.05};
    const Quaternion tilt = Roll(0.65);
    const auto target = grab.Step(handStart + movement, tilt);
    assert(Length(target.origin - movement) < 1e-8);
    assert(RotationDistance(target.rotation, tilt) < 1e-6);

    // Mouse-up can be followed immediately by upright hover or tracking loss.
    // The drag's final pose must survive SteamVR's deferred read.
    const PointerPose last = grab.Pose();
    const int64_t mouseUp = 1'000'000'000;
    grab.Release(mouseUp);
    assert(grab.HoldingRelease(mouseUp + 150'000'000));
    const auto frozen = grab.Step(handStart, std::nullopt);
    assert(Length(frozen.origin - last.origin) == 0);
    assert(RotationDistance(frozen.rotation, last.rotation) < 1e-6);
    assert(!grab.HoldingRelease(mouseUp + 250'000'000));

    // The laser still sees the new aim while the window sees its frozen pose.
    CheckLaser(frozen, start);
    CheckLaser(frozen, {{0.4, -0.1, 0.3}, AimRotation({0.48, -0.36, -0.8})});
    CheckLaser(target, target);

    GrabPose tap(start, handStart, neutral);
    tap.Step(handStart + V{0.002, 0, 0}, Roll(0.006));
    tap.Release(mouseUp);
    assert(!tap.HoldingRelease(mouseUp));
    std::cout << "Head-stable aim, jitter, sweeps, unfiltered grabs, and release checks passed\n";
}
