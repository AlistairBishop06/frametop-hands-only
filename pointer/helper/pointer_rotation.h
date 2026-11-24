#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace pointer_pose {

struct V { double x, y, z; };
inline V operator-(V a, V b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline V operator+(V a, V b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline V operator*(V a, double k) { return {a.x*k, a.y*k, a.z*k}; }
inline double Dot(V a, V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline double Length(V a) { return std::sqrt(Dot(a, a)); }
inline V Cross(V a, V b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}

struct Quaternion {
    double w, x, y, z;
    Quaternion Conjugate() const { return {w, -x, -y, -z}; }
};

inline double QuaternionDot(Quaternion a, Quaternion b) {
    return a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
}

inline Quaternion operator*(Quaternion a, Quaternion b) {
    return {a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w};
}

inline Quaternion AimRotation(V direction) {
    // SteamVR's laser points down the device's local -Z axis.
    const double yaw = std::atan2(-direction.x, -direction.z) * 0.5;
    const double pitch = std::asin(std::clamp(direction.y, -1.0, 1.0)) * 0.5;
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    return {cy*cp, cy*sp, sy*cp, -sy*sp};
}

inline std::optional<Quaternion> WristRotation(V forward, V across) {
    const double palmLength = Length(forward);
    if (!std::isfinite(palmLength) || palmLength < 0.02) return std::nullopt;
    forward = forward * (1.0 / palmLength);
    const V right = across - forward * Dot(across, forward);
    const double palmWidth = Length(right);
    if (!std::isfinite(palmWidth) || palmWidth < 0.02) return std::nullopt;
    // Compare the wrist's right axis with the zero-roll aiming frame.
    const double yaw = std::atan2(-forward.x, -forward.z);
    const V aimRight{std::cos(yaw), 0, -std::sin(yaw)};
    const V aimUp = Cross(forward * -1, aimRight);
    const double roll = std::atan2(Dot(right, aimUp), Dot(right, aimRight)) * 0.5;
    return AimRotation(forward) * Quaternion{std::cos(roll), 0, 0, std::sin(roll)};
}

inline double RotationDistance(Quaternion a, Quaternion b) {
    return 2 * std::acos(std::clamp(std::abs(QuaternionDot(a, b)), 0.0, 1.0));
}

inline Quaternion BlendRotation(Quaternion from, Quaternion to, double fraction) {
    // Use the same hemisphere so a sign change never makes the panel spin.
    if (QuaternionDot(from, to) < 0) to = {-to.w, -to.x, -to.y, -to.z};
    const Quaternion q{from.w + (to.w - from.w) * fraction, from.x + (to.x - from.x) * fraction,
                       from.y + (to.y - from.y) * fraction, from.z + (to.z - from.z) * fraction};
    const double inverse = 1 / std::sqrt(QuaternionDot(q, q));
    return {q.w * inverse, q.x * inverse, q.y * inverse, q.z * inverse};
}

struct PointerPose {
    V origin;
    Quaternion rotation;
};

inline V Rotate(Quaternion q, V v) {
    const Quaternion result = q * Quaternion{0, v.x, v.y, v.z} * q.Conjugate();
    return {result.x, result.y, result.z};
}

// OpenVR pose components are offsets from the tracked device's pose.
inline PointerPose RelativePose(PointerPose device, PointerPose laser) {
    const Quaternion inverse = device.rotation.Conjugate();
    return {Rotate(inverse, laser.origin - device.origin), inverse * laser.rotation};
}

class GrabPose {
public:
    GrabPose(PointerPose start, V knuckle, std::optional<Quaternion> wrist, int64_t captureNs, int64_t now)
        : start_(start), pose_(start), knuckleStart_(knuckle), targetOrigin_(start.origin),
          wristStart_(wrist), lastWrist_(wrist), lastSampleNs_(captureNs),
          lastWristNs_(captureNs), lastStepNs_(now) {}

    PointerPose Step(V knuckle, std::optional<Quaternion> wrist, int64_t captureNs, int64_t now) {
        if (releaseUntil_) return pose_;
        if (captureNs > lastSampleNs_) {
            // Pan with the hand and turn with the wrist independently of aim.
            targetOrigin_ = start_.origin + (knuckle - knuckleStart_);
            if (wrist) UpdateWrist(*wrist, captureNs);
            else {
                angularVelocity_ = {};
                suspectWrist_.reset();
            }
            lastSampleNs_ = captureNs;
        }
        // Advance the panel between camera updates. Estimate wrist speed from
        // fresh samples only, so polling a held frame cannot amplify jitter.
        const double dt = std::clamp((now - lastStepNs_) / 1e9, 0.0, 0.04);
        lastStepNs_ = now;
        const V offset = targetOrigin_ - pose_.origin;
        const double positionHz = std::min(5.0 + 15.0 * Length(offset), 9.0);
        pose_.origin = pose_.origin + offset * Alpha(positionHz, dt);

        const Quaternion target = wristDelta_ * start_.rotation;
        const double angle = RotationDistance(pose_.rotation, target);
        constexpr double kRestAngle = 0.012;  // about 0.7 degrees
        if (angle > kRestAngle) {
            const double turnSpeed = std::max(0.0, Length(angularVelocity_) - 0.65);
            const double rotationHz = std::min(1.35 + 3.5 * turnSpeed, 10.0);
            const double step = std::min(Alpha(rotationHz, dt) * (angle - kRestAngle), 9.0 * dt);
            pose_.rotation = BlendRotation(pose_.rotation, target, step / angle);
        }
        hasDragged_ = hasDragged_ || Length(pose_.origin - start_.origin) > 0.006 ||
                      RotationDistance(pose_.rotation, start_.rotation) > 0.017;
        return pose_;
    }

    void Release(int64_t now) {
        // SteamVR can wait 150 ms after mouse-up before it samples the
        // controller to place the window. Leave room for event delivery.
        if (hasDragged_) releaseUntil_ = now + 250'000'000;
    }
    bool Released() const { return releaseUntil_ != 0; }
    bool HoldingRelease(int64_t now) const { return Released() && now < releaseUntil_; }
    const PointerPose &Pose() const { return pose_; }
    double WristAngle() const { return RotationDistance(start_.rotation, pose_.rotation); }

private:
    static double Alpha(double hz, double dt) { return -std::expm1(-6.283185307179586 * hz * dt); }

    void UpdateWrist(Quaternion wrist, int64_t captureNs) {
        if (!wristStart_) wristStart_ = wrist;
        if (lastWrist_) {
            const double dt = std::clamp((captureNs - lastWristNs_) / 1e9, 0.001, 0.10);
            const double angle = RotationDistance(*lastWrist_, wrist);
            // Require a second agreeing sample for a sudden wrist jump. This
            // rejects a one-frame tracking flip without discarding real turns.
            if (angle > 0.40 + 7.0 * dt &&
                (!suspectWrist_ || RotationDistance(*suspectWrist_, wrist) > 0.30)) {
                suspectWrist_ = wrist;
                angularVelocity_ = {};
                return;
            }
            Quaternion change = wrist * lastWrist_->Conjugate();
            if (change.w < 0) change = {-change.w, -change.x, -change.y, -change.z};
            const V axis{change.x, change.y, change.z};
            const double length = Length(axis);
            const V velocity = length > 1e-8 ? axis * (angle / (length * dt)) : V{};
            // A signed rate cancels alternating jitter instead of classing
            // each reversal as a fast, deliberate turn.
            angularVelocity_ = angularVelocity_ + (velocity - angularVelocity_) * Alpha(2.0, dt);
        }
        suspectWrist_.reset();
        lastWrist_ = wrist;
        lastWristNs_ = captureNs;
        wristDelta_ = wrist * wristStart_->Conjugate();
    }

    PointerPose start_, pose_;
    V knuckleStart_, targetOrigin_, angularVelocity_{};
    Quaternion wristDelta_{1, 0, 0, 0};
    std::optional<Quaternion> wristStart_;
    std::optional<Quaternion> lastWrist_, suspectWrist_;
    int64_t lastSampleNs_, lastWristNs_, lastStepNs_;
    bool hasDragged_ = false;
    int64_t releaseUntil_ = 0;
};

}  // namespace pointer_pose
