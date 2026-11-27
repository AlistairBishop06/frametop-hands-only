// On-demand SteamVR dashboard overlay, launched from the + menu.
// A separate oneshot systemd unit registers the app; this UI is never auto-started.
// Closing the UI destroys the overlay but does NOT change hand-tracking settings.
#include <openvr.h>
#include "../config/hand_calibration.h"
#include "../config/interaction_mode.h"
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
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "assets/stb_truetype.h"

namespace {
using framehands::InteractionMode;
using framehands::ReadInteractionMode;
constexpr int W = 1280, H = 1580;
struct RGB { uint8_t r, g, b; };
struct Box {
    int x0, y0, x1, y1;
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
constexpr Box kClose{1144, 56, 1224, 136};
constexpr Box kTracking{56, 184, 1224, 376};
constexpr Box kCalibration{886, 395, 1224, 449};
constexpr Box kCalibrationStart{88, 660, 550, 740};
constexpr Box kCalibrationReset{624, 660, 1192, 740};
constexpr Box kVisuals{56, 460, 1224, 566};
constexpr Box kGamePause{56, 672, 1224, 778};
struct InteractionOption {
    InteractionMode mode;
    const char *value;
    const char *label;
    Box box;
};
constexpr InteractionOption kInteractionOptions[] = {
    {InteractionMode::Aim, "aim", "Aim & Pinch", {88, 616, 446, 661}},
    {InteractionMode::Touch, "touch", "Direct Touch", {460, 616, 818, 661}},
    {InteractionMode::Gaze, "gaze", "Gaze Assisted", {832, 616, 1192, 661}},
};
constexpr Box kSlider{88, 936, 1192, 990};
constexpr Box kColour{80, 1102, 1200, 1182};
constexpr Box kStable{780, 1306, 982, 1370};
constexpr Box kDev{986, 1306, 1188, 1370};
constexpr Box kUpdate{844, 1425, 1192, 1501};
constexpr int kSliderLeft = 88, kSliderRight = 1192;
std::vector<uint8_t> pixels(W * H * 4);
std::string updateStatus = "CHECK FOR UPDATES";
std::string handColour = "white";
std::string updateChannel = "stable";
constexpr const char *kColourNames[] = {"red", "orange", "yellow", "green", "blue", "purple", "black", "white"};
constexpr RGB kColourSwatches[] = {{226, 98, 93}, {240, 159, 80}, {235, 207, 95}, {109, 185, 131},
                                   {101, 159, 223}, {171, 128, 203}, {11, 17, 19}, {241, 245, 245}};

std::vector<uint8_t> fontBytes;
stbtt_fontinfo fontInfo{};
bool fontReady = false;
void LoadFont() {
    const auto path = std::filesystem::read_symlink("/proc/self/exe")
        .parent_path().parent_path() / "assets/InterVariable.ttf";
    std::ifstream file(path, std::ios::binary);
    fontBytes.assign(std::istreambuf_iterator<char>(file), {});
    fontReady = !fontBytes.empty() && stbtt_InitFont(&fontInfo, fontBytes.data(), 0);
    if (!fontReady) std::cerr << "framehands-dashboard: Inter font unavailable: " << path << '\n';
}
// Retained only as a fallback if the packaged font is unavailable.
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
    if (fontReady) {
        const float scale = stbtt_ScaleForPixelHeight(&fontInfo, float(size));
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &gap);
        const int baseline = y + int(std::lround(ascent * scale));
        float pen = float(x0);
        for (size_t k = 0; k < s.size(); ++k) {
            const unsigned char ch = static_cast<unsigned char>(s[k]);
            int bw = 0, bh = 0, ox = 0, oy = 0;
            unsigned char *bitmap = stbtt_GetCodepointBitmap(&fontInfo, 0, scale, ch, &bw, &bh, &ox, &oy);
            for (int row = 0; row < bh; ++row) for (int col = 0; col < bw; ++col) {
                const int px = int(std::lround(pen)) + ox + col, py = baseline + oy + row;
                if (px < 0 || px >= W || py < 0 || py >= H) continue;
                const int alpha = bitmap[row * bw + col];
                const size_t at = size_t((py * W + px) * 4);
                pixels[at] = uint8_t((pixels[at] * (255 - alpha) + c.r * alpha) / 255);
                pixels[at + 1] = uint8_t((pixels[at + 1] * (255 - alpha) + c.g * alpha) / 255);
                pixels[at + 2] = uint8_t((pixels[at + 2] * (255 - alpha) + c.b * alpha) / 255);
            }
            stbtt_FreeBitmap(bitmap, nullptr);
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&fontInfo, ch, &advance, &bearing);
            pen += advance * scale;
        }
        return;
    }
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
    RoundRect({x, y, x+136, y+64}, 32, on ? RGB{113,215,197} : RGB{71,86,93});
    Circle(x + (on ? 104 : 32), y+32, 26, {11,32,30});
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

struct CalibrationPanel {
    bool open = false;
    uint64_t token = 0;
    std::string state = "ready";
    int progress = 0;
    std::filesystem::path request = hand_calibration::Runtime() / "hand-calibration-request";
    std::filesystem::path status = hand_calibration::Runtime() / "hand-calibration-status";

    bool HasProfile() const {
        std::error_code error;
        return std::filesystem::exists(hand_calibration::Profile(), error);
    }

    void End() {
        if (token) unlink(request.c_str());
        token = 0;
    }

    void Start(const char *command) {
        progress = 0;
        const auto now = hand_calibration::Now();
        if (hand_calibration::Write(request, std::to_string(now) + " " + command + "\n")) {
            token = now;
            state = "waiting";
        } else state = "error";
    }

    bool Refresh() {
        if (!token) return false;
        std::ifstream in(status);
        uint64_t id = 0;
        std::string next;
        int percent = 0;
        if (!(in >> id >> next >> percent) || id != token) next.clear();
        const uint64_t elapsed = hand_calibration::Now() - token;
        if (next.empty() && state == "waiting" && elapsed >= 8'000'000'000) next = "unavailable";
        if ((next.empty() || next == "left" || next == "right") &&
            elapsed >= hand_calibration::kTimeoutNs) next = "timeout";
        if (next.empty() || (next == state && percent == progress)) return false;
        state = next;
        progress = std::clamp(percent, 0, 100);
        if (state != "left" && state != "right") End();
        return true;
    }
} calibration;

void PaintCalibration(bool tracking) {
    const RGB ink{241,245,245}, muted{171,185,189}, teal{113,215,197};
    TextAt(56, 49, "Calibrate hands", 46, ink);
    TextAt(56, 113, "Save a hand shape for each hand.", 24, muted);
    RoundRect(kClose, 20, {39,49,55});
    TextAt(1172, 65, "x", 48, ink);
    RoundRect({56, 184, 1224, 790}, 24, {26,34,39});

    const bool measuring = calibration.state == "left" || calibration.state == "right";
    std::string title = "Fit the skeleton to your hands";
    std::string first = "You will show one open hand at a time.";
    std::string second = "Keep your fingers straight and comfortably spread.";
    if (measuring) {
        title = calibration.state == "left" ? "Open your left hand" : "Open your right hand";
        first = "Hold it in front of you with your fingers spread.";
        second = "Turn your wrist slightly, keeping your fingers straight.";
    } else if (calibration.state == "waiting") {
        title = "Waiting for hand tracking";
        first = "Keep the dashboard open.";
        second.clear();
    } else if (calibration.state == "saved") {
        title = "Hand calibration saved";
        first = "Try pointing and pinching with each hand.";
        second = "FrameHands will reuse this fit when tracking starts.";
    } else if (calibration.state == "reset") {
        title = "Automatic calibration restored";
        first = "FrameHands will learn your hand shape as you use it.";
        second.clear();
    } else if (calibration.state == "timeout") {
        title = "Calibration timed out";
        first = "Try again with each hand clearly in front of you.";
        second = "Turn tracking on first if it is paused.";
    } else if (calibration.state == "unavailable") {
        title = "The hand tracker did not respond";
        first = "Check that hand tracking is running and try again.";
        second.clear();
    } else if (calibration.state == "error") {
        title = "Could not finish calibration";
        first = "Try again.";
        second.clear();
    }
    if (!tracking && !calibration.token) {
        first = "Turn on hand tracking before starting.";
        second.clear();
    }

    TextAt(88, 243, title, 40, ink);
    TextAt(88, 338, first, 27, muted);
    TextAt(88, 385, second, 27, muted);
    if (measuring) {
        RoundRect({88, 509, 1040, 531}, 10, {71,86,93});
        const int fill = 952 * calibration.progress / 100;
        if (fill > 0) RoundRect({88, 509, 88 + fill, 531}, 10, teal);
        TextAt(1080, 497, std::to_string(calibration.progress) + "%", 30, teal);
    }
    RoundRect(kCalibrationStart, 16, tracking || calibration.token ? RGB{44,73,69} : RGB{39,49,55});
    TextAt(120, 683, calibration.token ? "Cancel" : "Start calibration", 29, ink);
    if (!calibration.token && calibration.HasProfile()) {
        RoundRect(kCalibrationReset, 16, {39,49,55});
        TextAt(655, 683, "Restore automatic calibration", 28, ink);
    }
}

void Paint(bool enabled, bool visuals, InteractionMode mode, bool gamePause, int smoothness, bool busy, bool failed) {
    const RGB ink{241,245,245}, muted{171,185,189}, teal{113,215,197};
    Rect(0, 0, W, H, {17,21,23});
    if (calibration.open) {
        PaintCalibration(enabled && !GamePaused());
        return;
    }
    TextAt(56, 49, "FrameHands", 46, ink);
    TextAt(56, 113, "Hand tracking for Steam Frame", 24, muted);
    RoundRect(kClose, 20, {39,49,55});
    TextAt(1172, 65, "x", 48, ink);

    const bool paused = gamePause && GamePaused();
    RoundRect(kTracking, 24, {59,112,104});
    RoundRect({58,186,1222,374}, 22, {25,45,45});
    TextAt(92, 214, "Hand Tracking", 36, ink);
    Circle(100, 300, 6, paused ? RGB{244,190,105} : teal);
    TextAt(122, 278, paused ? "Paused for VR game" : failed ? "Check logs" :
           busy ? "Applying change" : enabled ? "Tracking active" : "Tracking off",
           26, paused ? RGB{244,190,105} : teal);
    Toggle(1044, 240, enabled);

    TextAt(56, 408, "Interaction", 26, muted);
    RoundRect(kCalibration, 14, {39,49,55});
    TextAt(923, 407, "Calibrate hands", 25, ink);
    RoundRect({56,460,1224,778}, 24, {52,65,71});
    RoundRect({58,462,1222,776}, 22, {26,34,39});
    Rect(88, 566, 1192, 567, {52,65,71});
    Rect(88, 672, 1192, 673, {52,65,71});
    TextAt(88, 480, "Hand Visuals", 28, ink);
    TextAt(88, 524, "Show the hand overlay.", 22, muted);
    Toggle(1048, 473, visuals);
    TextAt(88, 576, "Interaction Mode", 28, ink);
    if (mode == InteractionMode::Gaze)
        TextAt(430, 581, "Turn your head and pinch to click.", 22, muted);
    for (const auto &option : kInteractionOptions) {
        const bool selected = mode == option.mode;
        RoundRect(option.box, 12, selected ? RGB{49,78,74} : RGB{39,49,55});
        TextAt(option.box.x0 + 20, option.box.y0 + 8, option.label, 24, selected ? teal : ink);
    }
    TextAt(88, 692, "Pause During VR Games", 28, ink);
    TextAt(88, 736, "Pause tracking while a VR game is running.", 22, muted);
    Toggle(1048, 685, gamePause);

    TextAt(56, 810, "Appearance", 26, muted);
    RoundRect({56,862,1224,1194}, 24, {52,65,71});
    RoundRect({58,864,1222,1192}, 22, {26,34,39});
    TextAt(88, 883, "Smoothness", 28, ink);
    TextAt(1115, 880, std::to_string(smoothness), 32, teal);
    RoundRect({88,962,1192,970}, 4, {71,86,93});
    const int knob = kSliderLeft + (kSliderRight-kSliderLeft) * std::clamp(smoothness, 0, 100) / 100;
    if (knob > kSliderLeft) RoundRect({88,962,knob,970}, 4, teal);
    Circle(knob, 966, 24, teal);
    Circle(knob, 966, 20, {241,245,245});
    TextAt(88, 990, "0", 22, muted);
    TextAt(1150, 990, "100", 22, muted);
    Rect(88, 1036, 1192, 1037, {52,65,71});
    TextAt(88, 1055, "Hand Colour", 28, ink);
    std::string colour = handColour;
    if (!colour.empty()) colour[0] = char(std::toupper(static_cast<unsigned char>(colour[0])));
    TextAt(1090, 1058, colour, 24, muted);
    for (int i = 0; i < 8; ++i) {
        const int cx = 142 + 140 * i;
        if (handColour == kColourNames[i]) Circle(cx, 1142, 39, teal);
        Circle(cx, 1142, 30, kColourSwatches[i]);
        if (i == 6) { Circle(cx, 1142, 29, {11,17,19}); }
    }

    TextAt(56, 1226, "Updates", 26, muted);
    RoundRect({56,1278,1224,1532}, 24, {52,65,71});
    RoundRect({58,1280,1222,1530}, 22, {26,34,39});
    TextAt(88, 1312, "Release channel", 28, ink);
    RoundRect({776,1302,1192,1374}, 18, {16,23,27});
    RoundRect(updateChannel == "dev" ? kDev : kStable, 14, {49,78,74});
    TextAt(837, 1318, "Stable", 28, updateChannel == "stable" ? ink : muted);
    TextAt(1052, 1318, "Dev", 28, updateChannel == "dev" ? ink : muted);
    Rect(88, 1396, 1192, 1397, {52,65,71});
    const bool available = updateStatus.rfind("AVAILABLE ", 0) == 0;
    const std::string title = failed ? "Update failed" : available ? "Update available" :
        updateStatus == "CHECKING" ? "Checking for updates" :
        updateStatus.rfind("ERROR", 0) == 0 ? "Update check failed" :
        updateStatus == "RESTART HEADSET" ? "Restart headset" : "No update available";
    TextAt(88, 1423, title, 28, ink);
    std::string detail = available ? updateStatus.substr(10) :
        updateStatus == "CHECK FOR UPDATES" ? "Check for the latest release." :
        updateStatus == "UP TO DATE" ? "You're on the latest " + updateChannel + " version." :
        updateStatus;
    if (detail.size() > 45) detail.resize(45);
    TextAt(88, 1468, detail, 22, muted);
    RoundRect(kUpdate, 16, {82,120,111});
    RoundRect({846,1427,1190,1499}, 14, {44,73,69});
    TextAt(available ? 954 : 895, 1447, available ? "Download" : "Check for Updates", 26, ink);
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
    LoadFont();
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

    bool enabled = Enabled(), visuals = VisualsEnabled();
    InteractionMode mode = ReadInteractionMode();
    bool gamePause = GamePauseEnabled(), pausedState = GamePaused(), busy = false, failed = false;
    int smoothness = Smoothness();
    handColour = HandColour();
    updateChannel = UpdateChannel();
    bool sliding = false, updateChild = false, awaitingPassword = false;
    std::string updateVersion;
    pid_t child = -1;
    auto next = std::chrono::steady_clock::now();
    auto nextSliderDraw = next;
    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
        Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                        Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const float x = event.data.mouse.x;
                const float y = H - event.data.mouse.y; // GL mouse coordinates are bottom-left.
                if (calibration.open || kCalibration.Contains(x, y)) {
                    if (!calibration.open) calibration.open = true, calibration.state = "ready";
                    else if (kClose.Contains(x, y)) calibration.End(), calibration.open = false;
                    else if (kCalibrationStart.Contains(x, y)) {
                        if (calibration.token) calibration.End(), calibration.state = "ready";
                        else if (enabled && !GamePaused()) calibration.Start("capture");
                    } else if (kCalibrationReset.Contains(x, y) && !calibration.token &&
                               enabled && !GamePaused() && calibration.HasProfile()) calibration.Start("reset");
                    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                if (kClose.Contains(x, y)) {
                    running = false;
                    break;
                }
                if (kColour.Contains(x, y)) {
                    const int slot = std::clamp((int(x) - 72) / 140, 0, 7);
                    if (y >= 1102 && y < 1182) {
                        child = fork();
                        if (child == 0) {
                            const std::string cli = Cli().string();
                            execl(cli.c_str(), cli.c_str(), "colour", kColourNames[slot], (char *)nullptr);
                            _exit(127);
                        }
                        busy = child > 0; failed = child < 0;
                        Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                        Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                        Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                // Slider changes are committed on mouse-up, not on every move.
                if (kSlider.Contains(x, y)) {
                    sliding = true;
                    smoothness = SmoothnessAt(x);
                    nextSliderDraw = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
                    continue;
                }
                const auto interaction = std::find_if(std::begin(kInteractionOptions), std::end(kInteractionOptions),
                    [x, y](const InteractionOption &option) { return option.box.Contains(x, y); });
                if (interaction != std::end(kInteractionOptions)) {
                    child = fork();
                    if (child == 0) {
                        const std::string cli = Cli().string();
                        execl(cli.c_str(), cli.c_str(), "interaction-mode", interaction->value, (char *)nullptr);
                        _exit(127);
                    }
                    busy = child > 0; failed = child < 0;
                    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                    Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
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
                mode = ReadInteractionMode();
                gamePause = GamePauseEnabled();
                pausedState = GamePaused();
                smoothness = Smoothness();
                handColour = HandColour();
                updateChannel = UpdateChannel();
                Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        if (std::chrono::steady_clock::now() >= next && !busy) {
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            const bool current = Enabled(), currentVisuals = VisualsEnabled();
            const auto currentMode = ReadInteractionMode();
            const bool currentGamePause = GamePauseEnabled(), currentPaused = GamePaused();
            const int currentSmooth = Smoothness();
            const std::string currentColour = HandColour();
            const std::string currentChannel = UpdateChannel();
            const bool calibrationChanged = calibration.open && calibration.Refresh();
            if (!sliding && (calibrationChanged || current != enabled || currentVisuals != visuals || currentMode != mode ||
                             currentGamePause != gamePause || currentPaused != pausedState || currentSmooth != smoothness ||
                             currentColour != handColour || currentChannel != updateChannel)) {
                enabled = current; visuals = currentVisuals; mode = currentMode;
                gamePause = currentGamePause; pausedState = currentPaused; smoothness = currentSmooth;
                handColour = currentColour;
                updateChannel = currentChannel;
                Paint(enabled, visuals, mode, gamePause, smoothness, busy, failed);
                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    calibration.End();
    vr::VROverlay()->DestroyOverlay(mainOverlay);
    vr::VROverlay()->DestroyOverlay(thumb);
    vr::VR_Shutdown();
    return 0;
}
