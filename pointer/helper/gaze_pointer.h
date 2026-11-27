#pragma once

#include "pointer_aim.h"

namespace pointer_pose {

inline std::optional<AimRay> HeadGazeRay(V head, V forward) {
    const double length = Length(forward);
    if (!std::isfinite(Length(head)) || !std::isfinite(length) || length < 0.5) return std::nullopt;
    return AimRay{head, forward * (1.0 / length)};
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
    GazeButton button;
};

class GazeClick {
public:
    bool Pressed() const { return pressedAt_ != 0; }
    void Reset() { *this = {}; }

    GazeCursor Step(AimRay live, const GazeHand (&hands)[2], bool pointerReady, int64_t now) {
        constexpr int64_t kFreshNs = 300'000'000;
        constexpr int64_t kPendingNs = 600'000'000;
        constexpr int64_t kMinimumPressNs = 50'000'000;
        constexpr int64_t kMaximumPressNs = 4'000'000'000;
        constexpr int64_t kReleaseHoldNs = 45'000'000;
        const auto usable = [&](const GazeHand &hand) {
            return hand.eligible && hand.captureNs > 0 && now - hand.captureNs <= kFreshNs;
        };

        for (int side = 0; side < 2; ++side) {
            const auto &hand = hands[side];
            if (lastRay_ && owner_ < 0 && hand.begins != begins_[side] && usable(hand)) {
                owner_ = side;
                handId_ = hand.id;
                pendingAt_ = now;
                heldRay_ = *lastRay_;
            }
            begins_[side] = hand.begins;
        }

        GazeButton button = GazeButton::None;
        if (owner_ >= 0) {
            const auto &hand = hands[owner_];
            const bool sameHand = usable(hand) && hand.id == handId_;
            if (Pressed() && now - pressedAt_ >= kMinimumPressNs &&
                (!sameHand || !hand.pinching || now - pressedAt_ >= kMaximumPressNs)) {
                button = GazeButton::Up;
                pressedAt_ = 0;
                owner_ = -1;
                holdUntil_ = now + kReleaseHoldNs;
            } else if (!Pressed()) {
                if (!sameHand || now - pendingAt_ >= kPendingNs) owner_ = -1;
                else if (pointerReady) {
                    button = GazeButton::Down;
                    pressedAt_ = now;
                }
            }
        }
        // Keep the last displayed ray steady until SteamVR sees mouse-up.
        lastRay_ = owner_ >= 0 || now < holdUntil_ ? heldRay_ : live;
        return {*lastRay_, button};
    }

private:
    std::optional<AimRay> lastRay_;
    AimRay heldRay_{};
    uint32_t begins_[2]{}, handId_ = 0;
    int owner_ = -1;
    int64_t pendingAt_ = 0, pressedAt_ = 0, holdUntil_ = 0;
};

}  // namespace pointer_pose
