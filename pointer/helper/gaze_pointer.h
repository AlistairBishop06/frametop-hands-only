#pragma once

#include "pointer_aim.h"

namespace pointer_pose {

inline std::optional<AimRay> HeadGazeRay(V head, V forward) {
    const double length = Length(forward);
    if (!std::isfinite(Length(head)) || !std::isfinite(length) || length < 0.5) return std::nullopt;
    return AimRay{head, forward * (1.0 / length)};
}

inline std::optional<double> GazeCursorDepth(AimRay ray, V cursor) {
    const V delta = cursor - ray.origin;
    const double depth = Dot(delta, ray.direction);
    // The compositor's cursor can be from a preceding frame. Use it only
    // when it is still on this sightline and in front of the headset.
    if (!std::isfinite(depth) || depth <= 0 || Length(delta - ray.direction * depth) > 0.006)
        return std::nullopt;
    return depth;
}

inline constexpr double kGazeSearchDistance = 2.0;
inline constexpr double kGazeSurfaceClearance = 0.025;

inline double GazeRearClearance(double hitDistance) {
    if (!(hitDistance > 0 && hitDistance <= kGazeSearchDistance)) return kGazeSearchDistance;
    return std::max(0.0, kGazeSearchDistance - hitDistance - kGazeSurfaceClearance);
}

inline AimRay GazeScreenRay(AimRay ray, std::optional<double> surfaceDepth,
                           double searchDistance = kGazeSearchDistance) {
    // Start just in front of the surface so SteamVR can still intersect it.
    // In empty space, move the origin behind the head. SteamVR can still
    // find the next target along this line, but its idle beam is out of view.
    const double advance = surfaceDepth && std::isfinite(*surfaceDepth) ?
        std::max(0.0, *surfaceDepth - kGazeSurfaceClearance) : -searchDistance;
    ray.origin = ray.origin + ray.direction * advance;
    return ray;
}

inline AimRay BlendGazeRay(AimRay from, AimRay to, double fraction) {
    const V direction = from.direction + (to.direction - from.direction) * fraction;
    const double length = Length(direction);
    return {from.origin + (to.origin - from.origin) * fraction,
            length > 1e-6 ? direction * (1.0 / length) : to.direction};
}

struct GazeHand {
    uint32_t begins = 0;
    uint32_t id = 0;
    int64_t captureNs = 0;
    bool eligible = false;
    bool pinching = false;
};

enum class GazeButton { None, Down, Up };

struct GazeCursor {
    AimRay ray;
    AimRay deviceRay;
    GazeButton button;
    bool lockMouse;
};

class GazeClick {
public:
    bool Pressed() const { return pressedAt_ != 0; }
    void Reset() { *this = {}; }

    GazeCursor Step(AimRay live, const GazeHand (&hands)[2], bool pointerReady, int64_t now,
                    bool floating = false, bool lockReady = false) {
        constexpr int64_t kFreshNs = 300'000'000;
        constexpr int64_t kPendingNs = 600'000'000;
        constexpr int64_t kMinimumPressNs = 50'000'000;
        constexpr int64_t kMaximumPressNs = 4'000'000'000;
        constexpr int64_t kReleaseHoldNs = 45'000'000;
        constexpr int64_t kDragStartNs = 55'000'000;
        constexpr int64_t kDragBlendNs = 65'000'000;
        constexpr int64_t kDragReleaseNs = 250'000'000;
        const auto fresh = [&](const GazeHand &hand) {
            return hand.captureNs > 0 && now - hand.captureNs <= kFreshNs;
        };

        for (int side = 0; side < 2; ++side) {
            const auto &hand = hands[side];
            if (lastRay_ && owner_ < 0 &&
                hand.begins != begins_[side] && hand.eligible && fresh(hand)) {
                owner_ = side;
                handId_ = hand.id;
                pendingAt_ = now;
                heldRay_ = *lastRay_;
            }
            begins_[side] = hand.begins;
        }

        GazeButton button = GazeButton::None;
        bool lockMouse = locked_ && now < holdUntil_;
        if (owner_ >= 0) {
            const auto &hand = hands[owner_];
            const bool sameHand = fresh(hand) && hand.id == handId_;
            if (Pressed()) {
                const auto heldFor = now - pressedAt_;
                const bool released = !sameHand || !hand.pinching;
                if (heldFor >= kMinimumPressNs && (released || (!dragStartedAt_ && heldFor >= kMaximumPressNs))) {
                    button = GazeButton::Up;
                    pressedAt_ = 0;
                    owner_ = -1;
                    holdUntil_ = now + kReleaseHoldNs;
                    // SteamVR samples the controller again after mouse-up
                    // to commit a floated window's final world transform.
                    releasedRay_ = heldRay_;
                    deviceReleaseUntil_ = now + kDragReleaseNs;
                    lockMouse = locked_;
                    dragStartedAt_ = 0;
                } else {
                    lockMouse = locked_;
                    if (!released) {
                        if (!dragStartedAt_ && heldFor >= kDragStartNs && AimMoved(heldRay_, live))
                            dragStartedAt_ = now;
                        if (dragStartedAt_) {
                            const double fraction = std::min(double(now - dragStartedAt_) / kDragBlendNs, 1.0);
                            heldRay_ = BlendGazeRay(pressRay_, live, fraction);
                        }
                    }
                }
            } else {
                if (!sameHand || now - pendingAt_ >= kPendingNs) owner_ = -1;
                else if (pointerReady && now >= deviceReleaseUntil_) {
                    lockMouse = floating && hand.pinching;
                    if (!lockMouse || lockReady) {
                        button = GazeButton::Down;
                        pressedAt_ = now;
                        pressRay_ = heldRay_;
                        locked_ = lockMouse;
                    }
                }
            }
        }
        lastRay_ = owner_ >= 0 || now < holdUntil_ ? heldRay_ : live;
        return {*lastRay_, now < deviceReleaseUntil_ ? releasedRay_ : *lastRay_, button, lockMouse};
    }

private:
    std::optional<AimRay> lastRay_;
    AimRay heldRay_{}, pressRay_{}, releasedRay_{};
    uint32_t begins_[2]{}, handId_ = 0;
    int owner_ = -1;
    bool locked_ = false;
    int64_t pendingAt_ = 0, pressedAt_ = 0, holdUntil_ = 0, deviceReleaseUntil_ = 0, dragStartedAt_ = 0;
};

}  // namespace pointer_pose
