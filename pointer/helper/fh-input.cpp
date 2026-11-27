// Dashboard and undocked-panel pointer. The SteamVR driver remains disconnected
// until a bare hand is available for overlay input; no game input is synthesized.
#include "../../config/ftsock.h"
#include "../../config/interaction_mode.h"
#include "../../renderer/handcut.h"
#include "gaze_pointer.h"
#include "gesture_rules.h"
#include "pointer_aim.h"

#include <openvr.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>

namespace {
using Clock = std::chrono::steady_clock;
using framehands::InteractionMode;
using framehands::ReadInteractionMode;
using pointer_pose::AimMoved;
using pointer_pose::AimRay;
using pointer_pose::AimRotation;
using pointer_pose::Dot;
using pointer_pose::GazeButton;
using pointer_pose::GazeClick;
using pointer_pose::GazeCursorDepth;
using pointer_pose::GazeHand;
using pointer_pose::GazeScreenRay;
using pointer_pose::GrabPose;
using pointer_pose::GrabSample;
using pointer_pose::HandAim;
using pointer_pose::HeadGazeRay;
using pointer_pose::Length;
using pointer_pose::PointerPose;
using pointer_pose::Quaternion;
using pointer_pose::V;
using pointer_pose::WristRotation;
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
V Position(const vr::HmdMatrix34_t &m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }
V Point(const float p[3]) { return {p[0], p[1], p[2]}; }
V Local(const vr::HmdMatrix34_t &m, V p) {
    return {m.m[0][3]+m.m[0][0]*p.x+m.m[0][1]*p.y+m.m[0][2]*p.z,
            m.m[1][3]+m.m[1][0]*p.x+m.m[1][1]*p.y+m.m[1][2]*p.z,
            m.m[2][3]+m.m[2][0]*p.x+m.m[2][1]*p.y+m.m[2][2]*p.z};
}
V Rotate(const vr::HmdMatrix34_t &m, V p) {
    return {m.m[0][0]*p.x+m.m[0][1]*p.y+m.m[0][2]*p.z,
            m.m[1][0]*p.x+m.m[1][1]*p.y+m.m[1][2]*p.z,
            m.m[2][0]*p.x+m.m[2][1]*p.y+m.m[2][2]*p.z};
}
V Unrotate(const vr::HmdMatrix34_t &m, V p) {
    return {m.m[0][0]*p.x+m.m[1][0]*p.y+m.m[2][0]*p.z,
            m.m[0][1]*p.x+m.m[1][1]*p.y+m.m[2][1]*p.z,
            m.m[0][2]*p.x+m.m[1][2]*p.y+m.m[2][2]*p.z};
}
bool Send(int fd, const char *message) {
    sockaddr_un addr;
    return sendto(fd, message, std::strlen(message), MSG_NOSIGNAL,
                  reinterpret_cast<sockaddr *>(&addr), ft_sock_addr(FT_SOCK_POINTER, &addr)) >= 0;
}

void SendPoses(int fd, const PointerPose &laser, const PointerPose &device) {
    const auto &l = laser.origin;
    const auto &q = laser.rotation;
    const auto &p = device.origin;
    const auto &r = device.rotation;
    char command[256];
    std::snprintf(command, sizeof command,
                  "posq %.5f %.5f %.5f %.8f %.8f %.8f %.8f %.5f %.5f %.5f %.8f %.8f %.8f %.8f",
                  l.x, l.y, l.z, q.w, q.x, q.y, q.z, p.x, p.y, p.z, r.w, r.x, r.y, r.z);
    Send(fd, command);
}

void ReportGrabPose(bool right, bool dashboard, double wristAngle, Quaternion sent,
                    const vr::TrackedDevicePose_t *poses) {
    const auto role = right ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
    const auto device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(role);
    char type[64]{};
    double poseError = -1;
    if (device < vr::k_unMaxTrackedDeviceCount) {
        vr::VRSystem()->GetStringTrackedDeviceProperty(device, vr::Prop_ControllerType_String, type, sizeof type);
        if (poses[device].bPoseIsValid && !std::strcmp(type, "fh_pointer")) {
            const auto &m = poses[device].mDeviceToAbsoluteTracking.m;
            double trace = 0;
            for (int i = 0; i < 3; ++i) {
                const Quaternion axis{0, double(i == 0), double(i == 1), double(i == 2)};
                const Quaternion expected = sent * axis * sent.Conjugate();
                trace += Dot({m[0][i], m[1][i], m[2][i]}, {expected.x, expected.y, expected.z});
            }
            poseError = std::acos(std::clamp((trace - 1) / 2, -1.0, 1.0)) * 180 / M_PI;
        }
    }
    std::fprintf(stderr, "fh-input grab: hand=%s dashboard=%d wrist=%.1fdeg controller=%s pose_error=%.1fdeg\n",
                 right ? "right" : "left", int(dashboard), wristAngle * 180 / M_PI,
                 type[0] ? type : "unassigned", poseError);
}

struct Controller {
    V last{};
    int64_t at = 0, busyUntil = 0;
};
bool PhysicalControllerBusy(const vr::TrackedDevicePose_t *poses, const handcut::Hand &hand,
                            Controller *state, int64_t now) {
    V palm; double p[3]; hand.Palm(p); palm = {p[0], p[1], p[2]};
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
        char type[64] = "";
        vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
        if (!std::strcmp(type, "fh_pointer")) continue;
        const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);
        if (role != (hand.right ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand)) continue;
        if (!poses[i].bPoseIsValid || poses[i].eTrackingResult != vr::TrackingResult_Running_OK) continue;
        V pos = Position(poses[i].mDeviceToAbsoluteTracking);
        if (Length(pos-palm) < 0.15) state[i].busyUntil = now + 700'000'000ll;
        if (state[i].at && now-state[i].at < 250'000'000ll && Length(pos-state[i].last) > 0.025)
            state[i].busyUntil = now + 700'000'000ll;
        state[i].last = pos; state[i].at = now;
        if (now < state[i].busyUntil) return true;
    }
    return false;
}

struct GazeControllers {
    bool busy = false;
    int freeSide = 1;
    vr::TrackedDeviceIndex_t pointer = vr::k_unTrackedDeviceIndexInvalid;
};

GazeControllers CheckGazeControllers(const vr::TrackedDevicePose_t *poses, Controller *state, int64_t now) {
    GazeControllers result;
    bool physical[2]{};
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (!poses[i].bPoseIsValid || poses[i].eTrackingResult != vr::TrackingResult_Running_OK ||
            vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
        char type[64]{};
        vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
        if (!std::strcmp(type, "fh_pointer")) {
            result.pointer = i;
            continue;
        }
        const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);
        if (role == vr::TrackedControllerRole_LeftHand) physical[0] = true;
        if (role == vr::TrackedControllerRole_RightHand) physical[1] = true;
        vr::VRControllerState_t input{};
        const bool pressed = vr::VRSystem()->GetControllerState(i, &input, sizeof input) && input.ulButtonPressed;
        const auto &velocity = poses[i].vVelocity.v;
        const auto &angular = poses[i].vAngularVelocity.v;
        if (pressed || Length({velocity[0], velocity[1], velocity[2]}) > 0.07 ||
            Length({angular[0], angular[1], angular[2]}) > 0.20)
            state[i].busyUntil = now + 1'500'000'000ll;
        result.busy = result.busy || now < state[i].busyUntil;
    }
    if (physical[1] && !physical[0]) result.freeSide = 0;
    return result;
}

bool FloatingGazeAllowed() {
    auto *apps = vr::VRApplications();
    if (!apps) return false;
    const auto state = apps->GetSceneApplicationState();
    if (state == vr::EVRSceneApplicationState_Starting || state == vr::EVRSceneApplicationState_Quitting)
        return false;
    const uint32_t pid = apps->GetCurrentSceneProcessId();
    if (!pid) return state == vr::EVRSceneApplicationState_None;
    char key[256]{};
    if (apps->GetApplicationKeyByProcessId(pid, key, sizeof key) != vr::VRApplicationError_None) return false;
    // A persistent scene can be a VR game or the Steam Link client. Outside
    // the dashboard, claim the gaze cursor only in the shell or SteamVR Home.
    return !std::strcmp(key, "steam.app.250820") || !std::strcmp(key, "openvr.tool.steamvr_environments");
}

class GazeSurfaces {
public:
    void Refresh(const std::vector<std::string> &keys, const std::vector<vr::VROverlayHandle_t> &observed) {
        panels_.clear();
        auto *overlays = vr::VROverlay();
        const auto add = [&](vr::VROverlayHandle_t panel) {
            if (panel == vr::k_ulOverlayHandleInvalid || !overlays->IsOverlayVisible(panel) ||
                std::find(panels_.begin(), panels_.end(), panel) != panels_.end()) return;
            vr::VROverlayInputMethod method = vr::VROverlayInputMethod_None;
            if ((overlays->GetOverlayInputMethod(panel, &method) == vr::VROverlayError_None &&
                 method == vr::VROverlayInputMethod_Mouse) || overlays->IsHoverTargetOverlay(panel))
                panels_.push_back(panel);
        };
        const auto find = [&](const char *key) {
            vr::VROverlayHandle_t panel = vr::k_ulOverlayHandleInvalid;
            if (overlays->FindOverlay(key, &panel) == vr::VROverlayError_None) add(panel);
        };
        for (const auto &key : keys) find(key.c_str());
        for (const auto panel : observed) add(panel);
        for (const auto *key : {"system.dashboard", "system.dashboard.nowplaying",
                                "system.dashboard.quicklaunch", "system.desktop", "system.keyboard",
                                "valve.steam.gamepadui.keyboard"})
            find(key);
        cursor_ = vr::k_ulOverlayHandleInvalid;
        overlays->FindOverlay("system.pointer", &cursor_);
    }

    std::optional<double> Depth(AimRay ray, const vr::HmdMatrix34_t &rawHead,
                                vr::TrackedDeviceIndex_t pointer) const {
        auto *overlays = vr::VROverlay();
        auto *compositor = vr::VRCompositor();
        const auto raw = vr::TrackingUniverseRawAndUncalibrated;
        vr::VROverlayIntersectionParams_t query{};
        query.eOrigin = compositor ? compositor->GetTrackingSpace() : raw;
        vr::TrackedDevicePose_t spaceHead{};
        AimRay queryRay = ray;
        if (query.eOrigin != raw) {
            vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(query.eOrigin, 0, &spaceHead, 1);
            if (spaceHead.bPoseIsValid) {
                const auto &head = spaceHead.mDeviceToAbsoluteTracking;
                queryRay.origin = Local(head, Unrotate(rawHead, ray.origin - Position(rawHead)));
                queryRay.direction = Rotate(head, Unrotate(rawHead, ray.direction));
            } else query.eOrigin = raw;
        }
        query.vSource = {{float(queryRay.origin.x), float(queryRay.origin.y), float(queryRay.origin.z)}};
        query.vDirection = {{float(queryRay.direction.x), float(queryRay.direction.y), float(queryRay.direction.z)}};
        std::optional<double> nearest;
        vr::VROverlayHandle_t nearestPanel = vr::k_ulOverlayHandleInvalid;
        for (const auto panel : panels_) {
            vr::VROverlayIntersectionResults_t hit{};
            if (overlays->ComputeOverlayIntersection(panel, &query, &hit) &&
                std::isfinite(hit.fDistance) && hit.fDistance > 0 &&
                (!nearest || hit.fDistance < *nearest) && overlays->IsOverlayVisible(panel)) {
                nearest = hit.fDistance;
                nearestPanel = panel;
            }
        }
        if (nearest && overlays->IsHoverTargetOverlay(nearestPanel)) return nearest;
        // Scene-graph panels can expose only the compositor's native dot.
        // Avoid a cursor from a physical controller, or one at an old aim.
        if (pointer != vr::k_unTrackedDeviceIndexInvalid && overlays->GetPrimaryDashboardDevice() == pointer &&
            cursor_ != vr::k_ulOverlayHandleInvalid && overlays->IsOverlayVisible(cursor_)) {
            vr::ETrackingUniverseOrigin cursorSpace{};
            vr::HmdMatrix34_t transform{};
            if (overlays->GetOverlayTransformAbsolute(cursor_, &cursorSpace, &transform) == vr::VROverlayError_None) {
                V cursor = Position(transform);
                if (cursorSpace != raw) {
                    if (cursorSpace != query.eOrigin)
                        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(cursorSpace, 0, &spaceHead, 1);
                    if (!spaceHead.bPoseIsValid) return std::nullopt;
                    const auto &head = spaceHead.mDeviceToAbsoluteTracking;
                    cursor = Local(rawHead, Unrotate(head, cursor - Position(head)));
                }
                const auto depth = GazeCursorDepth(ray, cursor);
                if (depth && (!nearest || *depth <= *nearest + 0.006)) return depth;
            }
        }
        // Keep the full hit test when SteamVR hasn't confirmed this surface.
        // Starting at a guessed distance could skip a closer target.
        return std::nullopt;
    }

private:
    std::vector<vr::VROverlayHandle_t> panels_;
    vr::VROverlayHandle_t cursor_ = vr::k_ulOverlayHandleInvalid;
};
}

// Steam Frame's floated gamescope panels use keys such as
// valve.steam.desktopgame.<appid>, not only system.desktop.1. SteamVR does not
// provide public enumeration of every overlay, so discover keys in SteamVR's
// own settings and recent Steam client log, plus optional user-supplied keys.
std::vector<std::string> PointerOverlayKeys() {
    std::vector<std::string> keys = {
        "framehands.dashboard", "system.desktop.1", "system.desktop.2",
        "system.settings", "valve.steam.desktop", "valve.steam.bigpicture",
        "valve.steam.gamepadui.bootstrapper"
    };
    auto add = [&](const std::string &key) {
        if (!key.empty() && key.size() < 160 && keys.size() < 64 &&
            std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    };
    // Allows users to target another app's known overlay key without rebuilding.
    std::istringstream extra(handcut::Setting("HANDS_TOUCH_OVERLAY_KEYS", ""));
    std::string key;
    while (extra >> key) add(key);

    const char *home = std::getenv("HOME");
    if (!home) return keys;
    const std::filesystem::path base(home);
    for (const auto &path : {
             base / ".config/openvr/config/steamvr.vrsettings",
             base / ".local/share/Steam/config/steamvr.vrsettings",
             base / ".local/share/Steam/logs/console_log.txt"
         }) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) continue;
        stream.seekg(0, std::ios::end);
        const std::streamoff size = stream.tellg();
        if (size <= 0) continue;
        const auto count = static_cast<size_t>(std::min<std::streamoff>(size, 262144));
        std::string content(count, '\0');
        stream.seekg(size - static_cast<std::streamoff>(count), std::ios::beg);
        if (!stream.read(content.data(), static_cast<std::streamsize>(count))) continue;

        const std::string lastKey = "lastAccessedExternalOverlayKey";
        if (const auto i = content.rfind(lastKey); i != std::string::npos) {
            const auto colon = content.find(':', i + lastKey.size());
            if (colon != std::string::npos) {
                const auto q1 = content.find('"', colon + 1);
                const auto q2 = q1 == std::string::npos ? q1 : content.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) add(content.substr(q1 + 1, q2 - q1 - 1));
            }
        }
        // Gamescope's OpenVR backend creates keys under gamescope.<display>.
        // Float-in-world panels can use these instead of the dashboard keys.
        for (const std::string prefix : {"valve.steam.desktopgame.", "gamescope."}) {
            size_t i = 0;
            while ((i = content.find(prefix, i)) != std::string::npos) {
                const size_t begin = i;
                i += prefix.size();
                const size_t suffix = i;
                while (i < content.size() &&
                       ((content[i] >= '0' && content[i] <= '9') ||
                        (prefix == "gamescope." &&
                         ((content[i] >= 'a' && content[i] <= 'z') ||
                          (content[i] >= 'A' && content[i] <= 'Z') ||
                          content[i] == '_' || content[i] == '-' || content[i] == '.')))) ++i;
                if (i > suffix) add(content.substr(begin, i - begin));
            }
        }
    }
    return keys;
}

int main() {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) return std::fprintf(stderr, "fh-input: %s\n",
        vr::VR_GetVRInitErrorAsEnglishDescription(error)), 1;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::perror("fh-input socket"), 1;
    handcut::Hands hands;
    HandAim aim;
    GazeClick gazeClick;
    pointer_pose::TwoFistMenuGesture menuGesture;
    bool twoFistMenu = framehands::TwoFistMenuEnabled();
    bool interactionEnabled = framehands::InteractionEnabled();
    GazeSurfaces gazeSurfaces;
    Controller controllers[vr::k_unMaxTrackedDeviceCount]{};
    bool floatPointer = handcut::Setting("HANDS_FLOATING_POINTER", "1") != "0";
    InteractionMode mode = ReadInteractionMode();
    bool touchOn = mode == InteractionMode::Touch;
    bool touchDebug = handcut::Setting("HANDS_TOUCH_DEBUG", "0") == "1";
    bool debug = handcut::Setting("DEBUG", "0") == "1";
    auto pointerSmoothness = [] {
        return std::clamp(std::atoi(handcut::Setting("HANDS_SMOOTHNESS", "50").c_str()), 0, 100);
    };
    int smoothness = pointerSmoothness();
    int64_t nextSettings = 0;
    int64_t nextOverlayScan = 0;
    int64_t nextGazeScene = 0;
    int64_t nextGazeSurfaces = 0;
    bool floatingGaze = false;
    std::vector<std::string> overlayKeys;
    std::vector<vr::VROverlayHandle_t> observedOverlays;
    bool touchPressed = false;
    int64_t touchReleaseAt = 0;
    int64_t nextTouchReport = 0;
    int owner = -1;
    bool showing = false, pressed = false, initialized[2]{}, wasDashboard = false;
    uint32_t begins[2]{};
    uint32_t pendingHand[2]{};
    int64_t pressAt = 0, pending[2]{}, seenSince[2]{}, claimAt = 0, claimedAt = 0;
    // Hold the last displayed ray through the pinch click, then let a drag
    // follow the hand. This is independent of the target overlay's flags.
    PointerPose sentLaser{{}, {1, 0, 0, 0}};
    AimRay heldAim{};
    int64_t sentAt = 0;
    int heldOwner = -1;
    uint32_t sentHandId = 0;
    bool clickDragging = false;
    int64_t clickReleaseUntil = 0;
    std::optional<GrabPose> grabPose;
    Quaternion sentRotation{1, 0, 0, 0};
    uint32_t grabHandId = 0;
    bool grabUsesFittedWrist = false;
    int64_t nextGrabReport = 0;
    int64_t grabLockAt = 0;
    int64_t menuRequestedAt = 0, menuPressedAt = 0;
    bool menuButtonDown = false;
    auto releaseGrabLock = [&] {
        if (grabLockAt) {
            Send(fd, "btn grab_lock 0");
            grabLockAt = 0;
        }
    };
    auto releasePinch = [&](int64_t now) {
        Send(fd, "btn trigger 0");
        releaseGrabLock();
        pressed = false;
        if (!clickDragging && heldOwner == owner) clickReleaseUntil = now + 45'000'000ll;
        else heldOwner = -1;
        clickDragging = false;
        if (grabPose) {
            grabPose->Release(now);
            if (!grabPose->Released()) grabPose.reset();
        }
    };
    auto stopGaze = [&] {
        if (gazeClick.Pressed()) Send(fd, "btn trigger 0");
        gazeClick.Reset();
        if (showing) Send(fd, "hide");
        showing = false;
        claimAt = claimedAt = 0;
    };
    auto resetInteraction = [&] {
        if (pressed) Send(fd, "btn trigger 0");
        releaseGrabLock();
        stopGaze();
        pressed = touchPressed = clickDragging = false;
        owner = heldOwner = -1;
        pending[0] = pending[1] = seenSince[0] = seenSince[1] = 0;
        initialized[0] = initialized[1] = false;
        sentAt = clickReleaseUntil = touchReleaseAt = 0;
        aim.Reset();
        grabPose.reset();
    };
    auto stopMenu = [&] {
        if (menuButtonDown) Send(fd, "btn system 0");
        menuRequestedAt = menuPressedAt = 0;
        menuButtonDown = false;
        stopGaze();
    };
    auto showPointer = [&](int side, int64_t now) {
        if (showing) return;
        Send(fd, side ? "role right" : "role left");
        showing = Send(fd, "show");
        // Let SteamVR assign the input path before selecting its cursor.
        if (showing) claimAt = now + 180'000'000ll;
    };
    auto claimPointer = [&](int64_t now) {
        if (!showing || !claimAt || now < claimAt) return;
        Send(fd, "btn a 1"); Send(fd, "btn a 0");
        claimAt = 0;
        claimedAt = now;
    };
    // The physical trigger engages SteamVR's mouse lock before its click.
    constexpr int64_t kGrabLockLeadNs = 30'000'000ll;
    constexpr int64_t kClickHoldNs = 55'000'000ll;
    constexpr int64_t kGrabStartHoldNs = 35'000'000ll;
    constexpr int64_t kGestureStaleNs = 300'000'000ll;
    constexpr int64_t kAimLossHoldNs = 100'000'000ll;
    constexpr int64_t kPendingTimeoutNs = 600'000'000ll;
    for (; vr::VRSystem()->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd);
         std::this_thread::sleep_for(std::chrono::milliseconds(12))) {
        const int64_t now = NowNs();
        vr::VREvent_t event;
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event)) {
            if (event.eventType == vr::VREvent_Quit) goto done;
            if (event.eventType == vr::VREvent_SceneApplicationStateChanged ||
                event.eventType == vr::VREvent_SceneApplicationChanged) nextGazeScene = 0;
            if (event.eventType == vr::VREvent_OverlayCreated ||
                (mode == InteractionMode::Gaze && event.eventType == vr::VREvent_OverlayShown) ||
                event.eventType == vr::VREvent_OverlayFocusChanged) {
                const auto target = event.data.overlay.overlayHandle;
                if (target != vr::k_ulOverlayHandleInvalid &&
                    std::find(observedOverlays.begin(), observedOverlays.end(), target) == observedOverlays.end()) {
                    if (observedOverlays.size() == 64) observedOverlays.erase(observedOverlays.begin());
                    observedOverlays.push_back(target);
                }
                nextGazeSurfaces = 0;
            } else if (event.eventType == vr::VREvent_OverlayDestroyed) {
                const auto target = event.data.overlay.overlayHandle;
                observedOverlays.erase(std::remove(observedOverlays.begin(), observedOverlays.end(), target),
                                       observedOverlays.end());
                nextGazeSurfaces = 0;
            } else if (event.eventType == vr::VREvent_OverlayHidden) {
                nextGazeSurfaces = 0;
            }
        }
        if (now >= nextSettings) {
            nextSettings = now + 750'000'000ll;
            floatPointer = handcut::Setting("HANDS_FLOATING_POINTER", "1") != "0";
            const auto selection = ReadInteractionMode();
            const bool newInteraction = framehands::InteractionEnabled();
            if (selection != mode || newInteraction != interactionEnabled) {
                if (!menuRequestedAt) resetInteraction();
                mode = selection;
                interactionEnabled = newInteraction;
                touchOn = mode == InteractionMode::Touch;
                nextOverlayScan = nextGazeScene = nextGazeSurfaces = 0;
            }
            const bool newTwoFistMenu = framehands::TwoFistMenuEnabled();
            if (twoFistMenu != newTwoFistMenu) {
                twoFistMenu = newTwoFistMenu;
                menuGesture = {};
                if (menuRequestedAt) stopMenu();
            }
            touchDebug = handcut::Setting("HANDS_TOUCH_DEBUG", "0") == "1";
            debug = handcut::Setting("DEBUG", "0") == "1";
            smoothness = pointerSmoothness();
        }
        if ((touchOn || mode == InteractionMode::Gaze) && now >= nextOverlayScan) {
            overlayKeys = PointerOverlayKeys();
            nextGazeSurfaces = 0;
            // Query SteamVR live as well as its last saved settings file.
            char activeKey[256] = {};
            vr::EVRSettingsError settingsError = vr::VRSettingsError_None;
            vr::VRSettings()->GetString("dashboard", "lastAccessedExternalOverlayKey",
                                        activeKey, sizeof(activeKey), &settingsError);
            if (settingsError == vr::VRSettingsError_None && activeKey[0] &&
                std::find(overlayKeys.begin(), overlayKeys.end(), activeKey) == overlayKeys.end())
                overlayKeys.emplace_back(activeKey);
            nextOverlayScan = now + 3'000'000'000ll;
        }
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0,
                                                        poses, vr::k_unMaxTrackedDeviceCount);
        const bool dashboard = vr::VROverlay()->IsDashboardVisible();
        if (mode == InteractionMode::Gaze && dashboard != wasDashboard) {
            if (!menuRequestedAt) stopGaze();
            nextGazeScene = nextGazeSurfaces = 0;
        } else if (dashboard && !wasDashboard && showing) claimAt = now + 180'000'000ll;
        wasDashboard = dashboard;
        const bool fresh = poses[0].bPoseIsValid && hands.Update(poses[0].mDeviceToAbsoluteTracking, now);
        const handcut::Hand *side[2]{};
        const handcut::Hand *measured[2]{};
        if (fresh) for (const auto &h : hands.shown()) side[h.right ? 1 : 0] = &h;
        if (fresh) for (const auto &h : hands.seen()) measured[h.right ? 1 : 0] = &h;
        if (twoFistMenu) {
            const auto &left = hands.grip(0), &right = hands.grip(1);
            const auto gripId = [&](int s) {
                const auto &grip = hands.grip(s);
                return measured[s] && grip.tracked && measured[s]->id == grip.handId ? grip.handId : 0u;
            };
            const bool canToggle = !menuRequestedAt && !pressed && !gazeClick.Pressed() &&
                                   !(grabPose && grabPose->HoldingRelease(now));
            if (menuGesture.Step(left.captureNs == right.captureNs ? left.captureNs : 0, now,
                                 gripId(0), gripId(1), left.down, right.down, canToggle)) {
                resetInteraction();
                menuRequestedAt = now;
            }
        }
        if (menuRequestedAt) {
            // A system click needs a connected device. Give SteamVR time to
            // assign its input path and deliver button-up before disconnecting.
            const auto &hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
            if (!hmd.bPoseIsValid || now - menuRequestedAt >= 750'000'000 ||
                (menuPressedAt && now - menuPressedAt >= 140'000'000)) {
                stopMenu();
                continue;
            }
            const auto &m = hmd.mDeviceToAbsoluteTracking.m;
            const PointerPose pose{Position(hmd.mDeviceToAbsoluteTracking), AimRotation({-m[0][2], -m[1][2], -m[2][2]})};
            SendPoses(fd, pose, pose);
            if (!showing) showPointer(CheckGazeControllers(poses, controllers, now).freeSide, now);
            if (showing && !menuPressedAt && now >= claimAt && Send(fd, "btn system 1")) {
                menuPressedAt = now;
                menuButtonDown = true;
            } else if (menuButtonDown && now - menuPressedAt >= 55'000'000 && Send(fd, "btn system 0")) {
                menuButtonDown = false;
            }
            continue;
        }
        if (twoFistMenu && !interactionEnabled) continue;
        if (mode == InteractionMode::Gaze) {
            if (!dashboard && floatPointer && now >= nextGazeScene) {
                floatingGaze = FloatingGazeAllowed();
                nextGazeScene = now + 250'000'000ll;
            }
            const auto &hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
            if (!hmd.bPoseIsValid || (!dashboard && (!floatPointer || !floatingGaze))) {
                stopGaze();
                continue;
            }
            const auto physical = CheckGazeControllers(poses, controllers, now);
            const auto &m = hmd.mDeviceToAbsoluteTracking.m;
            const auto live = HeadGazeRay(Position(hmd.mDeviceToAbsoluteTracking), {-m[0][2], -m[1][2], -m[2][2]});
            if (!live || physical.busy) {
                stopGaze();
                continue;
            }
            GazeHand pinch[2]{};
            for (int s = 0; s < 2; ++s) {
                const auto &g = hands.pinch(s);
                bool eligible = false;
                if (measured[s] && measured[s]->id == g.handId && g.tracked && !g.lost && !hands.grip(s).down) {
                    const V knuckle = (Point(measured[s]->p[5]) + Point(measured[s]->p[9])) * 0.5;
                    const V offset = knuckle - live->origin;
                    eligible = offset.y > -0.50 && Dot(offset, live->direction) > 0.15;
                }
                pinch[s] = {g.begins, g.handId, g.captureNs, eligible, g.down};
            }
            const bool ready = showing && !claimAt && claimedAt && now - claimedAt >= 60'000'000ll;
            const auto cursor = gazeClick.Step(*live, pinch, ready, now);
            if (now >= nextGazeSurfaces) {
                gazeSurfaces.Refresh(overlayKeys, observedOverlays);
                nextGazeSurfaces = now + 150'000'000ll;
            }
            const auto depth = gazeSurfaces.Depth(cursor.ray, hmd.mDeviceToAbsoluteTracking, physical.pointer);
            const auto onScreen = GazeScreenRay(cursor.ray, depth);
            const auto rotation = AimRotation(cursor.ray.direction);
            SendPoses(fd, {onScreen.origin, rotation}, {cursor.ray.origin, rotation});
            showPointer(physical.freeSide, now);
            claimPointer(now);
            if (cursor.button == GazeButton::Down) Send(fd, "btn trigger 1");
            else if (cursor.button == GazeButton::Up) Send(fd, "btn trigger 0");
            continue;
        }
        bool ready[2]{};
        for (int s = 0; s < 2; ++s) {
            const auto &g = hands.pinch(s);
            if (!initialized[s]) begins[s] = g.begins, initialized[s] = true;
            if (!side[s]) seenSince[s] = 0;
            else if (!seenSince[s]) seenSince[s] = now;
            // Stereo depth is best, but a stable one-camera hand is sufficient
            // for a dashboard ray. Direct Touch still uses its own contact test.
            const bool recentlyAimed = s == owner && side[s] && side[s]->id == sentHandId &&
                                      sentAt && now - sentAt < kAimLossHoldNs;
            const bool located = side[s] && (side[s]->stereo || recentlyAimed || now - seenSince[s] >= 250'000'000ll);
            if ((dashboard || floatPointer) && located && measured[s] && !hands.grip(s).down) {
                const auto &headPose = poses[0].mDeviceToAbsoluteTracking;
                const V knuckle = (Point(measured[s]->p[5])+Point(measured[s]->p[9]))*0.5;
                const V fromHead = knuckle-Position(headPose);
                const V forward{-headPose.m[0][2],-headPose.m[1][2],-headPose.m[2][2]};
                ready[s] = Dot(fromHead, forward) > (s == owner ? 0.16 : 0.22) &&
                           fromHead.y > (s == owner ? -0.50 : -0.45) &&
                           !PhysicalControllerBusy(poses, *side[s], controllers, now);
            }
        }
        int pick = owner >= 0 && ready[owner] ? owner : ready[1] ? 1 : ready[0] ? 0 : -1;
        if (grabPose && grabPose->Released() && !grabPose->HoldingRelease(now))
            grabPose.reset();
        if (pressed && grabPose) {
            const auto &g = hands.pinch(owner);
            const bool pinchEnded = !g.down || g.handId != grabHandId || now - g.captureNs > kGestureStaleNs;
            // Keep a live drag attached past the timeout for an idle press.
            const bool idlePressExpired = !grabPose->HasDragged() && now - g.beginNs > 4'000'000'000ll;
            // Release before generating another pose so finger opening or
            // disappearing cannot move the window away from its last drag pose.
            if (pick != owner || side[owner]->id != grabHandId ||
                (now - pressAt > 50'000'000ll && (pinchEnded || idlePressExpired)))
                releasePinch(now);
        }
        const bool holdingRelease = grabPose && grabPose->HoldingRelease(now);
        if (touchOn && touchDebug && pick < 0 && now >= nextTouchReport) {
            nextTouchReport = now + 1'000'000'000ll;
            std::fprintf(stderr,
                         "fh-input touch: gated dashboard=%d fresh=%d hands=%d left=%d/%d right=%d/%d keys=%zu\n",
                         int(dashboard), int(fresh), int(bool(side[0]) + bool(side[1])),
                         int(side[0] && side[0]->stereo), int(ready[0]),
                         int(side[1] && side[1]->stereo), int(ready[1]), overlayKeys.size());
            std::fflush(stderr);
        }
        int64_t newestPending = 0;
        for (int s = 0; s < 2; ++s) {
            const auto &g = hands.pinch(s);
            if (g.begins != begins[s]) {
                begins[s] = g.begins;
                if (ready[s] && !pressed && g.handId == measured[s]->id) {
                    pending[s] = now;
                    pendingHand[s] = g.handId;
                    if (s == owner && sentHandId == g.handId && sentAt && now - sentAt < kAimLossHoldNs) {
                        heldAim = {sentLaser.origin, pointer_pose::Rotate(sentLaser.rotation, {0, 0, -1})};
                        heldOwner = s;
                    } else if (heldOwner == s) {
                        heldOwner = -1;
                    }
                }
            }
            if (!pressed && ready[s] && pending[s] > newestPending &&
                now - pending[s] < kPendingTimeoutNs) {
                newestPending = pending[s];
                pick = s;
            }
        }
        // Keep the device and its role alive while SteamVR saves the window's
        // transform, even if the wearer lowers the hand after letting go.
        const bool holdingMissingAim = !holdingRelease && !newestPending && owner >= 0 && showing &&
            !side[owner] && !pressed && poses[0].bPoseIsValid && (dashboard || floatPointer) &&
            sentAt && now - sentAt < kAimLossHoldNs;
        if (holdingMissingAim) {
            pending[owner] = 0;
            heldOwner = -1;
            releaseGrabLock();
        }
        if (holdingRelease || holdingMissingAim) pick = owner;
        if (pick != owner || pick < 0) {
            if (owner >= 0 && owner != pick) pending[owner] = 0;
            if (pick < 0) pending[0] = pending[1] = 0;
            if (pressed) Send(fd, "btn trigger 0"), pressed = touchPressed = false;
            releaseGrabLock();
            if (showing) Send(fd, "hide"), showing = false;
            claimAt = claimedAt = 0;
            heldOwner = -1;
            clickDragging = false;
            clickReleaseUntil = 0;
            aim.Reset();
            grabPose.reset();
            sentAt = 0;
            owner = pick;
        }
        if (holdingMissingAim) {
            SendPoses(fd, sentLaser, sentLaser);
        } else if (holdingRelease && (owner < 0 || !ready[owner])) {
            SendPoses(fd, sentLaser, grabPose->Pose());
        } else if (owner >= 0) {
            const auto &h = *side[owner];
            const auto &sample = *measured[owner];
            const auto &g = hands.pinch(owner);
            const auto &captureHead = hands.captureHead();
            const V knuckle = (Point(sample.p[5])+Point(sample.p[9])) * 0.5;
            const V headRight{captureHead.m[0][0], captureHead.m[1][0], captureHead.m[2][0]};
            const auto handAim = aim.Step(knuckle, Position(captureHead), headRight, sample.right,
                                         sample.id, sample.stereo, sample.captureNs, now, smoothness);
            if (handAim) {
                // Optional fingertip contact with a known SteamVR overlay.
                // Contact is tested against real 3D overlay geometry, not a guessed
                // fixed screen plane; SteamVR dispatches the virtual controller click.
                const V tip = Point(h.p[8]);
                const V fromHeadToTip = tip - Position(poses[0].mDeviceToAbsoluteTracking);
                const double tipRange = Length(fromHeadToTip);
                // Keep the head ray as a fallback. Its perspective can miss the
                // panel even when the tracked fingertip is beside the surface.
                const V towardTip = tipRange > 0.12 ? fromHeadToTip * (1.0 / tipRange) : handAim->direction;
                bool touching = false;
                int found = 0, visible = 0, intersections = 0, surfaceTransforms = 0, touchSpaceCode = -1;
                double closestGap = 1e9;
                double closestPlaneGap = 1e9, planeX = 0, planeY = 0, planeWidth = 0, planeHeight = 0;
                std::string closestOverlay = "none";
                V contactOrigin = tip - towardTip * 0.075, contactDir = towardTip;
                if (touchOn && tipRange > 0.12 && !g.down && !grabPose) {
                    const V head = Position(poses[0].mDeviceToAbsoluteTracking);
                    const auto &rawHead = poses[0].mDeviceToAbsoluteTracking;
                    vr::ETrackingUniverseOrigin touchSpace = vr::VRCompositor()->GetTrackingSpace();
                    vr::TrackedDevicePose_t spaceHeadPose{};
                    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(touchSpace, 0, &spaceHeadPose, 1);
                    if (!spaceHeadPose.bPoseIsValid) {
                        touchSpace = vr::TrackingUniverseRawAndUncalibrated;
                        spaceHeadPose = poses[0];
                    }
                    touchSpaceCode = int(touchSpace);
                    const auto &spaceHead = spaceHeadPose.mDeviceToAbsoluteTracking;
                    auto pointInSpace = [&](V raw) {
                        return Local(spaceHead, Unrotate(rawHead, raw - head));
                    };
                    auto directionInSpace = [&](V raw) {
                        return Rotate(spaceHead, Unrotate(rawHead, raw));
                    };
                    auto queryFor = [&](V source, V direction) {
                        vr::VROverlayIntersectionParams_t q{};
                        q.eOrigin = touchSpace;
                        const V p = pointInSpace(source), d = directionInSpace(direction);
                        q.vSource = {{float(p.x), float(p.y), float(p.z)}};
                        q.vDirection = {{float(d.x), float(d.y), float(d.z)}};
                        return q;
                    };

                    // This OpenVR SDK has no GetGamepadFocusOverlay() on IVROverlay.
                    // Probe keys discovered from SteamVR's current settings and logs.
                    auto probe = [&](vr::VROverlayHandle_t target, const std::string &key) {
                        if (target == vr::k_ulOverlayHandleInvalid) return;
                        ++found;
                        if (!vr::VROverlay()->IsOverlayVisible(target)) return;
                        vr::VROverlayInputMethod method = vr::VROverlayInputMethod_None;
                        if (vr::VROverlay()->GetOverlayInputMethod(target, &method) != vr::VROverlayError_None ||
                            method != vr::VROverlayInputMethod_Mouse) return;
                        ++visible;
                        auto intersect = [&](V source, V direction, double sourceToTip) {
                            auto query = queryFor(source, direction);
                            vr::VROverlayIntersectionResults_t hit{};
                            if (!vr::VROverlay()->ComputeOverlayIntersection(target, &query, &hit)) return;
                            ++intersections;
                            const double gap = double(hit.fDistance) - sourceToTip;
                            if (std::abs(gap) < std::abs(closestGap)) {
                                closestGap = gap;
                                closestOverlay = key;
                                contactOrigin = tip - direction * 0.075;
                                contactDir = direction;
                                // Stereo depth varies by centimetres. Keep the
                                // press zone narrower than the release zone.
                                touching = gap >= -0.040 && gap <= (touchPressed ? 0.090 : 0.050);
                            }
                        };
                        intersect(head, towardTip, tipRange);

                        // Query along the overlay's own normal through the tip.
                        // This measures actual fingertip-to-surface distance and
                        // keeps lateral fingertip movement at the same pixel.
                        vr::HmdVector2_t center{};
                        float width = 0;
                        vr::HmdVector2_t mouseScale{};
                        if (vr::VROverlay()->GetOverlayWidthInMeters(target, &width) == vr::VROverlayError_None &&
                            vr::VROverlay()->GetOverlayMouseScale(target, &mouseScale) == vr::VROverlayError_None &&
                            mouseScale.v[0] > 0) {
                            center.v[0] = width * 0.5f;
                            center.v[1] = width * mouseScale.v[1] / mouseScale.v[0] * 0.5f;
                        }
                        vr::HmdMatrix34_t surface{};
                        if (vr::VROverlay()->GetTransformForOverlayCoordinates(
                                target, touchSpace, center, &surface) ==
                            vr::VROverlayError_None) {
                            ++surfaceTransforms;
                            const V outwardInSpace{-surface.m[0][2], -surface.m[1][2], -surface.m[2][2]};
                            const V outward = Rotate(rawHead, Unrotate(spaceHead, outwardInSpace));
                            const double n = Length(outward);
                            if (n > 0.5) {
                                const V normal = outward * (1.0 / n);
                                const V delta = pointInSpace(tip) - Position(surface);
                                const V axisX{surface.m[0][0], surface.m[1][0], surface.m[2][0]};
                                const V axisY{surface.m[0][1], surface.m[1][1], surface.m[2][1]};
                                const double planeGap = Dot(delta, outwardInSpace * (1.0 / n));
                                if (std::abs(planeGap) < std::abs(closestPlaneGap)) {
                                    closestPlaneGap = planeGap;
                                    planeX = Dot(delta, axisX);
                                    planeY = Dot(delta, axisY);
                                    planeWidth = width;
                                    planeHeight = center.v[1] * 2.0;
                                }
                                intersect(tip + normal * 0.30, normal * -1.0, 0.30);
                                // If the ray misses our flat dashboard panel,
                                // its transform and dimensions still provide a
                                // bounded contact test at the fingertip.
                                if (key == "framehands.dashboard" &&
                                    width > 0 && center.v[1] > 0 &&
                                    std::abs(Dot(delta, axisX)) <= width * 0.5 &&
                                    std::abs(Dot(delta, axisY)) <= center.v[1] &&
                                    std::abs(planeGap) < std::abs(closestGap)) {
                                    closestGap = planeGap;
                                    closestOverlay = key + " (plane)";
                                    contactOrigin = tip + normal * 0.075;
                                    contactDir = normal * -1.0;
                                    touching = planeGap >= -0.040 &&
                                               planeGap <= (touchPressed ? 0.090 : 0.050);
                                }
                            }
                        }
                    };
                    std::vector<vr::VROverlayHandle_t> probed;
                    for (const std::string &key : overlayKeys) {
                        vr::VROverlayHandle_t target = vr::k_ulOverlayHandleInvalid;
                        if (vr::VROverlay()->FindOverlay(key.c_str(), &target) != vr::VROverlayError_None) continue;
                        probed.push_back(target);
                        probe(target, key);
                    }
                    for (const auto target : observedOverlays) {
                        if (std::find(probed.begin(), probed.end(), target) != probed.end()) continue;
                        char key[vr::k_unVROverlayMaxKeyLength]{};
                        vr::VROverlay()->GetOverlayKey(target, key, sizeof key);
                        probe(target, key[0] ? key : "observed overlay");
                    }
                }
                // Mono depth can be several centimetres wrong; keep the ray,
                // but require stereo for physical contact with an overlay.
                if (!h.stereo) touching = false;
                if (touchOn && touchDebug && now >= nextTouchReport) {
                    nextTouchReport = now + 1'000'000'000ll;
                    std::fprintf(stderr,
                                 "fh-input touch: owner=%s showing=%d tip=%.2fm pinch=%d space=%d keys=%zu observed=%zu found=%d visible=%d surfaces=%d intersections=%d closest=%s gap=%+.3fm plane=%+.3fm xy=%+.2f,%+.2fm size=%.2fx%.2fm contact=%d\n",
                                 owner ? "right" : "left", int(showing), tipRange, int(g.down),
                                 touchSpaceCode, overlayKeys.size(), observedOverlays.size(), found, visible, surfaceTransforms, intersections, closestOverlay.c_str(),
                                 intersections ? closestGap : 99.0, surfaceTransforms ? closestPlaneGap : 99.0,
                                 planeX, planeY, planeWidth, planeHeight, int(touching));
                    std::fflush(stderr);
                }
                // Use the same ray for the virtual controller that detected
                // contact, so its laser and click land on the tested overlay.
                const bool touchAim = touchOn && (touching || touchPressed) && tipRange > 0.12;
                if (touchAim) heldOwner = -1; // contact placement always wins
                const AimRay liveAim = touchAim ? AimRay{contactOrigin, contactDir} : *handAim;
                if (pending[owner] && (now - pending[owner] >= kPendingTimeoutNs ||
                    pendingHand[owner] != sample.id)) {
                    pending[owner] = 0;
                    if (!pressed) {
                        heldOwner = -1;
                        releaseGrabLock();
                    }
                }
                if (pending[owner] && heldOwner != owner && !touchAim) {
                    heldAim = liveAim;
                    heldOwner = owner;
                }
                if (heldOwner == owner && pressed && !touchPressed && !clickDragging &&
                    now - pressAt >= kClickHoldNs && AimMoved(heldAim, liveAim))
                    clickDragging = true;
                if (!pressed && !pending[owner] && now >= clickReleaseUntil) heldOwner = -1;
                const bool holdClick = !touchAim && heldOwner == owner &&
                    (pending[owner] || (pressed && !touchPressed && !clickDragging) ||
                     now < clickReleaseUntil);
                const auto &clickAim = holdClick ? heldAim : liveAim;
                const PointerPose laser{clickAim.origin, AimRotation(clickAim.direction)};
                PointerPose device = grabPose ? grabPose->Pose() : laser;
                const auto wrist = [&](bool fitted) {
                    if (fitted) return sample.fit ?
                        WristRotation(Point(sample.wristForward), Point(sample.wristRight)) : std::nullopt;
                    const V across = Point(sample.p[17]) - Point(sample.p[5]);
                    return WristRotation(Point(sample.p[9]) - Point(sample.p[0]), sample.right ? across : across * -1);
                };
                if (grabPose && !holdingRelease && now - pressAt >= kGrabStartHoldNs) {
                    // Keep the last wrist contribution if a sample is missing.
                    const auto trackedWrist = g.down && sample.id == grabHandId ?
                        wrist(grabUsesFittedWrist) : std::nullopt;
                    device = grabPose->Step({knuckle, Position(captureHead), trackedWrist,
                                             sample.captureNs, sample.stereo}, now);
                    if (debug && now >= nextGrabReport) {
                        ReportGrabPose(h.right, dashboard, grabPose->WristAngle(), sentRotation, poses);
                        nextGrabReport = now + 500'000'000ll;
                    }
                }
                SendPoses(fd, laser, device);
                sentRotation = device.rotation;
                sentLaser = laser; sentAt = now; sentHandId = touchAim ? 0 : sample.id;
                showPointer(owner, now);
                if (!holdingRelease) claimPointer(now);
                if (!holdingRelease && showing && !pressed && !grabLockAt && touchOn && touching && now >= touchReleaseAt) {
                    Send(fd, "btn trigger 1"); pressed = touchPressed = true; pressAt = now;
                }
                if (touchPressed) {
                    if (!touching && now - pressAt > 70'000'000ll) {
                        Send(fd, "btn trigger 0");
                        pressed = touchPressed = false;
                        touchReleaseAt = now + 130'000'000ll;
                    }
                }
                if (!holdingRelease && showing && !pressed && !claimAt && claimedAt &&
                    now - claimedAt >= 60'000'000ll && pending[owner] &&
                    now - pending[owner] < kPendingTimeoutNs) {
                    // Floated windows can be grabbed even when SteamVR says
                    // the dashboard is visible. That flag only gates mouse lock.
                    const bool grabbing = g.down && g.handId == h.id;
                    const bool lockGrab = grabbing && !dashboard;
                    if (!lockGrab) releaseGrabLock();
                    if (lockGrab && !grabLockAt) {
                        if (Send(fd, "btn grab_lock 1")) grabLockAt = now;
                    } else if (!lockGrab || now - grabLockAt >= kGrabLockLeadNs) {
                        if (Send(fd, "btn trigger 1")) {
                            pressed = true;
                            pressAt = now;
                            pending[owner] = 0;
                            clickReleaseUntil = 0;
                            // Measure wrist motion relative to trigger down.
                            if (grabbing) {
                                // The palm fit and the landmark estimate can
                                // use different axes. Keep one source per grab.
                                const auto fitted = wrist(true);
                                grabUsesFittedWrist = fitted.has_value();
                                grabPose.emplace(device, GrabSample{knuckle, Position(captureHead),
                                                 fitted ? fitted : wrist(false), sample.captureNs, sample.stereo}, now);
                                grabHandId = sample.id;
                                nextGrabReport = 0;
                            }
                        }
                    }
                }
                if (pressed && !touchPressed && !grabPose &&
                    (!g.down || now-g.beginNs > 4'000'000'000ll) && now-pressAt > 50'000'000ll)
                    releasePinch(now);
            }
        }
    }
done:
    Send(fd, "btn trigger 0");
    if (menuButtonDown) Send(fd, "btn system 0");
    releaseGrabLock();
    Send(fd, "hide");
    close(fd);
    vr::VR_Shutdown();
    return 0;
}
