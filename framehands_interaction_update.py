#!/usr/bin/env python3
"""Apply FrameHands floating-pointer, touch, smoothing, and stall-recovery update.

Run from repository root: python framehands_interaction_update.py
Idempotent: refuses to replace unexpected contents rather than corrupting files.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
EDITED = []

def edit(path, old, new):
    p = ROOT / path
    if not p.is_file():
        raise RuntimeError(f'{path} missing: run this script from the FrameHands repository root')
    s = p.read_text(encoding='utf-8').replace('\r\n','\n')
    if new in s:
        return
    if s.count(old) != 1:
        raise RuntimeError(f'{path}: expected one anchor, got {s.count(old)}: {old[:100]!r}')
    p.write_bytes(s.replace(old,new).encode('utf-8'))  # preserve LF for Linux shell scripts
    EDITED.append(path)

def add(path, body):
    p = ROOT/path
    if p.is_file():
        if p.read_text(encoding='utf-8') == body: return
        raise RuntimeError(f'{path} already exists with different contents')
    p.parent.mkdir(parents=True,exist_ok=True)
    p.write_text(body,encoding='utf-8')
    EDITED.append(path)

try:
    edit('config/default.conf', 'HANDS_POINTER_SMOOTH=1\n',
         'HANDS_POINTER_SMOOTH=1\nHANDS_FLOATING_POINTER=1\nHANDS_TOUCH=0\n'
         '# 0 is the least smoothing, 100 is the most; affects both hand display and laser.\n'
         'HANDS_SMOOTHNESS=50\nAUTO_RECOVER=1\n')

    # The dashboard uses the same persistently stored configuration as the CLI.
    edit('framehands', 'DASHBOARD_UNIT = "framehands-dashboard.service"\n',
         'DASHBOARD_UNIT = "framehands-dashboard.service"\n'
         'WATCHDOG_UNIT = "framehands-watchdog.service"\n')
    edit('framehands', '    for unit in (*UNITS, DASHBOARD_UNIT):\n        cmd = "enable" if (unit == DASHBOARD_UNIT or unit in wanted) else "disable"\n',
         '    for unit in (*UNITS, DASHBOARD_UNIT, WATCHDOG_UNIT):\n'
         '        use_watchdog = unit == WATCHDOG_UNIT and config().get("AUTO_RECOVER", "1") != "0"\n'
         '        cmd = "enable" if (unit == DASHBOARD_UNIT or (use_watchdog and bool(wanted)) or unit in wanted) else "disable"\n')
    edit('framehands', '"run-dashboard", "register-dashboard"))',
         '"run-dashboard", "register-dashboard", "touch-on", "touch-off", "smoothness", "run-watchdog"))')
    edit('framehands', '    args = parser.parse_args(argv)\n',
         '    parser.add_argument("value", nargs="?", help="smoothness 0..100")\n'
         '    args = parser.parse_args(argv)\n')
    edit('framehands', '        elif cmd == "register-dashboard":\n',
         '        elif cmd == "run-watchdog":\n'
         '            return watchdog()\n'
         '        elif cmd in ("touch-on", "touch-off"):\n'
         '            set_config("HANDS_TOUCH", "1" if cmd == "touch-on" else "0")\n'
         '            print("Direct touch:", "on" if cmd == "touch-on" else "off")\n'
         '        elif cmd == "smoothness":\n'
         '            if args.value is None or not args.value.isdecimal() or int(args.value) > 100:\n'
         '                raise RuntimeError("usage: framehands smoothness 0..100")\n'
         '            set_config("HANDS_SMOOTHNESS", str(int(args.value)))\n'
         '            print("Smoothness:", args.value)\n'
         '        elif cmd == "register-dashboard":\n')
    edit('framehands', '            if active("steamvr.service"):\n                ctl("start", *selected(), check=True)\n                ctl("start", DASHBOARD_UNIT, check=True)\n',
         '            if active("steamvr.service"):\n                ctl("start", *selected(), check=True)\n                ctl("start", DASHBOARD_UNIT, check=True)\n'
         '                if config().get("AUTO_RECOVER", "1") != "0": ctl("start", WATCHDOG_UNIT, check=True)\n')
    edit('framehands', '                ctl("start", DASHBOARD_UNIT, check=True)\n                if config().get("AUTO_START", "1") != "0" and selected():\n',
         '                ctl("start", DASHBOARD_UNIT, check=True)\n'
         '                if config().get("AUTO_RECOVER", "1") != "0" and selected() and config().get("AUTO_START", "1") != "0":\n'
         '                    ctl("start", WATCHDOG_UNIT, check=True)\n'
         '                else:\n                    ctl("stop", WATCHDOG_UNIT)\n'
         '                if config().get("AUTO_START", "1") != "0" and selected():\n')
    edit('framehands', 'for unit in (*UNITS, DASHBOARD_UNIT):\n                print(f"{unit}',
         'for unit in (*UNITS, DASHBOARD_UNIT, WATCHDOG_UNIT):\n                print(f"{unit}')
    edit('framehands', 'for u in (*UNITS, DASHBOARD_UNIT)), ()),',
         'for u in (*UNITS, DASHBOARD_UNIT, WATCHDOG_UNIT)), ()),')

    # Touch/floating settings can be switched live without restarting the camera broker.
    edit('pointer/helper/fh-input.cpp',
         '// Dashboard-only hand ray and pinch input. The SteamVR driver remains disconnected',
         '// Dashboard and undocked-panel pointer. The SteamVR driver remains disconnected')
    edit('pointer/helper/fh-input.cpp',
         '    const bool smoothPointer = handcut::Setting("HANDS_POINTER_SMOOTH", "1") != "0";\n',
         '    bool smoothPointer = handcut::Setting("HANDS_POINTER_SMOOTH", "1") != "0";\n'
         '    bool floatPointer = handcut::Setting("HANDS_FLOATING_POINTER", "1") != "0";\n'
         '    bool touchOn = handcut::Setting("HANDS_TOUCH", "0") == "1";\n'
         '    int smoothness = std::clamp(std::atoi(handcut::Setting("HANDS_SMOOTHNESS", "50").c_str()), 0, 100);\n'
         '    int64_t nextSettings = 0;\n'
         '    bool touchPressed = false;\n'
         '    int64_t touchReleaseAt = 0;\n')
    edit('pointer/helper/fh-input.cpp',
         '        const int64_t now = NowNs();\n',
         '        const int64_t now = NowNs();\n'
         '        if (now >= nextSettings) {\n'
         '            nextSettings = now + 750\'000\'000ll;\n'
         '            smoothPointer = handcut::Setting("HANDS_POINTER_SMOOTH", "1") != "0";\n'
         '            floatPointer = handcut::Setting("HANDS_FLOATING_POINTER", "1") != "0";\n'
         '            touchOn = handcut::Setting("HANDS_TOUCH", "0") == "1";\n'
         '            smoothness = std::clamp(std::atoi(handcut::Setting("HANDS_SMOOTHNESS", "50").c_str()), 0, 100);\n'
         '        }\n')
    edit('pointer/helper/fh-input.cpp',
         'if (dashboard && side[s] && side[s]->stereo && !hands.grip(s).down) {',
         'if ((dashboard || floatPointer) && side[s] && side[s]->stereo && !hands.grip(s).down) {')
    edit('pointer/helper/fh-input.cpp',
         '            if (pressed) Send(fd, "btn trigger 0"), pressed = false;\n',
         '            if (pressed) Send(fd, "btn trigger 0"), pressed = touchPressed = false;\n')
    edit('pointer/helper/fh-input.cpp',
         '                if (smoothPointer) aim.Step(origin, dir, now);\n',
         '                // Optional fingertip contact with a known SteamVR overlay.\n'
         '                // Contact is tested against real 3D overlay geometry, not a guessed\n'
         '                // fixed screen plane; SteamVR dispatches the virtual controller click.\n'
         '                V tip = Point(h.p[8]);\n'
         '                V distal = Point(h.p[7]);\n'
         '                V finger = tip - distal;\n'
         '                const double fingerLen = Length(finger);\n'
         '                if (fingerLen > 0.009) finger = finger * (1.0 / fingerLen);\n'
         '                bool touching = false;\n'
         '                if (touchOn && fingerLen > 0.009 && !g.down) {\n'
         '                    // OpenVR cannot enumerate arbitrary third-party overlays.\n'
         '                    // Probe known dashboard/desktop keys; unknown apps need an adapter.\n'
         '                    const char *targets[] = {"framehands.dashboard", "system.desktop.1",\n'
         '                                             "system.desktop.2", "valve.steam.desktop", "system.settings"};\n'
         '                    vr::VROverlayIntersectionParams_t query{};\n'
         '                    query.eOrigin = vr::TrackingUniverseRawAndUncalibrated;\n'
         '                    query.vSource = {{float(distal.x), float(distal.y), float(distal.z)}};\n'
         '                    query.vDirection = {{float(finger.x), float(finger.y), float(finger.z)}};\n'
         '                    for (const char *key : targets) {\n'
         '                        vr::VROverlayHandle_t target = vr::k_ulOverlayHandleInvalid;\n'
         '                        if (vr::VROverlay()->FindOverlay(key, &target) != vr::VROverlayError_None ||\n'
         '                            !vr::VROverlay()->IsOverlayVisible(target)) continue;\n'
         '                        vr::VROverlayIntersectionResults_t hit{};\n'
         '                        if (vr::VROverlay()->ComputeOverlayIntersection(target, &query, &hit)) {\n'
         '                            const double gap = double(hit.fDistance) - fingerLen;\n'
         '                            if (gap >= -0.012 && gap <= (touchPressed ? 0.055 : 0.022)) {\n'
         '                                touching = true;\n'
         '                                break;\n'
         '                            }\n'
         '                        }\n'
         '                    }\n'
         '                }\n'
         '                // Switch to the fingertip pose for actual contact. The palm ray\n'
         '                // remains the default for distant pinching.\n'
         '                const bool touchAim = touchOn && (touching || touchPressed) && fingerLen > 0.009;\n'
         '                const V aimOrigin = touchAim ? tip - finger * 0.055 : origin;\n'
         '                const V aimDir = touchAim ? finger : dir;\n'
         '                if (touchAim) aim.Reset(); // contact must follow the fingertip exactly\n'
         '                else if (smoothPointer) aim.Step(aimOrigin, aimDir, now, smoothness);\n')
    edit('pointer/helper/fh-input.cpp',
         '                const V p = smoothPointer ? aim.origin : origin;\n                const V d = smoothPointer ? aim.direction : dir;\n',
         '                const V p = smoothPointer && !touchAim ? aim.origin : aimOrigin;\n'
         '                const V d = smoothPointer && !touchAim ? aim.direction : aimDir;\n')
    edit('pointer/helper/fh-input.cpp',
         '                if (showing && !pressed && pending[owner] && now-pending[owner] < 200\'000\'000ll) {\n',
         '                if (showing && !pressed && touchOn && touching && now >= touchReleaseAt) {\n'
         '                    Send(fd, "btn trigger 1"); pressed = touchPressed = true; pressAt = now;\n'
         '                }\n'
         '                if (touchPressed) {\n'
         '                    if (!touching && now - pressAt > 70\'000\'000ll) {\n'
         '                        Send(fd, "btn trigger 0");\n'
         '                        pressed = touchPressed = false;\n'
         '                        touchReleaseAt = now + 130\'000\'000ll;\n'
         '                        aim.Reset();\n'
         '                    }\n'
         '                }\n'
         '                if (showing && !pressed && pending[owner] && now-pending[owner] < 200\'000\'000ll) {\n')
    edit('pointer/helper/fh-input.cpp',
         '                if (pressed && (!g.down || now-g.beginNs > 4\'000\'000\'000ll) && now-pressAt > 50\'000\'000ll)\n',
         '                if (pressed && !touchPressed && (!g.down || now-g.beginNs > 4\'000\'000\'000ll) && now-pressAt > 50\'000\'000ll)\n')
    edit('pointer/helper/fh-input.cpp',
         '    void Step(V p, V d, int64_t now) {',
         '    void Step(V p, V d, int64_t now, int smoothness) {')
    edit('pointer/helper/fh-input.cpp',
         '            origin = Smooth(p, origin, originSpeed, dt, 25.0);\n'
         '            direction = Smooth(d, direction, directionSpeed, dt, 12.0);\n',
         '            // At 50 the original 2.5 Hz steady-hand cut-off is preserved.\n'
         '            // Movement still raises the cutoff so deliberate aiming responds.\n'
         '            const double base = 12.0 * std::exp(-3.14 * std::clamp(smoothness, 0, 100) / 100.0);\n'
         '            origin = Smooth(p, origin, originSpeed, dt, 25.0, base);\n'
         '            direction = Smooth(d, direction, directionSpeed, dt, 12.0, base);\n')
    edit('pointer/helper/fh-input.cpp',
         '    static V Smooth(V raw, V filtered, double &speed, double dt, double gain) {',
         '    static V Smooth(V raw, V filtered, double &speed, double dt, double gain, double base) {')
    edit('pointer/helper/fh-input.cpp',
         'Alpha(std::clamp(2.5 + gain * speed, 2.5, 40.0), dt)',
         'Alpha(std::clamp(base + gain * speed, base, 40.0), dt)')

    # Visual smoothing is live, not a configuration change that requires restarting tracking.
    edit('renderer/handhold.h',
         '    double blendMinMs = 15, blendMaxMs = 70;\n',
         '    double blendMinMs = 15, blendMaxMs = 70;\n'
         '    double blendFactor = 1.0; // extra damping without changing raw tracking\n')
    edit('renderer/handhold.h',
         'blendMs_ = snap ? 0 : std::clamp((nowNs - acceptNs_) / 1e6, k.blendMinMs, k.blendMaxMs);',
         'blendMs_ = snap ? 0 : std::clamp((nowNs - acceptNs_) / 1e6 * k.blendFactor, k.blendMinMs, k.blendMaxMs);')
    edit('renderer/handview.cpp',
         '    holdK.holdS = std::clamp(std::atof(handcut::Setting("HANDS_VIEW_HOLD", "3").c_str()), 0.0, 30.0);\n',
         '    holdK.holdS = std::clamp(std::atof(handcut::Setting("HANDS_VIEW_HOLD", "3").c_str()), 0.0, 30.0);\n'
         '    int64_t nextSmoothSettings = 0;\n')
    edit('renderer/handview.cpp',
         '        const auto tick = std::chrono::steady_clock::now();\n        const int64_t now = MonoNs();\n',
         '        const auto tick = std::chrono::steady_clock::now();\n        const int64_t now = MonoNs();\n'
         '        if (now >= nextSmoothSettings) {\n'
         '            nextSmoothSettings = now + 750\'000\'000ll;\n'
         '            const int level = std::clamp(std::atoi(handcut::Setting("HANDS_SMOOTHNESS", "50").c_str()), 0, 100);\n'
         '            holdK.blendMinMs = 5.0 + 0.22 * level;\n'
         '            holdK.blendMaxMs = 35.0 + 1.3 * level;\n'
         '            holdK.blendFactor = 0.3 + 0.023 * level;\n'
         '        }\n')

    # Pair the newest *matching* side-camera frames, instead of rejecting
    # everything when one camera has advanced a frame before the other.
    # This is a concrete candidate for the intermittent sub-2-Hz stalls.
    edit('hands/track/main.cpp',
         '            if (!met || t1 - t0 > 3\'000\'000) {   // one is a frame ahead: wait for the other\n'
         '                std::this_thread::sleep_for(std::chrono::milliseconds(1));\n'
         '                continue;\n'
         '            }\n',
         '            if (!met || t1 - t0 > 3\'000\'000) {\n'
         '                // Side-camera reads can straddle a camera tick. Search the last four\n'
         '                // unconsumed frames for a truly synchronous pair before dropping\n'
         '                // the whole set. Never combine frames taken 30 ms apart.\n'
         '                bool matched = false;\n'
         '                if (lead.size() == 2) {\n'
         '                    const auto &a = lead[0], &b = lead[1];\n'
         '                    uint64_t bestA = 0, bestB = 0, bestGap = 3\'000\'001;\n'
         '                    fh_ring_slot_t bestAM{}, bestBM{};\n'
         '                    for (int j = 0; j < 4; ++j) {\n'
         '                        const uint64_t na = ring.latest(index[a]);\n'
         '                        if (na <= uint64_t(j) || na - j <= last[a]) break;\n'
         '                        fh_ring_slot_t ma{};\n'
         '                        if (!ring.meta(index[a], na - j, &ma)) continue;\n'
         '                        for (int k = 0; k < 4; ++k) {\n'
         '                            const uint64_t nb = ring.latest(index[b]);\n'
         '                            if (nb <= uint64_t(k) || nb - k <= last[b]) break;\n'
         '                            fh_ring_slot_t mb{};\n'
         '                            if (!ring.meta(index[b], nb - k, &mb)) continue;\n'
         '                            const uint64_t gap = ma.capture_ns > mb.capture_ns\n'
         '                                ? ma.capture_ns - mb.capture_ns : mb.capture_ns - ma.capture_ns;\n'
         '                            if (gap <= 3\'000\'000 && (gap < bestGap ||\n'
         '                                (gap == bestGap && na - j + nb - k > bestA + bestB))) {\n'
         '                                bestGap = gap; bestA = na - j; bestB = nb - k;\n'
         '                                bestAM = ma; bestBM = mb; matched = true;\n'
         '                            }\n'
         '                        }\n'
         '                    }\n'
         '                    if (matched) {\n'
         '                        latest[a] = bestA; latest[b] = bestB;\n'
         '                        t0 = std::min(bestAM.capture_ns, bestBM.capture_ns);\n'
         '                        t1 = std::max(bestAM.capture_ns, bestBM.capture_ns);\n'
         '                        lead_dq = std::max(bestAM.dqbuf_ns, bestBM.dqbuf_ns);\n'
         '                    }\n'
         '                }\n'
         '                if (!matched) {\n'
         '                    std::this_thread::sleep_for(std::chrono::milliseconds(1));\n'
         '                    continue;\n'
         '                }\n'
         '            }\n')

    # Persisted dashboard controls. Bottom-left y=0 in OpenVR mouse events.
    edit('dashboard/framehands-dashboard.cpp',
         'constexpr int W = 640, H = 360;', 'constexpr int W = 640, H = 480;')
    edit('dashboard/framehands-dashboard.cpp',
         'void Paint(bool enabled, bool busy, bool failed) {',
         'bool TouchEnabled() {\n'
         '    const char *home = std::getenv("HOME");\n'
         '    if (!home) return false;\n'
         '    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");\n'
         '    std::string line; bool enabled = false;\n'
         '    while (std::getline(f, line))\n'
         '        if (line == "HANDS_TOUCH=1") enabled = true;\n'
         '        else if (line == "HANDS_TOUCH=0") enabled = false;\n'
         '    return enabled;\n'
         '}\n'
         'int Smoothness() {\n'
         '    const char *home = std::getenv("HOME");\n'
         '    if (!home) return 50;\n'
         '    std::ifstream f(std::filesystem::path(home) / ".config/framehands/config");\n'
         '    std::string line; int level = 50;\n'
         '    while (std::getline(f, line)) if (line.rfind("HANDS_SMOOTHNESS=", 0) == 0) {\n'
         '        try { level = std::stoi(line.substr(17)); } catch (...) {}\n'
         '    }\n'
         '    return std::clamp(level, 0, 100);\n'
         '}\n'
         'void Paint(bool enabled, bool touch, int smoothness, bool busy, bool failed) {')
    edit('dashboard/framehands-dashboard.cpp',
         '    Text(failed ? "FAILED - CHECK LOGS" : enabled ? "CLICK TO TURN OFF" : "CLICK TO TURN ON",\n'
         '         293, 2, {204, 220, 233});\n',
         '    Text(failed ? "FAILED - CHECK LOGS" : enabled ? "CLICK TO TURN OFF" : "CLICK TO TURN ON",\n'
         '         276, 2, {204, 220, 233});\n'
         '    Rect(76, 306, 564, 356, touch ? RGB{26, 133, 115} : RGB{69, 83, 107});\n'
         '    Text(touch ? "DIRECT TOUCH ON" : "DIRECT TOUCH OFF", 316, 3, {245, 250, 255});\n'
         '    Text("HAND SMOOTHNESS", 369, 2, {204, 220, 233});\n'
         '    Rect(100, 409, 540, 425, {52, 69, 90});\n'
         '    const int knob = 100 + 440 * std::clamp(smoothness, 0, 100) / 100;\n'
         '    Rect(100, 409, knob, 425, {73, 175, 235});\n'
         '    Rect(knob-6, 402, knob+6, 432, {237, 245, 255});\n'
         '    Text("SMOOTHNESS " + std::to_string(smoothness), 444, 2, {204, 220, 233});\n')
    edit('dashboard/framehands-dashboard.cpp',
         '    bool enabled = Enabled(), busy = false, failed = false;\n',
         '    bool enabled = Enabled(), touch = TouchEnabled(), busy = false, failed = false;\n'
         '    int smoothness = Smoothness();\n'
         '    bool sliding = false;\n')
    edit('dashboard/framehands-dashboard.cpp',
         '            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {\n',
         '            if (event.eventType == vr::VREvent_MouseMove && sliding) {\n'
         '                smoothness = std::clamp(int((event.data.mouse.x - 100.f) * 100.f / 440.f), 0, 100);\n'
         '                Paint(enabled, touch, smoothness, busy, failed);\n'
         '                vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);\n'
         '            }\n'
         '            if (event.eventType == vr::VREvent_MouseButtonUp && sliding) {\n'
         '                sliding = false;\n'
         '                child = fork();\n'
         '                if (child == 0) {\n'
         '                    const std::string cli = Cli().string();\n'
         '                    const std::string level = std::to_string(smoothness);\n'
         '                    execl(cli.c_str(), cli.c_str(), "smoothness", level.c_str(), (char *)nullptr);\n'
         '                    _exit(127);\n'
         '                }\n'
         '                busy = child > 0; failed = child < 0;\n'
         '            }\n'
         '            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {\n')
    edit('dashboard/framehands-dashboard.cpp',
         '                const float y = event.data.mouse.y;\n',
         '                const float y = H - event.data.mouse.y; // GL mouse coordinates are bottom-left.\n')
    edit('dashboard/framehands-dashboard.cpp',
         '                // Accept both OpenVR\'s mouse-coordinate orientations for the X.\n'
         '                if (x >= 566 && x <= 628 && (y <= 100 || y >= H - 100)) {\n',
         '                if (x >= 566 && x <= 628 && y >= 22 && y <= 94) {\n')
    edit('dashboard/framehands-dashboard.cpp',
         '                // Otherwise preserve the original full-panel toggle behaviour.\n'
         '                const bool toEnable = !enabled;\n',
         '                // Slider changes are committed on mouse-up, not on every move.\n'
         '                if (x >= 82 && x <= 554 && y >= 390 && y <= 439) {\n'
         '                    sliding = true;\n'
         '                    smoothness = std::clamp(int((x - 100.f) * 100.f / 440.f), 0, 100);\n'
         '                    Paint(enabled, touch, smoothness, busy, failed);\n'
         '                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);\n'
         '                    continue;\n'
         '                }\n'
         '                if (x >= 76 && x <= 564 && y >= 306 && y <= 356) {\n'
         '                    const bool toEnable = !touch;\n'
         '                    child = fork();\n'
         '                    if (child == 0) {\n'
         '                        const std::string cli = Cli().string();\n'
         '                        execl(cli.c_str(), cli.c_str(), toEnable ? "touch-on" : "touch-off", (char *)nullptr);\n'
         '                        _exit(127);\n'
         '                    }\n'
         '                    busy = child > 0; failed = child < 0;\n'
         '                    Paint(enabled, touch, smoothness, busy, failed);\n'
         '                    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);\n'
         '                    continue;\n'
         '                }\n'
         '                if (!(x >= 76 && x <= 564 && y >= 165 && y <= 277)) continue;\n'
         '                const bool toEnable = !enabled;\n')
    # Replace remaining paint calls *globally* after all targeted changes.
    p = ROOT / 'dashboard/framehands-dashboard.cpp'
    ss = p.read_text(encoding='utf-8')
    count = ss.count('Paint(enabled, busy, failed);')
    if count:
        p.write_text(ss.replace('Paint(enabled, busy, failed);', 'Paint(enabled, touch, smoothness, busy, failed);'), encoding='utf-8')
        if 'dashboard/framehands-dashboard.cpp' not in EDITED: EDITED.append('dashboard/framehands-dashboard.cpp')
    edit('dashboard/framehands-dashboard.cpp',
         '                enabled = Enabled();\n',
         '                enabled = Enabled();\n                touch = TouchEnabled();\n                smoothness = Smoothness();\n')
    edit('dashboard/framehands-dashboard.cpp',
         '            const bool current = Enabled();\n'
         '            if (current != enabled) {\n'
         '                enabled = current;\n',
         '            const bool current = Enabled(), currentTouch = TouchEnabled();\n'
         '            const int currentSmooth = Smoothness();\n'
         '            if (!sliding && (current != enabled || currentTouch != touch || currentSmooth != smoothness)) {\n'
         '                enabled = current; touch = currentTouch; smoothness = currentSmooth;\n')

    # Install watchdog with the same SteamVR lifetime and limited, logged recovery.
    add('services/framehands-watchdog.service', '''[Unit]
Description=FrameHands frame-rate recovery monitor
After=steamvr.service framehands-track.service
Requisite=steamvr.service
PartOf=steamvr.service

[Service]
ExecStart=@ROOT@/framehands run-watchdog
Restart=on-failure
RestartSec=5
TimeoutStopSec=5

[Install]
WantedBy=steamvr.service
''')
    # Installer has multiple unit lists; replace all in consistent locations.
    p = ROOT/'install.sh'
    content = p.read_text(encoding='utf-8')
    content = content.replace('framehands-input.service framehands-dashboard.service; do',
                              'framehands-input.service framehands-dashboard.service framehands-watchdog.service; do')
    content = content.replace('systemctl --user stop framehands-dashboard.service framehands-input.service',
                              'systemctl --user stop framehands-watchdog.service framehands-dashboard.service framehands-input.service')
    p.write_text(content,encoding='utf-8')
    edit('uninstall.sh', 'for unit in framehands-dashboard.service framehands-input.service',
         'for unit in framehands-watchdog.service framehands-dashboard.service framehands-input.service')
    # Steam Frame's + menu reads XDG desktop entries, not OpenVR manifests.
    launcher_block = """# FrameHands SteamOS application launcher
launcher_dir="$HOME/.local/share/applications"
mkdir -p "$launcher_dir"
cat > "$launcher_dir/framehands-dashboard.desktop" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=FrameHands
Comment=Hand tracking controls
Exec=$app/dashboard/launch.sh
Icon=preferences-system
Terminal=false
Categories=Utility;
EOF
chmod 644 "$launcher_dir/framehands-dashboard.desktop"
"""
    edit('install.sh',
         "echo 'FrameHands installed. Restart SteamVR normally to load its input driver, then run: ~/.local/bin/framehands status'",
         launcher_block + "\necho 'FrameHands installed. Restart SteamVR normally to load its input driver, then run: ~/.local/bin/framehands status'")
    edit('uninstall.sh', 'link=$HOME/.local/bin/framehands\n',
         '# Remove only our own FrameHands desktop entry.\n'
         'launcher_file="$HOME/.local/share/applications/framehands-dashboard.desktop"\n'
         'if [[ -f "$launcher_file" ]] && grep -Fxq "Exec=$app/dashboard/launch.sh" "$launcher_file"; then\n'
         '  rm -f -- "$launcher_file"\n'
         'fi\n'
         'link=$HOME/.local/bin/framehands\n')
    edit('framehands', 'def main(argv=None):\n', r'''def watchdog():
    """Recover only our services after persistent low publish rates.

    The tracker normally publishes at >=5 Hz with no hands; the sequence
    number is monotonically incremented twice on every published set.
    This runs separately because a blocked tracker cannot restart itself.
    """
    prev = None
    bad = 0
    last_recovery = 0.0
    while True:
        time.sleep(2)
        cfg = config()
        if (cfg.get("AUTO_RECOVER", "1") == "0" or cfg.get("AUTO_START", "1") == "0"
                or not selected() or not active("steamvr.service")
                or not active("framehands-track.service") or not active("framehands-camd.service")
                or (RUN / "park").exists()):
            prev, bad = None, 0
            continue
        data = shared(RUN / "hands", b"FHHANDS1", 64)
        seq = struct.unpack_from("<Q", data, 16)[0] if data else None
        if seq is None:
            bad += 1
        elif prev is not None:
            rate = ((seq - prev) // 2) / 2 if seq >= prev else 0
            bad = bad + 1 if rate < 2.0 else 0
        prev = seq
        if bad < 3 or time.monotonic() - last_recovery < 90:
            continue
        # Never touch SteamVR, XRService, files, or other applications.
        print("framehands watchdog: under 2 published sets/s for >=6s; restarting FrameHands services", flush=True)
        last_recovery = time.monotonic()
        bad, prev = 0, None
        stopped = ctl("stop", *reversed(UNITS))
        if stopped.returncode:
            print("framehands watchdog: stop failed:", stopped.stderr.strip(), flush=True)
            continue
        started = ctl("start", *selected())
        if started.returncode:
            print("framehands watchdog: start failed:", started.stderr.strip(), flush=True)


def main(argv=None):
''')

    # Add smoke unit tests for settings and watchdog selection without ARM64 hardware.
    add('tests/test_controls.py', '''import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("framehands_controls_cli", str(ROOT / "framehands"))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)

class ControlTests(unittest.TestCase):
    def test_touch_and_smoothness_persist(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "CONFIG", Path(tmp)/"config"):
            self.assertEqual(cli.main(["touch-on"]), 0)
            self.assertEqual(cli.main(["smoothness", "95"]), 0)
            self.assertEqual(cli.config()["HANDS_TOUCH"], "1")
            self.assertEqual(cli.config()["HANDS_SMOOTHNESS"], "95")
            self.assertEqual(cli.main(["touch-off"]), 0)
            self.assertEqual(cli.config()["HANDS_TOUCH"], "0")
            self.assertEqual(cli.main(["smoothness", "101"]), 1)
            self.assertEqual(cli.config()["HANDS_SMOOTHNESS"], "95")

    def test_watchdog_is_only_enabled_when_tracking_is_autostarted(self):
        from types import SimpleNamespace
        commands=[]
        def fake_ctl(*a,**_):
            commands.append(a)
            return SimpleNamespace(returncode=0,stderr="")
        with patch.object(cli,"ctl", side_effect=fake_ctl):
            with patch.object(cli,"config",return_value={"AUTO_START":"1","TRACKING":"1","AUTO_RECOVER":"1"}):
                cli.sync_enabled()
                self.assertIn(("enable",cli.WATCHDOG_UNIT),commands)
            commands.clear()
            with patch.object(cli,"config",return_value={"AUTO_START":"0","TRACKING":"1","AUTO_RECOVER":"1"}):
                cli.sync_enabled()
                self.assertIn(("disable",cli.WATCHDOG_UNIT),commands)

if __name__ == "__main__": unittest.main()
''')

    # Run the new Python control tests on every native ARM64 CI build, not only
    # when someone remembers to test locally on Windows.
    edit('.github/workflows/release.yml',
         '      - name: Check binary architecture and external libraries\n',
         '      - name: Validate FrameHands service and dashboard commands\n'
         '        run: python3 -m unittest discover -s tests -v\n'
         '      - name: Check binary architecture and external libraries\n')

    # Short documentation and key bindings, merged into user's existing README.
    p = ROOT/'README.md'
    text = p.read_text(encoding='utf-8').replace('\r\n','\n')
    appendix = '''
## Floating panels, direct touch, smoothness, and recovery

The hand pointer now remains available while the SteamVR dashboard is hidden
(`HANDS_FLOATING_POINTER=1`), permitting SteamVR to route its synthetic
controller ray to compatible **Float in World** panels. On SteamVR builds
that expose it, turn on **Settings > Dashboard > Allow Undocked Dashboard
Overlay Interaction** as well. This relies on the
panel's existing SteamVR laser-mouse support and has not been verified on every
floating overlay implementation. Disable it with `HANDS_FLOATING_POINTER=0`
if you do not want a virtual pointer outside the dashboard.

The FrameHands panel now includes **Direct Touch** (default off) and a
**Smoothness** slider (0–100, default 50). Both settings persist in
`~/.config/framehands/config` and update live without restarting camera
tracking. Smoothness affects the rendered hand interpolation and laser aim;
it cannot recover missing camera data. For direct touch, FrameHands currently
checks its **own** visible dashboard overlay using OpenVR's
`ComputeOverlayIntersection`. When a fingertip approaches the overlay, it
switches the pointer to a fingertip ray and sends a click with contact/release
hysteresis. The helper probes known Valve dashboard and desktop overlay keys (including
`system.desktop.1`) and uses the compositor's 3D intersection; arbitrary
third-party panels still require overlay discovery or integration, so touch
is **not** claimed to work everywhere yet. Pinch-at-distance
is unchanged.

`AUTO_RECOVER=1` (default) runs a lightweight watchdog under SteamVR. The
tracker normally publishes at least 5 sets/s, even when no hands are present.
The tracker also scans up to four recent camera frames when its side cameras
are momentarily out of sync, rather than discarding every set. If its
published rate nevertheless falls below 2 sets/s for approximately six
seconds while the camera broker and tracking services are active, the watchdog
restarts only the four FrameHands tracking/input services. Recovery attempts are at least 90 seconds
apart. It does not restart SteamVR or XRService. Disable with
`AUTO_RECOVER=0` followed by `framehands sync`. This is a recovery mechanism,
not a confirmed root-cause fix for all 1-fps stalls; check
`framehands logs` to diagnose camera desynchronisation and inference overload.

The + application menu shortcut should remain installed via
`~/.local/share/applications/framehands-dashboard.desktop`. This update does
not alter desktop-launcher registration.
'''
    if '## Floating panels, direct touch, smoothness, and recovery' not in text:
        p.write_text(text+appendix,encoding='utf-8')
        EDITED.append('README.md')

except (RuntimeError, OSError) as exc:
    print('Patch halted:', exc, file=sys.stderr)
    print('Previously modified files:', ', '.join(EDITED) or 'none', file=sys.stderr)
    sys.exit(1)
print('FrameHands update applied to', len(EDITED), 'files:')
print('\n'.join('  '+p for p in EDITED))
print('Next: git diff --check; python -m unittest discover -s tests -v')
