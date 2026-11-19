#pragma once

#include <algorithm>
#include <cmath>
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

inline std::optional<Quaternion> PalmRotation(V wrist, V index, V middle, V little) {
    V forward = middle - wrist;
    const double palmLength = Length(forward);
    if (!std::isfinite(palmLength) || palmLength < 0.02) return std::nullopt;
    forward = forward * (1.0 / palmLength);
    const V across = index - little;
    const V right = across - forward * Dot(across, forward);
    const double palmWidth = Length(right);
    if (!std::isfinite(palmWidth) || palmWidth < 0.02) return std::nullopt;
    // Compare the palm's across-knuckles axis with the zero-roll frame.
    // This axis has a different anatomical sign on opposing hands. The sign
    // cancels when computing rotation relative to the same hand at grab start.
    const double yaw = std::atan2(-forward.x, -forward.z);
    const V aimRight{std::cos(yaw), 0, -std::sin(yaw)};
    const V aimUp = Cross(forward * -1, aimRight);
    const double roll = std::atan2(Dot(right, aimUp), Dot(right, aimRight)) * 0.5;
    return AimRotation(forward) * Quaternion{std::cos(roll), 0, 0, std::sin(roll)};
}

inline Quaternion BlendRotation(Quaternion from, Quaternion to, double alpha) {
    // A quaternion and its negation are the same rotation. Blend along the
    // short path so crossing the sign boundary cannot spin the window.
    if (from.w*to.w + from.x*to.x + from.y*to.y + from.z*to.z < 0)
        to = {-to.w, -to.x, -to.y, -to.z};
    const Quaternion q{from.w + (to.w-from.w)*alpha, from.x + (to.x-from.x)*alpha,
                       from.y + (to.y-from.y)*alpha, from.z + (to.z-from.z)*alpha};
    const double n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    return {q.w/n, q.x/n, q.y/n, q.z/n};
}

class GrabRotation {
public:
    GrabRotation(Quaternion pointer, std::optional<Quaternion> palm) : rotation_(pointer) {
        if (palm) pointerInPalm_ = palm->Conjugate() * pointer;
    }

    Quaternion Step(std::optional<Quaternion> palm, double alpha) {
        if (palm) {
            if (!pointerInPalm_) pointerInPalm_ = palm->Conjugate() * rotation_;
            rotation_ = BlendRotation(rotation_, *palm * *pointerInPalm_, alpha);
        }
        return rotation_;
    }

private:
    Quaternion rotation_;
    std::optional<Quaternion> pointerInPalm_;
};

}  // namespace pointer_pose
