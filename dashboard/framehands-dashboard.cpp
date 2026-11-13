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
struct Box {
    int x0, y0, x1, y1;
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
constexpr Box kClose{580, 24, 626, 70};
constexpr Box kTracking{28, 105, 612, 218};
constexpr Box kVisuals{28, 232, 311, 345};
constexpr Box kTouch{327, 232, 612, 345};
constexpr Box kSlider{49, 407, 591, 462};
constexpr int kSliderLeft = 50, kSliderRight = 590;
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
void RoundRect(Box b, int radius, RGB c) {
    for (int y = std::max(0, b.y0); y < std::min(H, b.y1); ++y)
        for (int x = std::max(0, b.x0); x < std::min(W, b.x1); ++x) {
            const int dx = x < b.x0 + radius ? b.x0 + radius - x - 1 :
                           x >= b.x1 - radius ? x - (b.x1 - radius) : 0;
            const int dy = y < b.y0 + radius ? b.y0 + radius - y - 1 :
                           y >= b.y1 - radius ? y - (b.y1 - radius) : 0;
            if (dx * dx + dy * dy > radius * radius) continue;
            const auto i = size_t((y * W + x) * 4);
            pixels[i] = c.r; pixels[i+1] = c.g; pixels[i+2] = c.b; pixels[i+3] = 255;
        }
}
void Circle(int cx, int cy, int radius, RGB c) {
    for (int y = std::max(0, cy-radius); y < std::min(H, cy+radius+1); ++y)
        for (int x = std::max(0, cx-radius); x < std::min(W, cx+radius+1); ++x)
            if ((x-cx)*(x-cx) + (y-cy)*(y-cy) <= radius*radius) {
                const auto i = size_t((y * W + x) * 4);
                pixels[i] = c.r; pixels[i+1] = c.g; pixels[i+2] = c.b; pixels[i+3] = 255;
            }
}
void TextAt(int x0, int y, const std::string &s, int size, RGB c) {
    for (size_t k = 0; k < s.size(); ++k) {
        const uint8_t *p = Glyph(s[k]);
        for (int r = 0; r < 7; ++r)
            for (int col = 0; col < 5; ++col)
                if (p[r] & (16 >> col))
                    Rect(x0 + int(k)*6*size + col*size, y+r*size,
                         x0 + int(k)*6*size + (col+1)*size, y+(r+1)*size, c);
    }
}
void Toggle(int x, int y, bool on) {
    RoundRect({x, y, x+72, y+38}, 19, on ? RGB{34, 156, 133} : RGB{64, 81, 101});
    Circle(x + (on ? 52 : 20), y+19, 14, {246, 251, 253});
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
bool VisualsEnabled() {
    const char *home = std::getenv("HOME");
    if (!home) return true;
    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");
    std::string line; bool enabled = true;
    while (std::getline(f, line))
        if (line == "RENDER=1") enabled = true;
        else if (line == "RENDER=0") enabled = false;
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
void Paint(bool enabled, bool visuals, bool touch, int smoothness, bool busy, bool failed) {
    const RGB ink{236, 245, 251}, muted{151, 174, 195}, teal{80, 219, 180};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int dx = x-535, dy = y-8;
            const int glow = std::max(0, 18 - (dx*dx + dy*dy) / 8500);
            const auto i = size_t((y * W + x) * 4);
            pixels[i] = uint8_t(9 + glow/3);
            pixels[i+1] = uint8_t(17 + glow/2);
            pixels[i+2] = uint8_t(30 + glow);
            pixels[i+3] = 255;
        }
    Rect(0, 0, W, 5, teal);
    RoundRect({28, 27, 74, 73}, 14, {30, 107, 120});
    // A small hand mark gives the app a recognizable silhouette in VR.
    RoundRect({44, 48, 61, 65}, 6, ink);
    Rect(43, 40, 47, 54, ink);
    Rect(49, 35, 53, 53, ink);
    Rect(55, 38, 59, 53, ink);
    Rect(61, 43, 65, 54, ink);
    TextAt(92, 32, "FRAMEHANDS", 4, ink);
    TextAt(94, 68, "STEAM FRAME CONTROLS", 2, muted);

    const RGB status = failed ? RGB{72, 43, 44} : busy ? RGB{50, 64, 82} :
                       enabled ? RGB{25, 75, 70} : RGB{56, 67, 80};
    RoundRect({394, 34, 574, 69}, 17, status);
    Circle(410, 51, 5, failed ? RGB{255, 149, 132} : busy ? RGB{255, 194, 103} :
                         enabled ? teal : RGB{153, 170, 185});
    TextAt(423, 43, failed ? "CHECK LOGS" : busy ? "UPDATING" :
           enabled ? "TRACKING ON" : "TRACKING OFF", 2, ink);
    // Closing this panel never changes tracking.
    RoundRect(kClose, 14, {33, 48, 66});
    for (int i = 0; i < 14; ++i) {
        Rect(595+i, 39+i, 598+i, 42+i, ink);
        Rect(608-i, 39+i, 611-i, 42+i, ink);
    }

    RoundRect({28, 109, 612, 222}, 18, {5, 11, 22});
    RoundRect(kTracking, 18, {26, 43, 60});
    RoundRect({29, 106, 34, 217}, 2, enabled ? teal : RGB{112, 134, 156});
    TextAt(51, 127, "HAND TRACKING", 3, ink);
    TextAt(52, 166, "CAMERAS AND HAND INPUT", 2, muted);
    TextAt(52, 193, enabled ? "OFF HIDES HAND POINTER" : "CONTROLLER NEEDED TO RESTART",
           2, enabled ? RGB{236, 188, 116} : muted);
    Toggle(514, 142, enabled);

    RoundRect({28, 236, 311, 349}, 18, {5, 11, 22});
    RoundRect(kVisuals, 18, {23, 37, 55});
    TextAt(48, 251, "HAND VISUALS", 2, ink);
    TextAt(48, 287, "WHITE HANDS", 2, muted);
    TextAt(48, 319, visuals ? (enabled ? "VISIBLE" : "READY") : "HIDDEN", 2,
           visuals && enabled ? teal : muted);
    Toggle(227, 292, visuals);

    RoundRect({327, 236, 612, 349}, 18, {5, 11, 22});
    RoundRect(kTouch, 18, {23, 37, 55});
    TextAt(347, 251, "DIRECT TOUCH", 2, ink);
    TextAt(347, 287, "TOUCH TO CLICK", 2, muted);
    TextAt(347, 319, touch ? (enabled ? "ENABLED" : "READY") : "DISABLED", 2,
           touch && enabled ? teal : muted);
    Toggle(531, 292, touch);

    RoundRect({28, 364, 612, 469}, 18, {5, 11, 22});
    RoundRect({28, 360, 612, 465}, 18, {23, 37, 55});
    TextAt(50, 377, "SMOOTHNESS", 2, ink);
    TextAt(520, 377, std::to_string(std::clamp(smoothness, 0, 100)), 2, teal);
    RoundRect({kSliderLeft, 418, kSliderRight, 434}, 8, {53, 70, 89});
    const int knob = kSliderLeft + (kSliderRight-kSliderLeft) * std::clamp(smoothness, 0, 100) / 100;
    if (knob > kSliderLeft) RoundRect({kSliderLeft, 418, knob, 434}, 8, {56, 177, 164});
    Circle(knob, 426, 13, ink);
    Circle(knob, 426, 5, {51, 166, 156});
    TextAt(50, 443, "LESS", 2, muted);
    TextAt(542, 443, "MORE", 2, muted);
}
int SmoothnessAt(float x) {
    return std::clamp(int(std::lround((x - kSliderLeft) * 100.0f / (kSliderRight-kSliderLeft))), 0, 100);
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
    // Match the panel's hand mark without an external image dependency.
    std::array<uint8_t, 64*64*4> thumbnail{};
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const int i = (y*64+x)*4;
        const bool palm = x >= 23 && x < 45 && y >= 33 && y < 51;
        const bool fingers = (x >= 23 && x < 28 && y >= 22 && y < 39) ||
                             (x >= 29 && x < 34 && y >= 15 && y < 39) ||
                             (x >= 35 && x < 40 && y >= 19 && y < 39) ||
                             (x >= 41 && x < 46 && y >= 25 && y < 39);
        const bool thumb = x >= 17 && x < 26 && y >= 36 && y < 46;
        const bool hand = palm || fingers || thumb;
        thumbnail[i] = hand ? 236 : 20;
        thumbnail[i+1] = hand ? 250 : 55;
        thumbnail[i+2] = hand ? 250 : 72;
        thumbnail[i+3] = 255;
    }
    vr::VROverlay()->SetOverlayRaw(thumb, thumbnail.data(), 64, 64, 4);

    bool enabled = Enabled(), visuals = VisualsEnabled(), touch = TouchEnabled(), busy = false, failed = false;
    int smoothness = Smoothness();
    bool sliding = false;
    pid_t child = -1;
    auto next = std::chrono::steady_clock::now();
    auto nextSliderDraw = next;
    Paint(enabled, visuals, touch, smoothness, busy, failed);
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
                const int value = SmoothnessAt(event.data.mouse.x);
                if (value != smoothness) {
                    smoothness = value;
                    if (std::chrono::steady_clock::now() >= nextSliderDraw) {
                        nextSliderDraw = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                        Paint(enabled, visuals, touch, smoothness, busy, failed);
                        vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    }
                }
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
                Paint(enabled, visuals, touch, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const float x = event.data.mouse.x;
                const float y = H - event.data.mouse.y; // GL mouse coordinates are bottom-left.
                if (kClose.Contains(x, y)) {
                    running = false;
                    break;
                }
                // Slider changes are committed on mouse-up, not on every move.
                if (kSlider.Contains(x, y)) {
                    sliding = true;
                    smoothness = SmoothnessAt(x);
                    nextSliderDraw = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                    Paint(enabled, visuals, touch, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (kVisuals.Contains(x, y)) {
                    const bool toEnable = !visuals;
                    child = fork();
                    if (child == 0) {
                        const std::string cli = Cli().string();
                        execl(cli.c_str(), cli.c_str(), toEnable ? "visuals-on" : "visuals-off", (char *)nullptr);
                        _exit(127);
                    }
                    busy = child > 0; failed = child < 0;
                    Paint(enabled, visuals, touch, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (kTouch.Contains(x, y)) {
                    const bool toEnable = !touch;
                    child = fork();
                    if (child == 0) {
                        const std::string cli = Cli().string();
                        execl(cli.c_str(), cli.c_str(), toEnable ? "touch-on" : "touch-off", (char *)nullptr);
                        _exit(127);
                    }
                    busy = child > 0; failed = child < 0;
                    Paint(enabled, visuals, touch, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (!kTracking.Contains(x, y)) continue;
                const bool toEnable = !enabled;
                child = fork();
                if (child == 0) {
                    const std::string cli = Cli().string();
                    execl(cli.c_str(), cli.c_str(), toEnable ? "enable" : "disable", (char *)nullptr);
                    _exit(127);
                }
                busy = child > 0;
                failed = child < 0;
                Paint(enabled, visuals, touch, smoothness, busy, failed);
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
                visuals = VisualsEnabled();
                touch = TouchEnabled();
                smoothness = Smoothness();
                Paint(enabled, visuals, touch, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        if (std::chrono::steady_clock::now() >= next && !busy) {
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            const bool current = Enabled(), currentVisuals = VisualsEnabled(), currentTouch = TouchEnabled();
            const int currentSmooth = Smoothness();
            if (!sliding && (current != enabled || currentVisuals != visuals || currentTouch != touch || currentSmooth != smoothness)) {
                enabled = current; visuals = currentVisuals; touch = currentTouch; smoothness = currentSmooth;
                Paint(enabled, visuals, touch, smoothness, busy, failed);
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
