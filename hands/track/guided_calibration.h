#pragma once

#include "../../config/hand_calibration.h"
#include "skeleton.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace hand_calibration {

class Session {
public:
    explicit Session(skel::Skeleton &skeleton)
        : skeleton_(skeleton), request_(Runtime() / "hand-calibration-request"),
          status_(Runtime() / "hand-calibration-status"), profile_(Profile()) {
        std::ifstream in(profile_);
        std::string version, extra;
        skel::Calibration left, right;
        if (in >> version && version == "FRAMEHANDS_SHAPE_1" &&
            left.load(in) && right.load(in) && !(in >> extra))
            skeleton_.use_calibration(left, right);
    }

    void Poll(uint64_t now) {
        std::ifstream in(request_);
        uint64_t token = 0;
        std::string command;
        if (!(in >> token >> command) || token > now || now - token >= kTimeoutNs) {
            Stop();
            return;
        }

        if (token != token_) {
            Stop();
            token_ = token;
            result_ = "error";
            if (command == "capture") {
                samples_[0] = {};
                samples_[1] = {};
                side_ = 0;
                skeleton_.capture_calibration(-1, &samples_[0]);
            } else if (command == "reset") {
                std::error_code error;
                std::filesystem::remove(profile_, error);
                if (!profile_.empty() && !error) {
                    skeleton_.use_calibration({}, {});
                    result_ = "reset";
                }
            }
        }

        if (side_ >= 0 && samples_[side_].settled() >= 1.0) {
            if (side_ == 0) {
                side_ = 1;
                skeleton_.capture_calibration(1, &samples_[1]);
            } else {
                Stop();
                std::ostringstream out;
                out << "FRAMEHANDS_SHAPE_1\n" << std::setprecision(9);
                for (const auto &sample : samples_) sample.save(out);
                // Apply only a complete, reloadable profile. An interrupted
                // or failed capture leaves the previous profile usable.
                std::istringstream check(out.str());
                std::string version;
                skel::Calibration left, right;
                if (check >> version && left.load(check) && right.load(check) &&
                    Write(profile_, out.str())) {
                    skeleton_.use_calibration(left, right);
                    result_ = "saved";
                }
            }
        }
        const std::string state = side_ < 0 ? result_ + " 0" :
            std::string(side_ == 0 ? "left " : "right ") +
                std::to_string(int(samples_[side_].settled() * 20) * 5);
        const auto message = std::to_string(token_) + " " + state + "\n";
        if (message != reported_ && Write(status_, message)) reported_ = message;
    }

private:
    void Stop() {
        if (side_ < 0) return;
        skeleton_.capture_calibration(0, nullptr);
        side_ = -1;
    }

    skel::Skeleton &skeleton_;
    const std::filesystem::path request_, status_, profile_;
    skel::Calibration samples_[2];
    uint64_t token_ = 0;
    int side_ = -1;
    std::string result_, reported_;
};

}  // namespace hand_calibration
