#include "../helper/gaze_pointer.h"

#include <cassert>
#include <iostream>

using namespace pointer_pose;

static Quaternion Roll(double radians) {
    return {std::cos(radians / 2), 0, 0, std::sin(radians / 2)};
}

static Quaternion Pitch(double radians) {
    return {std::cos(radians / 2), std::sin(radians / 2), 0, 0};
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
    GrabPose grab(start, {handStart, {}, neutral, begin, true}, begin);
    for (int tick = 1; tick <= 180; ++tick) {
        const int frame = tick / cameraTicks;
        const int64_t capture = begin + frame * cameraTicks * tickNs;
        const V moved{0.002 * frame * cameraTicks, 0, 0};
        const auto pose = grab.Step({handStart + moved, {}, Roll(frame % 2 ? 0.052 : -0.052),
                                     capture, true}, begin + tick * tickNs);
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
    GrabPose grab(start, {handStart, {}, neutral, begin, true}, begin);

    // Pan and tilt together. The panel should respond to each pose and
    // continue closing the gap during the next tick of the same camera frame.
    const V movement{0.25, 0.12, -0.05};
    const Quaternion tilt = Roll(0.75);
    for (int frame = 1; frame <= 6; ++frame) {
        const double progress = frame / 6.0;
        const int64_t capture = begin + frame * frameNs;
        const auto first = grab.Step({handStart + movement * progress, {}, Roll(0.75 * progress),
                                      capture, true}, capture);
        const auto next = grab.Step({handStart, {}, std::nullopt, capture, true}, capture + 11'000'000);
        assert(next.origin.x > first.origin.x);
    }
    assert(grab.Pose().origin.x > 0.20);
    assert(grab.WristAngle() > 0.55);
    const int64_t lastCapture = begin + 6 * frameNs;
    for (int tick = 2; tick <= 14; ++tick)
        grab.Step({handStart + movement, {}, tilt, lastCapture, true}, lastCapture + tick * 11'000'000);
    const auto target = grab.Pose();
    assert(Length(target.origin - movement) < 0.003);
    assert(RotationDistance(target.rotation, tilt) < 0.025);

    // Mouse-up can be followed immediately by upright hover or tracking loss.
    // The drag's final pose must survive SteamVR's deferred read.
    const int64_t mouseUp = lastCapture + 155'000'000;
    grab.Release(mouseUp);
    assert(grab.HoldingRelease(mouseUp + 150'000'000));
    const auto frozen = grab.Step({handStart, {}, std::nullopt, mouseUp, true}, mouseUp + frameNs);
    assert(Length(frozen.origin - target.origin) == 0);
    assert(RotationDistance(frozen.rotation, target.rotation) < 1e-6);
    assert(!grab.HoldingRelease(mouseUp + 250'000'000));

    // The laser still sees the new aim while the window sees its frozen pose.
    CheckLaser(frozen, start);
    CheckLaser(frozen, {{0.4, -0.1, 0.3}, AimRotation({0.48, -0.36, -0.8})});
    CheckLaser(target, target);

    GrabPose tap(start, {handStart, {}, neutral, begin, true}, begin);
    tap.Step({handStart + V{0.002, 0, 0}, {}, Roll(0.006), begin + frameNs, true}, begin + frameNs);
    tap.Release(mouseUp);
    assert(!tap.HoldingRelease(mouseUp));
}

static void CheckGrabReacquisition() {
    const PointerPose start{{}, Roll(0)};
    const int64_t frameNs = 33'000'000;
    int64_t now = 1'000'000'000;
    GrabSample sample{{0, 0, -0.52}, {}, Roll(0), now, true};
    GrabPose grab(start, sample, now);
    const auto step = [&](int64_t interval = 33'000'000) {
        now += interval;
        sample.captureNs = now;
        return grab.Step(sample, now);
    };
    const V panelOffset{0, 0.3, -1.8};
    const auto checkStationary = [&](PointerPose pose) {
        assert(Length(pose.origin + Rotate(pose.rotation, panelOffset) - panelOffset) < 1e-6);
        assert(grab.WristAngle() < 1e-6);
    };
    const V originalHand = sample.knuckle;

    // A single bad frame should not displace the window, even if the helper
    // publishes multiple poses from that camera sample.
    sample.knuckle = {0.20, 0, -0.84};
    sample.wrist = Pitch(1.2);
    checkStationary(step());
    now += 11'000'000;
    checkStationary(grab.Step(sample, now));
    sample.knuckle = originalHand;
    sample.wrist = Quaternion{-1, 0, 0, 0};
    checkStationary(step(22'000'000));

    // A persistent shifted fit is a new baseline. Its difference from the
    // old fit is not a hand movement, even after two agreeing frames.
    sample.knuckle = {0, 0, -0.85};
    sample.wrist = Pitch(1.2);
    for (int frame = 0; frame < 4; ++frame) checkStationary(step());

    // Camera source changes and bad mono range estimates cannot move the
    // window forward/back, even when the hand has the same ID throughout.
    sample.stereo = false;
    sample.knuckle.z = -0.65;
    sample.wrist = Roll(0);
    checkStationary(step());
    for (double range : {0.82, 0.43, 0.65}) {
        sample.knuckle.z = -range;
        checkStationary(step());
    }
    sample.stereo = true;
    sample.knuckle.z = -0.50;
    sample.wrist.reset();
    checkStationary(step(frameNs * 6));
    sample.wrist = Pitch(-0.9);
    checkStationary(step());

    // Deliberate stereo depth motion and pitch still work after rebasing.
    for (int frame = 1; frame <= 8; ++frame) {
        sample.knuckle.z -= 0.016;
        sample.wrist = Pitch(-0.9 + 0.07 * frame);
        step();
    }
    for (int frame = 0; frame < 20; ++frame) step();
    const auto moved = grab.Pose();
    assert(std::abs(moved.origin.z + 0.128) < 0.003);
    assert(RotationDistance(moved.rotation, Pitch(0.56)) < 0.025);
    assert(grab.HasDragged());

    // A fit switch preserves the existing tilt. A turn around a different
    // axis then applies relative to that tilt, including in mono.
    sample.stereo = false;
    sample.knuckle = {0, 0, -0.52};
    sample.wrist = Roll(0.8);
    for (int frame = 0; frame < 12; ++frame) step();
    assert(Length(grab.Pose().origin - moved.origin) < 0.003);
    assert(RotationDistance(grab.Pose().rotation, moved.rotation) < 0.003);
    for (int frame = 1; frame <= 8; ++frame) {
        sample.knuckle.x += 0.016;
        sample.wrist = Roll(0.8 + 0.07 * frame);
        step();
    }
    for (int frame = 0; frame < 20; ++frame) step();
    assert(grab.Pose().origin.x > 0.12);
    assert(RotationDistance(grab.Pose().rotation, Roll(0.56) * Pitch(0.56)) < 0.025);
}

static void CheckGaze() {
    const auto first = HeadGazeRay({0, 1.6, 0}, {0, 0, -1}).value();
    const auto moved = HeadGazeRay({0.02, 1.6, 0}, {0.4, 0, -0.8}).value();
    assert(!HeadGazeRay({}, {}));
    assert(Length(Rotate(AimRotation(moved.direction), {0, 0, -1}) - moved.direction) < 1e-8);
    GazeClick gaze;
    GazeHand hands[2]{};
    int64_t now = 1'000'000'000;

    // Hover follows the head before any hand is available.
    auto result = gaze.Step(first, hands, false, now);
    assert(!AimMoved(first, result.ray) && result.button == GazeButton::None);
    // A quick pinch can start and finish between polls. Queue the click on
    // the displayed target while SteamVR is still claiming the cursor.
    now += 11'000'000;
    hands[1] = {1, 12, now, true, false};
    result = gaze.Step(moved, hands, false, now);
    assert(!AimMoved(first, result.ray) && result.button == GazeButton::None);
    now += 33'000'000;
    result = gaze.Step(moved, hands, true, now);
    assert(!AimMoved(first, result.ray) && result.button == GazeButton::Down);
    result = gaze.Step(moved, hands, true, now + 33'000'000);
    assert(gaze.Pressed() && result.button == GazeButton::None);
    now += 55'000'000;
    result = gaze.Step(moved, hands, true, now);
    assert(!gaze.Pressed() && !AimMoved(first, result.ray) && result.button == GazeButton::Up);
    now += 60'000'000;
    result = gaze.Step(moved, hands, true, now);
    assert(!AimMoved(moved, result.ray) && result.button == GazeButton::None);

    // An old or already-held pinch cannot click when gaze takes control.
    gaze.Reset();
    hands[0] = {9, 10, now, true, true};
    assert(gaze.Step(moved, hands, true, now).button == GazeButton::None);
    hands[1] = {2, 12, now - 500'000'000, true, true};
    assert(gaze.Step(moved, hands, true, now + 12'000'000).button == GazeButton::None);
    // Either hand can click. Losing the pressed hand releases the button.
    now += 24'000'000;
    hands[0] = {10, 10, now, true, true};
    assert(gaze.Step(moved, hands, true, now).button == GazeButton::Down);
    hands[0].eligible = false;
    assert(gaze.Step(first, hands, true, now + 60'000'000).button == GazeButton::Up);
}

int main() {
    CheckAim();
    CheckGrabJitter(3);  // 30 Hz cameras
    CheckGrabJitter(6);  // 15 Hz cameras
    CheckGrab();
    CheckGrabReacquisition();
    CheckGaze();
    std::cout << "Pointer aim, window grabs, gaze hover, and pinch click checks passed\n";
}
