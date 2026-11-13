// Dashboard and undocked-panel pointer. The SteamVR driver remains disconnected
// until a bare hand is pointing at the dashboard; no game input is synthesized.
#include "../../config/ftsock.h"
#include "../../renderer/handcut.h"

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

namespace {
using Clock = std::chrono::steady_clock;
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
struct V { double x, y, z; };
V Position(const vr::HmdMatrix34_t &m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }
V operator-(V a, V b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V operator+(V a, V b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V operator*(V a, double k) { return {a.x*k, a.y*k, a.z*k}; }
double Length(V a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
// Adaptively smooth the dashboard pointer. Small movements are steadied;
// fast deliberate motion raises the cut-off to keep the ray responsive.
struct AimSmoother {
    V origin{}, direction{};
    double originSpeed = 0, directionSpeed = 0;
    int64_t last = 0;
    static double Alpha(double hz, double dt) { return 1.0 - std::exp(-2.0 * M_PI * hz * dt); }
    static V Smooth(V raw, V filtered, double &speed, double dt, double gain, double base) {
        const V delta = raw - filtered;
        speed += (Length(delta) / dt - speed) * Alpha(1.5, dt);
        return filtered + delta * Alpha(std::clamp(base + gain * speed, base, 40.0), dt);
    }
    void Reset() { last = 0; originSpeed = directionSpeed = 0; }
    void Step(V p, V d, int64_t now, int smoothness) {
        if (!last || now <= last || now - last > 250'000'000ll) {
            origin = p; direction = d; originSpeed = directionSpeed = 0;
        } else {
            const double dt = std::clamp((now - last) / 1e9, 0.002, 0.10);
            // At 50 the original 2.5 Hz steady-hand cut-off is preserved.
            // Movement still raises the cutoff so deliberate aiming responds.
            const double base = 12.0 * std::exp(-3.14 * std::clamp(smoothness, 0, 100) / 100.0);
            origin = Smooth(p, origin, originSpeed, dt, 25.0, base);
            direction = Smooth(d, direction, directionSpeed, dt, 12.0, base);
            const double n = Length(direction);
            direction = n > 1e-6 ? direction * (1.0 / n) : d;
        }
        last = now;
    }
};
V Point(const float p[3]) { return {p[0], p[1], p[2]}; }
V Local(const vr::HmdMatrix34_t &m, V p) {
    return {m.m[0][3]+m.m[0][0]*p.x+m.m[0][1]*p.y+m.m[0][2]*p.z,
            m.m[1][3]+m.m[1][0]*p.x+m.m[1][1]*p.y+m.m[1][2]*p.z,
            m.m[2][3]+m.m[2][0]*p.x+m.m[2][1]*p.y+m.m[2][2]*p.z};
}
double Dot(V a, V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
bool Send(int fd, const char *message) {
    sockaddr_un addr;
    return sendto(fd, message, std::strlen(message), MSG_NOSIGNAL,
                  reinterpret_cast<sockaddr *>(&addr), ft_sock_addr(FT_SOCK_POINTER, &addr)) >= 0;
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
        if (Length(pos-palm) < 0.15) state[i].busyUntil = now + 3'000'000'000ll;
        if (state[i].at && now-state[i].at < 250'000'000ll && Length(pos-state[i].last) > 0.025)
            state[i].busyUntil = now + 3'000'000'000ll;
        state[i].last = pos; state[i].at = now;
        if (now < state[i].busyUntil) return true;
    }
    return false;
}
}

int main() {
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) return std::fprintf(stderr, "fh-input: %s\n",
        vr::VR_GetVRInitErrorAsEnglishDescription(error)), 1;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::perror("fh-input socket"), 1;
    handcut::Hands hands;
    Controller controllers[vr::k_unMaxTrackedDeviceCount]{};
    AimSmoother aim;
    bool smoothPointer = handcut::Setting("HANDS_POINTER_SMOOTH", "1") != "0";
    bool floatPointer = handcut::Setting("HANDS_FLOATING_POINTER", "1") != "0";
    bool touchOn = handcut::Setting("HANDS_TOUCH", "0") == "1";
    int smoothness = std::clamp(std::atoi(handcut::Setting("HANDS_SMOOTHNESS", "50").c_str()), 0, 100);
    int64_t nextSettings = 0;
    bool touchPressed = false;
    int64_t touchReleaseAt = 0;
    int owner = -1;
    bool showing = false, pressed = false, initialized[2]{};
    uint32_t begins[2]{};
    int64_t pressAt = 0, pending[2]{};
    while (vr::VRSystem()->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) {
        const int64_t now = NowNs();
        if (now >= nextSettings) {
            nextSettings = now + 750'000'000ll;
            smoothPointer = handcut::Setting("HANDS_POINTER_SMOOTH", "1") != "0";
            floatPointer = handcut::Setting("HANDS_FLOATING_POINTER", "1") != "0";
            touchOn = handcut::Setting("HANDS_TOUCH", "0") == "1";
            smoothness = std::clamp(std::atoi(handcut::Setting("HANDS_SMOOTHNESS", "50").c_str()), 0, 100);
        }
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0,
                                                        poses, vr::k_unMaxTrackedDeviceCount);
        const bool dashboard = vr::VROverlay()->IsDashboardVisible();
        const bool fresh = poses[0].bPoseIsValid && hands.Update(poses[0].mDeviceToAbsoluteTracking, now);
        const handcut::Hand *side[2]{};
        if (fresh) for (const auto &h : hands.shown()) side[h.right ? 1 : 0] = &h;
        bool ready[2]{};
        for (int s = 0; s < 2; ++s) {
            const auto &g = hands.pinch(s);
            if (!initialized[s]) begins[s] = g.begins, initialized[s] = true;
            if ((dashboard || floatPointer) && side[s] && side[s]->stereo && !hands.grip(s).down) {
                const auto &headPose = poses[0].mDeviceToAbsoluteTracking;
                const V knuckle = (Point(side[s]->p[5])+Point(side[s]->p[9]))*0.5;
                const V fromHead = knuckle-Position(headPose);
                const V forward{-headPose.m[0][2],-headPose.m[1][2],-headPose.m[2][2]};
                ready[s] = Dot(fromHead, forward) > 0.22 && fromHead.y > -0.45 &&
                           !PhysicalControllerBusy(poses, *side[s], controllers, now);
            }
        }
        int pick = owner >= 0 && ready[owner] ? owner : ready[1] ? 1 : ready[0] ? 0 : -1;
        for (int s = 0; s < 2; ++s) {
            const auto &g = hands.pinch(s);
            if (g.begins != begins[s]) {
                begins[s] = g.begins;
                if (ready[s] && !pressed) pick = s, pending[s] = now;
            }
        }
        if (pick != owner || pick < 0) {
            aim.Reset();  // never interpolate a ray between different hands
            if (pressed) Send(fd, "btn trigger 0"), pressed = touchPressed = false;
            if (showing) Send(fd, "hide"), showing = false;
            owner = pick;
        }
        if (owner >= 0) {
            const auto &h = *side[owner];
            const auto &g = hands.pinch(owner);
            const V shoulder = Local(poses[0].mDeviceToAbsoluteTracking,
                                     {owner ? 0.17 : -0.17, -0.20, 0.08});
            const V knuckle = (Point(h.p[5])+Point(h.p[9])) * 0.5;
            V dir = knuckle-shoulder;
            const double distance = Length(dir);
            if (distance > 0.15) {
                dir = dir * (1.0/distance);
                const V origin = knuckle + dir*0.03;
                // Optional fingertip contact with a known SteamVR overlay.
                // Contact is tested against real 3D overlay geometry, not a guessed
                // fixed screen plane; SteamVR dispatches the virtual controller click.
                V tip = Point(h.p[8]);
                V distal = Point(h.p[7]);
                V finger = tip - distal;
                const double fingerLen = Length(finger);
                if (fingerLen > 0.009) finger = finger * (1.0 / fingerLen);
                bool touching = false;
                if (touchOn && fingerLen > 0.009 && !g.down) {
                    // OpenVR cannot enumerate arbitrary third-party overlays.
                    // Probe known dashboard/desktop keys; unknown apps need an adapter.
                    const char *targets[] = {"framehands.dashboard", "system.desktop.1",
                                             "system.desktop.2", "valve.steam.desktop", "system.settings"};
                    vr::VROverlayIntersectionParams_t query{};
                    query.eOrigin = vr::TrackingUniverseRawAndUncalibrated;
                    query.vSource = {{float(distal.x), float(distal.y), float(distal.z)}};
                    query.vDirection = {{float(finger.x), float(finger.y), float(finger.z)}};
                    for (const char *key : targets) {
                        vr::VROverlayHandle_t target = vr::k_ulOverlayHandleInvalid;
                        if (vr::VROverlay()->FindOverlay(key, &target) != vr::VROverlayError_None ||
                            !vr::VROverlay()->IsOverlayVisible(target)) continue;
                        vr::VROverlayIntersectionResults_t hit{};
                        if (vr::VROverlay()->ComputeOverlayIntersection(target, &query, &hit)) {
                            const double gap = double(hit.fDistance) - fingerLen;
                            if (gap >= -0.012 && gap <= (touchPressed ? 0.055 : 0.022)) {
                                touching = true;
                                break;
                            }
                        }
                    }
                }
                // Switch to the fingertip pose for actual contact. The palm ray
                // remains the default for distant pinching.
                const bool touchAim = touchOn && (touching || touchPressed) && fingerLen > 0.009;
                const V aimOrigin = touchAim ? tip - finger * 0.055 : origin;
                const V aimDir = touchAim ? finger : dir;
                if (touchAim) aim.Reset(); // contact must follow the fingertip exactly
                else if (smoothPointer) aim.Step(aimOrigin, aimDir, now, smoothness);
                const V p = smoothPointer && !touchAim ? aim.origin : aimOrigin;
                const V d = smoothPointer && !touchAim ? aim.direction : aimDir;
                char pose[160];
                std::snprintf(pose, sizeof pose, "pose %.5f %.5f %.5f %.3f %.3f", p.x, p.y, p.z,
                              std::atan2(-d.x,-d.z)*180/M_PI, std::asin(std::clamp(d.y,-1.0,1.0))*180/M_PI);
                Send(fd, pose);
                if (!showing) {
                    Send(fd, owner ? "role right" : "role left");
                    showing = Send(fd, "show");
                    if (showing) Send(fd, "btn a 1"), Send(fd, "btn a 0"); // claim the dashboard laser
                }
                if (showing && !pressed && touchOn && touching && now >= touchReleaseAt) {
                    Send(fd, "btn trigger 1"); pressed = touchPressed = true; pressAt = now;
                }
                if (touchPressed) {
                    if (!touching && now - pressAt > 70'000'000ll) {
                        Send(fd, "btn trigger 0");
                        pressed = touchPressed = false;
                        touchReleaseAt = now + 130'000'000ll;
                        aim.Reset();
                    }
                }
                if (showing && !pressed && pending[owner] && now-pending[owner] < 200'000'000ll) {
                    Send(fd, "btn trigger 1"); pressed = true; pressAt = now; pending[owner] = 0;
                }
                if (pressed && !touchPressed && (!g.down || now-g.beginNs > 4'000'000'000ll) && now-pressAt > 50'000'000ll)
                    Send(fd, "btn trigger 0"), pressed = false;
            }
        }
        vr::VREvent_t event;
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event))
            if (event.eventType == vr::VREvent_Quit) goto done;
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }
done:
    Send(fd, "btn trigger 0"); Send(fd, "hide");
    close(fd);
    vr::VR_Shutdown();
    return 0;
}
