#pragma once

#include "pointer_rotation.h"

namespace pointer_pose {

struct AimRay {
    V origin;
    V direction;
};

inline std::optional<AimRay> HandRay(V knuckle, V shoulder) {
    const V offset = knuckle - shoulder;
    const double distance = Length(offset);
    if (!std::isfinite(distance) || distance < 0.15) return std::nullopt;
    const V direction = offset * (1 / distance);
    return AimRay{knuckle + direction * 0.03, direction};
}

inline bool AimMoved(AimRay from, AimRay to) {
    // A small change in direction matters more than the same motion at
    // the ray's origin when aiming at a panel across the room.
    return Length(to.origin - from.origin) > 0.012 ||
           Length(to.direction - from.direction) > 0.015;
}

class HandAim {
public:
    void Reset() { *this = {}; }

    std::optional<AimRay> Step(V knuckle, V head, V headRight, bool right,
                               uint32_t handId, bool stereo, int64_t captureNs, int64_t nowNs,
                               int smoothness) {
        if (lastCaptureNs_ && captureNs == lastCaptureNs_ && handId == handId_)
            return Follow(nowNs, smoothness);
        const int64_t gap = captureNs - lastCaptureNs_;
        // Preserve a nearby ray through short re-acquisitions, but never
        // carry one into a different part of the room or a restarted stream.
        const bool newAnchor = !lastCaptureNs_ || gap <= 0 || gap > 220'000'000 ||
                               (handId != handId_ && Length(knuckle - lastKnuckle_) > 0.16);
        const double dt = std::clamp(gap / 1e9, 0.001, 0.08);
        if (newAnchor) {
            headAnchor_ = head;
            V horizontalRight{headRight.x, 0, headRight.z};
            const double n = Length(horizontalRight);
            horizontalRight = n > 0.1 ? horizontalRight * (1 / n) : V{1, 0, 0};
            const V back{-horizontalRight.z, 0, horizontalRight.x};
            shoulderOffset_ = horizontalRight * (right ? 0.17 : -0.17) + V{0, -0.20, 0} + back * 0.08;
        } else {
            // Small head movements are independent of the pointing hand.
            // Follow larger translations so aiming still works while walking.
            const V headDelta = head - headAnchor_;
            const double distance = Length(headDelta);
            if (distance > 0.16)
                headAnchor_ = headAnchor_ + headDelta * ((1 - 0.16 / distance) * Alpha(3.0, dt));
        }
        const auto raw = HandRay(knuckle, headAnchor_ + shoulderOffset_);
        if (!raw) {
            Reset();
            return std::nullopt;
        }
        if (newAnchor || !ray_) {
            ray_ = raw;
            originVelocity_ = directionVelocity_ = {};
            recoveringUntil_ = 0;
            lastAimNs_ = nowNs;
        } else {
            if (handId != handId_ || stereo != stereo_ || gap > 100'000'000) {
                originVelocity_ = directionVelocity_ = {};
                recoveringUntil_ = nowNs + 70'000'000;
                // A gap is not elapsed time spent approaching the new sample.
                lastAimNs_ = std::max(lastAimNs_, nowNs - 12'000'000);
            } else {
                const double alpha = Alpha(2.0, dt);
                originVelocity_ = originVelocity_ + ((raw->origin - target_.origin) * (1 / dt) -
                                                     originVelocity_) * alpha;
                directionVelocity_ = directionVelocity_ + ((raw->direction - target_.direction) * (1 / dt) -
                                                           directionVelocity_) * alpha;
            }
        }
        target_ = *raw;
        lastKnuckle_ = knuckle;
        lastCaptureNs_ = captureNs;
        handId_ = handId;
        stereo_ = stereo;
        return Follow(nowNs, smoothness);
    }

private:
    static double Alpha(double hz, double dt) {
        return -std::expm1(-6.283185307179586 * hz * dt);
    }

    std::optional<AimRay> Follow(int64_t nowNs, int smoothness) {
        // Advance the visible ray between camera frames, but estimate motion
        // only from actual samples. Signed velocities cancel alternating noise.
        const double dt = std::clamp((nowNs - lastAimNs_) / 1e9, 0.0, 0.05);
        lastAimNs_ = nowNs;
        const double level = 1 - std::clamp(smoothness, 0, 100) / 100.0;
        const double baseHz = 1.4 + 5.6 * level * level;
        const auto alpha = [&](V velocity, double gain, double noiseSpeed) {
            const double motion = std::max(0.0, Length(velocity) - noiseSpeed);
            const double hz = nowNs < recoveringUntil_ ? std::min(baseHz, 1.5) :
                              std::min(baseHz + gain * motion, 32.0);
            return Alpha(hz, dt);
        };
        ray_->origin = ray_->origin + (target_.origin - ray_->origin) * alpha(originVelocity_, 32.0, 0.085);
        const V direction = ray_->direction +
            (target_.direction - ray_->direction) * alpha(directionVelocity_, 28.0, 0.16);
        const double n = Length(direction);
        ray_->direction = n > 1e-6 ? direction * (1 / n) : target_.direction;
        return ray_;
    }

    std::optional<AimRay> ray_;
    AimRay target_{};
    V headAnchor_{}, shoulderOffset_{}, lastKnuckle_{};
    V originVelocity_{}, directionVelocity_{};
    int64_t lastCaptureNs_ = 0, lastAimNs_ = 0, recoveringUntil_ = 0;
    uint32_t handId_ = 0;
    bool stereo_ = false;
};

}  // namespace pointer_pose
