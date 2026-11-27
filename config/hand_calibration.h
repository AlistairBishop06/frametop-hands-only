#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <string>
#include <time.h>
#include <unistd.h>

namespace hand_calibration {

inline constexpr uint64_t kTimeoutNs = 75'000'000'000;

inline uint64_t Now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

inline std::filesystem::path Runtime() {
    return std::filesystem::path("/run/user") / std::to_string(getuid()) / "framehands";
}

inline std::filesystem::path Profile() {
    const char *home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) / ".config/framehands/hand-calibration" :
                           std::filesystem::path{};
}

inline bool Write(const std::filesystem::path &path, const std::string &data) {
    if (path.empty()) return false;
    std::string temp = path.string() + ".XXXXXX";
    const int fd = mkstemp(temp.data());
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temp.c_str());
        return false;
    }
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (std::fclose(file) == 0 && written && std::rename(temp.c_str(), path.c_str()) == 0) return true;
    unlink(temp.c_str());
    return false;
}

}  // namespace hand_calibration
