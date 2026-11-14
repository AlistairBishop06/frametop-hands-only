#pragma once
#include <algorithm>
#include <string>

inline bool IsGameScene(const std::string &key) {
    // Home/Aurora and the Steam Link shell can be persistent OpenVR scenes.
    // Unknown scene keys leave tracking available.
    if (key.rfind("steam.app.", 0) != 0 ||
        key == "steam.app.250820" || key == "steam.app.353380") return false;
    const std::string id = key.substr(10);
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}
