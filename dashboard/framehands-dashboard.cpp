// On-demand SteamVR dashboard overlay, launched from the + menu.
// A separate oneshot systemd unit registers the app; this UI is never auto-started.
// Closing the UI destroys the overlay but does NOT change hand-tracking settings.
#include <openvr.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr int W = 640, H = 480;
struct RGB { uint8_t r, g, b; };
std::vector<uint8_t> pixels(W * H * 4);

// Five-wide bitmap capitals keep this app entirely self-contained on ARM64.
const uint8_t *Glyph(char c) {
    static const uint8_t font[39][7] = {
        {14,17,17,31,17,17,17}, // A
        {30,17,17,30,17,17,30}, // B
        {15,16,16,16,16,16,15}, // C
        {30,17,17,17,17,17,30}, // D
        {31,16,16,30,16,16,31}, // E
        {31,16,16,30,16,16,16}, // F
        {15,16,16,23,17,17,15}, // G
        {17,17,17,31,17,17,17}, // H
        {31,4,4,4,4,4,31},       // I
        {7,2,2,2,18,18,12},     // J
        {17,18,20,24,20,18,17}, // K
        {16,16,16,16,16,16,31}, // L
        {17,27,21,21,17,17,17}, // M
        {17,25,21,19,17,17,17}, // N
        {14,17,17,17,17,17,14}, // O
        {30,17,17,30,16,16,16}, // P
        {14,17,17,17,21,18,13}, // Q
        {30,17,17,30,20,18,17}, // R
        {15,16,16,14,1,1,30},   // S
        {31,4,4,4,4,4,4},       // T
        {17,17,17,17,17,17,14}, // U
        {17,17,17,17,17,10,4},  // V
        {17,17,17,21,21,21,10}, // W
        {17,17,10,4,10,17,17},  // X
        {17,17,10,4,4,4,4},     // Y
        {31,1,2,4,8,16,31},    // Z
        {14,17,19,21,25,17,14}, // 0
        {4,12,4,4,4,4,14},     // 1
        {14,17,1,2,4,8,31},    // 2
        {30,1,1,14,1,1,30},    // 3
        {2,6,10,18,31,2,2},    // 4
        {31,16,16,30,1,1,30},  // 5
        {14,16,16,30,17,17,14},// 6
        {31,1,2,4,8,8,8},      // 7
        {14,17,17,14,17,17,14},// 8
        {14,17,17,15,1,1,14},  // 9
        {0,4,4,0,4,4,0},      // :
        {0,0,0,31,0,0,0},     // -
        {0,0,0,0,0,0,0},      // space
    };
    if (c >= 'A' && c <= 'Z') return font[c - 'A'];
    if (c >= '0' && c <= '9') return font[26 + c - '0'];
    return font[c == ':' ? 36 : c == '-' ? 37 : 38];
}
void Rect(int x0, int y0, int x1, int y1, RGB c) {
    for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
            auto i = size_t((y * W + x) * 4);
            pixels[i] = c.r; pixels[i+1] = c.g; pixels[i+2] = c.b; pixels[i+3] = 255;
        }
}
void Text(const std::string &s, int y, int size, RGB c) {
    const int width = int(s.size()) * 6 * size;
    const int x0 = (W - width) / 2;
    for (size_t k = 0; k < s.size(); ++k) {
        const uint8_t *p = Glyph(s[k]);
        for (int r = 0; r < 7; ++r)
            for (int col = 0; col < 5; ++col)
                if (p[r] & (16 >> col))
                    Rect(x0 + int(k)*6*size + col*size, y+r*size,
                         x0 + int(k)*6*size + (col+1)*size, y+(r+1)*size, c);
    }
}

std::filesystem::path AppRoot() {
    // $app/dashboard/build/framehands-dashboard -> $app
    return std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path().parent_path();
}
std::filesystem::path Manifest() { return AppRoot() / "dashboard/framehands.vrmanifest"; }
std::filesystem::path Cli() { return AppRoot() / "framehands"; }

bool Enabled() {
    const char *home = std::getenv("HOME");
    if (!home) return false;
    std::ifstream in(std::filesystem::path(home) / ".config/framehands/config");
    std::string line;
    bool track = true, autostart = true;
    while (std::getline(in, line)) {
        if (line == "TRACKING=0") track = false;
        if (line == "TRACKING=1") track = true;
        if (line == "AUTO_START=0") autostart = false;
        if (line == "AUTO_START=1") autostart = true;
    }
    return track && autostart;
}
bool TouchEnabled() {
    const char *home = std::getenv("HOME");
    if (!home) return false;
    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");
    std::string line; bool enabled = false;
    while (std::getline(f, line))
        if (line == "HANDS_TOUCH=1") enabled = true;
        else if (line == "HANDS_TOUCH=0") enabled = false;
    return enabled;
}
int Smoothness() {
    const char *home = std::getenv("HOME");
    if (!home) return 50;
    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");
    std::string line; int level = 50;
    while (std::getline(f, line)) if (line.rfind("HANDS_SMOOTHNESS=", 0) == 0) {
        try { level = std::stoi(line.substr(17)); } catch (...) {}
    }
    return std::clamp(level, 0, 100);
}
void Paint(bool enabled, bool touch, int smoothness, bool busy, bool failed) {
    Rect(0, 0, W, H, {13, 22, 38});
    Rect(0, 0, W, 10, {73, 175, 235});
    // The X is separate from the toggle: closing never disables tracking.
    Rect(566, 22, 628, 94, {44, 62, 83});
    for (int i = 0; i < 23; ++i) {
        Rect(581 + i, 43 + i, 585 + i, 47 + i, {232, 244, 255});
        Rect(603 - i, 43 + i, 607 - i, 47 + i, {232, 244, 255});
    }
    Text("FRAMEHANDS", 39, 5, {232, 244, 255});
    Text("HAND TRACKING", 116, 3, {176, 198, 219});
    const RGB action = enabled ? RGB{30, 130, 98} : RGB{45, 123, 195};
    Rect(76, 165, 564, 277, action);
    if (busy) Text("UPDATING", 188, 5, {255, 255, 255});
    else Text(enabled ? "ON" : "OFF", 188, 6, {255, 255, 255});
    Text(failed ? "FAILED - CHECK LOGS" : enabled ? "CLICK TO TURN OFF" : "CLICK TO TURN ON",
         276, 2, {204, 220, 233});
    Rect(76, 306, 564, 356, touch ? RGB{26, 133, 115} : RGB{69, 83, 107});
    Text(touch ? "DIRECT TOUCH ON" : "DIRECT TOUCH OFF", 316, 3, {245, 250, 255});
    Text("HAND SMOOTHNESS", 369, 2, {204, 220, 233});
    Rect(100, 409, 540, 425, {52, 69, 90});
    const int knob = 100 + 440 * std::clamp(smoothness, 0, 100) / 100;
    Rect(100, 409, knob, 425, {73, 175, 235});
    Rect(knob-6, 402, knob+6, 432, {237, 245, 255});
    Text("SMOOTHNESS " + std::to_string(smoothness), 444, 2, {204, 220, 233});
}

int RegisterManifest(bool remove) {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Utility);
    if (error != vr::VRInitError_None) return 1;
    const std::string path = Manifest().string();
    const vr::EVRApplicationError result = remove
        ? vr::VRApplications()->RemoveApplicationManifest(path.c_str())
        : vr::VRApplications()->AddApplicationManifest(path.c_str(), false);
    vr::VR_Shutdown();
    return result == vr::VRApplicationError_None ? 0 : 1;
}
}

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--unregister")) return RegisterManifest(true);
    if (argc == 2 && !std::strcmp(argv[1], "--register")) return RegisterManifest(false);
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        std::cerr << "framehands-dashboard: " << vr::VR_GetVRInitErrorAsEnglishDescription(error) << '\n';
        return 1;
    }
    const std::string manifest = Manifest().string();
    const auto registered = vr::VRApplications()->AddApplicationManifest(manifest.c_str(), false);
    if (registered != vr::VRApplicationError_None)
        std::cerr << "framehands-dashboard: registration returned " << int(registered) << '\n';
    vr::VROverlayHandle_t mainOverlay = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumb = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError created = vr::VROverlay()->CreateDashboardOverlay(
        "framehands.dashboard", "FrameHands", &mainOverlay, &thumb);
    if (created != vr::VROverlayError_None) {
        std::cerr << "framehands-dashboard: CreateDashboardOverlay failed: " << int(created) << '\n';
        vr::VR_Shutdown();
        return 1;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(mainOverlay, 1.8f);
    vr::VROverlay()->SetOverlayInputMethod(mainOverlay, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t mouseScale{}; mouseScale.v[0] = W; mouseScale.v[1] = H;
    vr::VROverlay()->SetOverlayMouseScale(mainOverlay, &mouseScale);
    // The thumbnail needs no external PNG dependencies.
    std::array<uint8_t, 64*64*4> thumbnail{};
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const int i = (y*64+x)*4;
        const bool mid = x > 20 && x < 44 && y > 13 && y < 51;
        thumbnail[i] = mid ? 88 : 17;
        thumbnail[i+1] = mid ? 198 : 38;
        thumbnail[i+2] = mid ? 238 : 66;
        thumbnail[i+3] = 255;
    }
    vr::VROverlay()->SetOverlayRaw(thumb, thumbnail.data(), 64, 64, 4);

    bool enabled = Enabled(), touch = TouchEnabled(), busy = false, failed = false;
    int smoothness = Smoothness();
    bool sliding = false;
    pid_t child = -1;
    auto next = std::chrono::steady_clock::now();
    Paint(enabled, touch, smoothness, busy, failed);
    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
    // Opening FrameHands from + selects this panel immediately.
    vr::VROverlay()->ShowDashboard("framehands.dashboard");
    bool running = true;
    bool wasActive = false;
    while (running) {
        vr::VREvent_t event{};
        while (running && vr::VROverlay()->PollNextOverlayEvent(mainOverlay, &event, sizeof event)) {
            if (event.eventType == vr::VREvent_OverlayClosed ||
                event.eventType == vr::VREvent_Quit) {
                running = false;
                break;
            }
            if (event.eventType == vr::VREvent_OverlayHidden && wasActive) {
                // Leaving this panel removes it from the dashboard tab bar.
                running = false;
                break;
            }
            if (event.eventType == vr::VREvent_MouseMove && sliding) {
                smoothness = std::clamp(int((event.data.mouse.x - 100.f) * 100.f / 440.f), 0, 100);
                Paint(enabled, touch, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
            if (event.eventType == vr::VREvent_MouseButtonUp && sliding) {
                sliding = false;
                child = fork();
                if (child == 0) {
                    const std::string cli = Cli().string();
                    const std::string level = std::to_string(smoothness);
                    execl(cli.c_str(), cli.c_str(), "smoothness", level.c_str(), (char *)nullptr);
                    _exit(127);
                }
                busy = child > 0; failed = child < 0;
            }
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const float x = event.data.mouse.x;
                const float y = H - event.data.mouse.y; // GL mouse coordinates are bottom-left.
                if (x >= 566 && x <= 628 && y >= 22 && y <= 94) {
                    running = false;
                    break;
                }
                // Slider changes are committed on mouse-up, not on every move.
                if (x >= 82 && x <= 554 && y >= 390 && y <= 439) {
                    sliding = true;
                    smoothness = std::clamp(int((x - 100.f) * 100.f / 440.f), 0, 100);
                    Paint(enabled, touch, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (x >= 76 && x <= 564 && y >= 306 && y <= 356) {
                    const bool toEnable = !touch;
                    child = fork();
                    if (child == 0) {
                        const std::string cli = Cli().string();
                        execl(cli.c_str(), cli.c_str(), toEnable ? "touch-on" : "touch-off", (char *)nullptr);
                        _exit(127);
                    }
                    busy = child > 0; failed = child < 0;
                    Paint(enabled, touch, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (!(x >= 76 && x <= 564 && y >= 165 && y <= 277)) continue;
                const bool toEnable = !enabled;
                child = fork();
                if (child == 0) {
                    const std::string cli = Cli().string();
                    execl(cli.c_str(), cli.c_str(), toEnable ? "enable" : "disable", (char *)nullptr);
                    _exit(127);
                }
                busy = child > 0;
                failed = child < 0;
                Paint(enabled, touch, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event))
            if (event.eventType == vr::VREvent_Quit ||
                event.eventType == vr::VREvent_OverlayClosed) running = false;
        if (vr::VROverlay()->IsActiveDashboardOverlay(mainOverlay)) wasActive = true;
        if (!running) break;
        if (child > 0) {
            int status = 0;
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child || done < 0) {
                child = -1;
                busy = false;
                failed = done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
                enabled = Enabled();
                touch = TouchEnabled();
                smoothness = Smoothness();
                Paint(enabled, touch, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        if (std::chrono::steady_clock::now() >= next && !busy) {
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            const bool current = Enabled(), currentTouch = TouchEnabled();
            const int currentSmooth = Smoothness();
            if (!sliding && (current != enabled || currentTouch != touch || currentSmooth != smoothness)) {
                enabled = current; touch = currentTouch; smoothness = currentSmooth;
                Paint(enabled, touch, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    vr::VROverlay()->DestroyOverlay(mainOverlay);
    vr::VROverlay()->DestroyOverlay(thumb);
    vr::VR_Shutdown();
    return 0;
}
