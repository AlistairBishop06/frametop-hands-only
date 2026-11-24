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
    const double dot = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    return 2 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0));
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
    GrabPose(PointerPose start, V knuckle, std::optional<Quaternion> wrist)
        : start_(start), pose_(start), knuckleStart_(knuckle), wristStart_(wrist) {}

    PointerPose Step(V knuckle, std::optional<Quaternion> wrist) {
        if (releaseUntil_) return pose_;
        if (wrist) {
            if (!wristStart_) wristStart_ = wrist;
            wristDelta_ = *wrist * wristStart_->Conjugate();
        }
        // A grab behaves like a controller: translate with the hand and
        // rotate with the wrist. Changes in laser aim do not steer it.
        pose_ = {start_.origin + (knuckle - knuckleStart_), wristDelta_ * start_.rotation};
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
    double WristAngle() const { return RotationDistance({1, 0, 0, 0}, wristDelta_); }

private:
    PointerPose start_, pose_;
    V knuckleStart_;
    Quaternion wristDelta_{1, 0, 0, 0};
    std::optional<Quaternion> wristStart_;
    bool hasDragged_ = false;
    int64_t releaseUntil_ = 0;
};

}  // namespace pointer_pose
