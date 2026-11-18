// On-demand SteamVR dashboard overlay, launched from the + menu.
// A separate oneshot systemd unit registers the app; this UI is never auto-started.
// Closing the UI destroys the overlay but does NOT change hand-tracking settings.
#include <openvr.h>
#include "game_scene.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <csignal>
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
constexpr int W = 640, H = 790;
struct RGB { uint8_t r, g, b; };
struct Box {
    int x0, y0, x1, y1;
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
constexpr Box kClose{580, 24, 626, 70};
constexpr Box kTracking{28, 105, 612, 218};
constexpr Box kVisuals{28, 232, 311, 345};
constexpr Box kTouch{327, 232, 612, 345};
constexpr Box kGamePause{28, 360, 612, 449};
constexpr Box kSlider{49, 517, 591, 572};
constexpr Box kColour{28, 592, 612, 691};
constexpr Box kStable{332, 705, 464, 744};
constexpr Box kDev{472, 705, 612, 744};
constexpr Box kUpdate{28, 747, 612, 784};
constexpr int kSliderLeft = 50, kSliderRight = 590;
std::vector<uint8_t> pixels(W * H * 4);
std::string updateStatus = "CHECK FOR UPDATES";
std::string handColour = "white";
std::string updateChannel = "stable";
constexpr const char *kColourNames[] = {"red", "orange", "yellow", "green", "blue", "purple", "black", "white"};
constexpr RGB kColourSwatches[] = {{230, 55, 55}, {246, 132, 37}, {247, 213, 42}, {56, 202, 91},
                                   {62, 126, 238}, {167, 89, 220}, {16, 20, 27}, {244, 248, 253}};

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
std::filesystem::path Updater() { return AppRoot() / "scripts/framehands-updater.py"; }
std::filesystem::path UpdateStatusFile() {
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/nonexistent") / ".cache/framehands/update-status";
}
void ReadUpdateStatus() {
    std::ifstream in(UpdateStatusFile());
    std::string line;
    if (std::getline(in, line) && !line.empty()) updateStatus = line;
}

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
bool GamePauseEnabled() {
    const char *home = std::getenv("HOME");
    if (!home) return false;
    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");
    std::string line; bool enabled = false;
    while (std::getline(f, line))
        if (line == "PAUSE_IN_GAMES=1") enabled = true;
        else if (line == "PAUSE_IN_GAMES=0") enabled = false;
    return enabled;
}
bool GamePaused() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime && std::filesystem::exists(std::filesystem::path(runtime) / "framehands/game-paused");
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
std::string HandColour() {
    const char *home = std::getenv("HOME");
    if (!home) return "white";
    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");
    std::string line, colour = "white";
    while (std::getline(f, line)) if (line.rfind("HANDS_VIEW_COLOUR=", 0) == 0)
        colour = line.substr(18);
    for (const char *name : kColourNames) if (colour == name) return colour;
    return "white";
}
std::string UpdateChannel() {
    const char *home = std::getenv("HOME");
    if (!home) return "stable";
    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");
    std::string line, channel = "stable";
    while (std::getline(f, line)) {
        if (line == "UPDATE_CHANNEL=dev") channel = "dev";
        else if (line == "UPDATE_CHANNEL=stable") channel = "stable";
    }
    return channel;
}
void Paint(bool enabled, bool visuals, bool touch, bool gamePause, int smoothness, bool busy, bool failed) {
    const RGB ink{235, 244, 250}, muted{139, 161, 178}, teal{75, 215, 179};
    Rect(0, 0, W, H, {12, 20, 31});
    Rect(0, 0, W, 4, teal);
    RoundRect({28, 27, 74, 73}, 12, {28, 83, 86});
    RoundRect({43, 47, 61, 64}, 5, ink);
    Rect(43, 39, 47, 54, ink); Rect(49, 35, 53, 53, ink);
    Rect(55, 38, 59, 53, ink); Rect(61, 43, 65, 54, ink);
    TextAt(91, 31, "FRAMEHANDS", 4, ink);
    TextAt(93, 67, "HAND TRACKING CONTROLS", 2, muted);
    RoundRect(kClose, 12, {34, 48, 63});
    for (int i = 0; i < 14; ++i) {
        Rect(595+i, 39+i, 598+i, 42+i, ink);
        Rect(608-i, 39+i, 611-i, 42+i, ink);
    }
    const bool paused = gamePause && GamePaused();
    RoundRect(kTracking, 18, {25, 43, 55});
    RoundRect({28, 105, 33, 218}, 2, enabled ? teal : muted);
    TextAt(51, 124, "TRACKING", 3, ink);
    TextAt(51, 164, paused ? "PAUSED FOR VR GAME" : failed ? "CHECK LOGS" :
           busy ? "APPLYING CHANGE" : enabled ? "HANDS ACTIVE" : "HANDS OFF", 2,
           paused ? RGB{244, 190, 105} : enabled ? teal : muted);
    Toggle(514, 139, enabled);

    TextAt(29, 228, "INTERACTION", 2, muted);
    RoundRect(kVisuals, 14, {24, 39, 53});
    TextAt(48, 249, "HAND OVERLAY", 2, ink);
    TextAt(48, 281, visuals ? "VISIBLE" : "HIDDEN", 2, visuals ? teal : muted);
    Toggle(227, 291, visuals);
    RoundRect(kTouch, 14, {24, 39, 53});
    TextAt(347, 249, "DIRECT TOUCH", 2, ink);
    TextAt(347, 281, touch ? "ON" : "OFF", 2, touch ? teal : muted);
    Toggle(531, 291, touch);
    RoundRect(kGamePause, 14, {24, 39, 53});
    TextAt(50, 380, "PAUSE DURING VR GAMES", 2, ink);
    TextAt(50, 414, gamePause ? (paused ? "TRACKING PAUSED" : "AUTOMATIC") : "OFF", 2,
           paused ? RGB{244, 190, 105} : muted);
    Toggle(514, 385, gamePause);

    TextAt(29, 462, "APPEARANCE", 2, muted);
    RoundRect({28, 489, 612, 579}, 14, {24, 39, 53});
    TextAt(50, 495, "SMOOTHNESS", 2, ink);
    TextAt(552, 495, std::to_string(std::clamp(smoothness, 0, 100)), 2, teal);
    RoundRect({kSliderLeft, 528, kSliderRight, 544}, 8, {52, 68, 83});
    const int knob = kSliderLeft + (kSliderRight-kSliderLeft) * std::clamp(smoothness, 0, 100) / 100;
    if (knob > kSliderLeft) RoundRect({kSliderLeft, 528, knob, 544}, 8, teal);
    Circle(knob, 536, 12, ink);
    RoundRect(kColour, 14, {24, 39, 53});
    std::string colour = handColour;
    std::transform(colour.begin(), colour.end(), colour.begin(), [](unsigned char c) { return std::toupper(c); });
    TextAt(50, 599, "HAND COLOUR", 2, ink);
    TextAt(450, 599, colour, 2, muted);
    for (int i = 0; i < 8; ++i) {
        const int x = 71 + 68 * i;
        if (handColour == kColourNames[i]) Circle(x, 645, 23, teal);
        Circle(x, 645, 18, kColourSwatches[i]);
    }
    TextAt(29, 704, "UPDATES", 2, muted);
    RoundRect(kStable, 10, updateChannel == "stable" ? RGB{35, 105, 98} : RGB{34, 48, 63});
    RoundRect(kDev, 10, updateChannel == "dev" ? RGB{35, 105, 98} : RGB{34, 48, 63});
    TextAt(359, 716, "STABLE", 2, ink);
    TextAt(515, 716, "DEV", 2, ink);
    RoundRect(kUpdate, 10, {32, 91, 85});
    const std::string shown = updateStatus.rfind("AVAILABLE ", 0) == 0
        ? "DOWNLOAD " + updateStatus.substr(10) : updateStatus;
    const std::string label = shown.size() > 31 ? shown.substr(0, 31) : shown;
    TextAt(50, 759, label, 2, updateStatus.rfind("ERROR", 0) == 0 ? RGB{255, 149, 132} : ink);
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
int GameMonitor() {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) return 1;
    bool paused = GamePaused();
    int gameTicks = 0, idleTicks = 0;
    std::string lastKey = "<initial>";
    while (true) {
        const uint32_t scenePid = vr::VRApplications()->GetCurrentSceneProcessId();
        char appKey[256] = {};
        const bool identified = scenePid &&
            vr::VRApplications()->GetApplicationKeyByProcessId(scenePid, appKey, sizeof appKey) == vr::VRApplicationError_None;
        const std::string key = identified ? appKey : "";
        if (key != lastKey) {
            std::cerr << "framehands game monitor: scene pid=" << scenePid
                      << " key=" << (key.empty() ? "unidentified" : key) << '\n';
            lastKey = key;
        }
        const bool game = IsGameScene(key);
        gameTicks = game ? std::min(gameTicks + 1, 4) : 0;
        idleTicks = game ? 0 : std::min(idleTicks + 1, 4);
        if ((gameTicks >= 2 && !paused) || (idleTicks >= 4 && paused)) {
            const char *command = game ? "game-pause" : "game-resume";
            const pid_t child = fork();
            if (child == 0) {
                const std::string cli = Cli().string();
                execl(cli.c_str(), cli.c_str(), command, (char *)nullptr);
                _exit(127);
            }
            int status = 0;
            if (child > 0 && waitpid(child, &status, 0) == child &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                paused = game;
                std::cerr << "framehands game monitor: " << (paused ? "paused" : "resumed") << '\n';
            } else {
                std::cerr << "framehands game monitor: " << command << " failed\n";
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
}

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--monitor-games")) return GameMonitor();
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
    const auto clickStabilization = vr::VROverlay()->SetOverlayFlag(
        mainOverlay, vr::VROverlayFlags_EnableClickStabilization, true);
    if (clickStabilization != vr::VROverlayError_None)
        std::cerr << "framehands-dashboard: click stabilization unavailable: "
                  << int(clickStabilization) << '\n';
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
    std::signal(SIGPIPE, SIG_IGN); // a failed updater must not terminate the panel

    bool enabled = Enabled(), visuals = VisualsEnabled(), touch = TouchEnabled();
    bool gamePause = GamePauseEnabled(), pausedState = GamePaused(), busy = false, failed = false;
    int smoothness = Smoothness();
    handColour = HandColour();
    updateChannel = UpdateChannel();
    bool sliding = false, updateChild = false, awaitingPassword = false;
    std::string updateVersion;
    pid_t child = -1;
    auto next = std::chrono::steady_clock::now();
    auto nextSliderDraw = next;
    Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
    // Opening FrameHands from + selects this panel immediately.
    vr::VROverlay()->ShowDashboard("framehands.dashboard");
    bool running = true;
    bool wasActive = false;
    auto passwordEvent = [&](uint32_t type) {
        if (!awaitingPassword) return;
        if (type == vr::VREvent_KeyboardClosed || type == vr::VREvent_KeyboardClosed_Global) {
            awaitingPassword = false;
            updateStatus = "UPDATE CANCELLED";
        } else if (type == vr::VREvent_KeyboardDone) {
            awaitingPassword = false;
            char password[128]{};
            const uint32_t length = vr::VROverlay()->GetKeyboardText(password, sizeof password);
            password[sizeof password - 1] = 0;
            if (!length || length > sizeof password || !password[0] ||
                std::strchr(password, '\n') || std::strchr(password, '\r')) {
                updateStatus = "PASSWORD REQUIRED";
            } else {
                int auth[2];
                if (pipe(auth) != 0) {
                    updateStatus = "UPDATE ERROR";
                } else {
                    child = fork();
                    if (child == 0) {
                        close(auth[1]);
                        if (auth[0] != 3 && dup2(auth[0], 3) < 0) _exit(127);
                        if (auth[0] != 3) close(auth[0]);
                        setenv("FRAMEHANDS_SUDO_FD", "3", 1);
                        const std::string updater = Updater().string();
                        execlp("python3", "python3", updater.c_str(), "install", updateVersion.c_str(), (char *)nullptr);
                        _exit(127);
                    }
                    close(auth[0]);
                    if (child > 0) {
                        const size_t count = std::strlen(password);
                        // Two setcap calls may need authentication: the new
                        // broker and removal of capabilities on the old one.
                        for (int i = 0; i < 2; ++i) {
                            if (write(auth[1], password, count) != ssize_t(count) ||
                                write(auth[1], "\n", 1) != 1) break;
                        }
                        updateChild = busy = true;
                        failed = false;
                        updateStatus = "DOWNLOADING";
                    } else {
                        updateStatus = "UPDATE ERROR";
                        failed = true;
                    }
                    close(auth[1]);
                }
            }
            volatile char *wipe = password;
            for (size_t i = 0; i < sizeof password; ++i) wipe[i] = 0;
        } else return;
        Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
        vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
    };
    while (running) {
        vr::VREvent_t event{};
        while (running && vr::VROverlay()->PollNextOverlayEvent(mainOverlay, &event, sizeof event)) {
            if (event.eventType == vr::VREvent_KeyboardDone ||
                event.eventType == vr::VREvent_KeyboardClosed ||
                event.eventType == vr::VREvent_KeyboardClosed_Global) {
                passwordEvent(event.eventType);
                continue;
            }
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
                        Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
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
                Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const float x = event.data.mouse.x;
                const float y = H - event.data.mouse.y; // GL mouse coordinates are bottom-left.
                if (kClose.Contains(x, y)) {
                    running = false;
                    break;
                }
                if (kColour.Contains(x, y)) {
                    const int slot = std::clamp((int(x) - 37) / 68, 0, 7);
                    if (y >= 622 && y < 690) {
                        child = fork();
                        if (child == 0) {
                            const std::string cli = Cli().string();
                            execl(cli.c_str(), cli.c_str(), "colour", kColourNames[slot], (char *)nullptr);
                            _exit(127);
                        }
                        busy = child > 0; failed = child < 0;
                        Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                        vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    }
                    continue;
                }
                if (kStable.Contains(x, y) || kDev.Contains(x, y)) {
                    const std::string target = kDev.Contains(x, y) ? "dev" : "stable";
                    if (target != updateChannel) {
                        child = fork();
                        if (child == 0) {
                            const std::string cli = Cli().string();
                            execl(cli.c_str(), cli.c_str(), "update-channel", target.c_str(), (char *)nullptr);
                            _exit(127);
                        }
                        busy = child > 0; failed = child < 0;
                        if (busy) { updateChannel = target; updateStatus = "CHECK FOR UPDATES"; }
                        Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                        vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    }
                    continue;
                }
                if (kUpdate.Contains(x, y)) {
                    const bool install = updateStatus.rfind("AVAILABLE ", 0) == 0;
                    const std::string version = install ? updateStatus.substr(10) : "";
                    if (install) {
                        updateVersion = version;
                        const auto shown = vr::VROverlay()->ShowKeyboardForOverlay(
                            mainOverlay, vr::k_EGamepadTextInputModePassword,
                            vr::k_EGamepadTextInputLineModeSingleLine, vr::KeyboardFlag_Modal,
                            "STEAMOS PASSWORD FOR FRAMEHANDS UPDATE", 127, "", 0);
                        awaitingPassword = shown == vr::VROverlayError_None;
                        updateStatus = awaitingPassword ? "ENTER STEAMOS PASSWORD" : "KEYBOARD UNAVAILABLE";
                        Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                        vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                        continue;
                    }
                    updateStatus = "CHECKING";
                    child = fork();
                    if (child == 0) {
                        const std::string updater = Updater().string();
                        execlp("python3", "python3", updater.c_str(), "check", (char *)nullptr);
                        _exit(127);
                    }
                    updateChild = child > 0;
                    busy = child > 0; failed = child < 0;
                    Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                // Slider changes are committed on mouse-up, not on every move.
                if (kSlider.Contains(x, y)) {
                    sliding = true;
                    smoothness = SmoothnessAt(x);
                    nextSliderDraw = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                    Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
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
                    Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
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
                    Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (kGamePause.Contains(x, y)) {
                    child = fork();
                    if (child == 0) {
                        const std::string cli = Cli().string();
                        execl(cli.c_str(), cli.c_str(), gamePause ? "games-off" : "games-on", (char *)nullptr);
                        _exit(127);
                    }
                    busy = child > 0; failed = child < 0;
                    Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
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
                Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event)) {
            if (event.eventType == vr::VREvent_KeyboardDone ||
                event.eventType == vr::VREvent_KeyboardClosed ||
                event.eventType == vr::VREvent_KeyboardClosed_Global) passwordEvent(event.eventType);
            if (event.eventType == vr::VREvent_Quit ||
                event.eventType == vr::VREvent_OverlayClosed) running = false;
        }
        if (vr::VROverlay()->IsActiveDashboardOverlay(mainOverlay)) wasActive = true;
        if (!running) break;
        if (child > 0) {
            int status = 0;
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child || done < 0) {
                child = -1;
                busy = false;
                failed = done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
                if (updateChild) {
                    ReadUpdateStatus();
                    updateChild = false;
                }
                enabled = Enabled();
                visuals = VisualsEnabled();
                touch = TouchEnabled();
                gamePause = GamePauseEnabled();
                pausedState = GamePaused();
                smoothness = Smoothness();
                handColour = HandColour();
                updateChannel = UpdateChannel();
                Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        if (std::chrono::steady_clock::now() >= next && !busy) {
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            const bool current = Enabled(), currentVisuals = VisualsEnabled(), currentTouch = TouchEnabled();
            const bool currentGamePause = GamePauseEnabled(), currentPaused = GamePaused();
            const int currentSmooth = Smoothness();
            const std::string currentColour = HandColour();
            const std::string currentChannel = UpdateChannel();
            if (!sliding && (current != enabled || currentVisuals != visuals || currentTouch != touch ||
                             currentGamePause != gamePause || currentPaused != pausedState || currentSmooth != smoothness ||
                             currentColour != handColour || currentChannel != updateChannel)) {
                enabled = current; visuals = currentVisuals; touch = currentTouch;
                gamePause = currentGamePause; pausedState = currentPaused; smoothness = currentSmooth;
                handColour = currentColour;
                updateChannel = currentChannel;
                Paint(enabled, visuals, touch, gamePause, smoothness, busy, failed);
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
