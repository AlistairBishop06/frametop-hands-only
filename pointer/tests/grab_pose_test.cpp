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

static void CheckGrabJitter(int cameraTicks) {
    const Quaternion neutral = Roll(0);
    const PointerPose start{{0, 0, 0}, neutral};
    const V handStart{0.17, -0.16, -0.52};
    const int64_t begin = 1'000'000'000, tickNs = 11'000'000;
    GrabPose grab(start, handStart, neutral, begin, begin);
    for (int tick = 1; tick <= 180; ++tick) {
        const int frame = tick / cameraTicks;
        const int64_t capture = begin + frame * cameraTicks * tickNs;
        const V moved{0.002 * frame * cameraTicks, 0, 0};
        const auto pose = grab.Step(handStart + moved, Roll(frame % 2 ? 0.052 : -0.052),
                                    capture, begin + tick * tickNs);
        // The hand is translating and the wrist estimate alternates by
        // about three degrees. A metre-away panel should barely wobble.
        assert(RotationDistance(pose.rotation, neutral) < 0.019);
    }
    assert(grab.Pose().origin.x > 0.34);
}

static void CheckGrab() {
    const Quaternion neutral = Roll(0);
    const PointerPose start{{0, 0, 0}, neutral};
    const V handStart{0.17, -0.16, -0.52};
    const int64_t begin = 1'000'000'000, frameNs = 33'000'000;
    GrabPose grab(start, handStart, neutral, begin, begin);

    // Pan and tilt together. The panel should respond to each pose and
    // continue closing the gap during the next tick of the same camera frame.
    const V movement{0.25, 0.12, -0.05};
    const Quaternion tilt = Roll(0.75);
    for (int frame = 1; frame <= 6; ++frame) {
        const double progress = frame / 6.0;
        const int64_t capture = begin + frame * frameNs;
        const auto first = grab.Step(handStart + movement * progress, Roll(0.75 * progress),
                                     capture, capture);
        const auto next = grab.Step(handStart, std::nullopt, capture, capture + 11'000'000);
        assert(next.origin.x > first.origin.x);
    }
    assert(grab.Pose().origin.x > 0.20);
    assert(grab.WristAngle() > 0.55);
    const int64_t lastCapture = begin + 6 * frameNs;
    for (int tick = 2; tick <= 14; ++tick)
        grab.Step(handStart + movement, tilt, lastCapture, lastCapture + tick * 11'000'000);
    const auto target = grab.Pose();
    assert(Length(target.origin - movement) < 0.003);
    assert(RotationDistance(target.rotation, tilt) < 0.025);

    // Mouse-up can be followed immediately by upright hover or tracking loss.
    // The drag's final pose must survive SteamVR's deferred read.
    const int64_t mouseUp = lastCapture + 155'000'000;
    grab.Release(mouseUp);
    assert(grab.HoldingRelease(mouseUp + 150'000'000));
    const auto frozen = grab.Step(handStart, std::nullopt, mouseUp, mouseUp + frameNs);
    assert(Length(frozen.origin - target.origin) == 0);
    assert(RotationDistance(frozen.rotation, target.rotation) < 1e-6);
    assert(!grab.HoldingRelease(mouseUp + 250'000'000));

    // The laser still sees the new aim while the window sees its frozen pose.
    CheckLaser(frozen, start);
    CheckLaser(frozen, {{0.4, -0.1, 0.3}, AimRotation({0.48, -0.36, -0.8})});
    CheckLaser(target, target);

    GrabPose tap(start, handStart, neutral, begin, begin);
    tap.Step(handStart + V{0.002, 0, 0}, Roll(0.006), begin + frameNs, begin + frameNs);
    tap.Release(mouseUp);
    assert(!tap.HoldingRelease(mouseUp));

    // A single bad fit or a missing fit must not replace the accepted pose.
    GrabPose glitch(start, handStart, neutral, begin, begin);
    glitch.Step(handStart, Roll(1.25), begin + frameNs, begin + frameNs);
    assert(glitch.WristAngle() < 1e-6);
    glitch.Step(handStart, std::nullopt, begin + 2 * frameNs, begin + 2 * frameNs);
    glitch.Step(handStart, Quaternion{-neutral.w, 0, 0, 0}, begin + 3 * frameNs, begin + 3 * frameNs);
    assert(glitch.WristAngle() < 1e-6);
}

int main() {
    CheckAim();
    CheckGrabJitter(3);  // 30 Hz cameras
    CheckGrabJitter(6);  // 15 Hz cameras
    CheckGrab();
    std::cout << "Pointer aim, window jitter, deliberate turns, and release checks passed\n";
}
