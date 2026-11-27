#pragma once

#include <cstdint>

namespace pointer_pose {

class TwoFistMenuGesture {
public:
    bool Step(int64_t captureNs, int64_t now, uint32_t left, uint32_t right,
              bool leftClosed, bool rightClosed, bool canToggle) {
        constexpr int64_t kMaxGapNs = 180'000'000;
        constexpr int64_t kHoldNs = 300'000'000;
        if (!left || !right || left == right || captureNs <= 0 || now - captureNs > kMaxGapNs) {
            heldSince_ = 0;
            return false;
        }
        if (captureNs == lastCapture_) return false;
        if (captureNs < lastCapture_ || captureNs - lastCapture_ > kMaxGapNs || left != left_ || right != right_)
            heldSince_ = 0;
        lastCapture_ = captureNs;
        left_ = left;
        right_ = right;

        if (!leftClosed || !rightClosed) {
            heldSince_ = 0;
            if (!leftClosed && !rightClosed) armed_ = true;
            return false;
        }
        if (!armed_) return false;
        if (!heldSince_) heldSince_ = captureNs;
        if (!canToggle || captureNs - heldSince_ < kHoldNs) return false;
        armed_ = false;
        heldSince_ = 0;
        return true;
    }

private:
    bool armed_ = false;
    int64_t lastCapture_ = 0, heldSince_ = 0;
    uint32_t left_ = 0, right_ = 0;
};

}  // namespace pointer_pose
