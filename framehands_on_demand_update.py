#!/usr/bin/env python3
"""Make FrameHands a launch-on-demand SteamVR dashboard app (run from repo root).

Compatible with the dashboard feature merged into main in November 2026.
No external Python dependencies; validates all replacements before writing files.
"""
from pathlib import Path
import sys

root = Path.cwd()
changes = {}


def source(path):
    p = root / path
    if not p.is_file():
        raise RuntimeError(f'Missing {path}. Run this in your frametop-hands-only repository.')
    return changes.setdefault(path, p.read_text(encoding='utf-8'))


def replace(path, old, new):
    data = source(path)
    if new in data:
        return  # safely repeat on an already patched tree
    occurrences = data.count(old)
    if occurrences != 1:
        raise RuntimeError(f'{path}: expected one matching code block but found {occurrences}. '
                           'Your checkout differs; no files were written.')
    changes[path] = data.replace(old, new, 1)


# The old service started the UI on every SteamVR boot, creating a permanent tab.
# A oneshot registration service keeps the app listed under + without opening it.
replace('services/framehands-dashboard.service',
'''[Service]
ExecStart=@ROOT@/framehands run-dashboard
Restart=on-failure
RestartSec=3
TimeoutStopSec=5
''',
'''[Service]
# Register FrameHands in SteamVR's + app menu without creating a dashboard tab.
# The actual UI is started by SteamVR only when the user chooses FrameHands.
Type=oneshot
ExecStart=@ROOT@/framehands register-dashboard
TimeoutStopSec=5
''')

replace('framehands',
'''"run-input", "run-dashboard"))''',
'''"run-input", "run-dashboard", "register-dashboard"))''')
replace('framehands',
'''        elif cmd == "run-dashboard":
            binary = str(ROOT / "dashboard/build/framehands-dashboard")
            os.execv(binary, (binary,))
''',
'''        elif cmd == "run-dashboard":
            binary = str(ROOT / "dashboard/build/framehands-dashboard")
            os.execv(binary, (binary,))
        elif cmd == "register-dashboard":
            binary = str(ROOT / "dashboard/build/framehands-dashboard")
            os.execv(binary, (binary, "--register"))
''')

replace('dashboard/framehands-dashboard.cpp',
'''// Small standalone SteamVR dashboard overlay; no Qt, browser or desktop needed.
// Its systemd unit survives disabling the four hand-tracking services.
''',
'''// On-demand SteamVR dashboard overlay, launched from the + menu.
// A separate oneshot systemd unit registers the app; this UI is never auto-started.
// Closing the UI destroys the overlay but does NOT change hand-tracking settings.
''')

# Add a small close button at the top right; it is outside the full-width toggle.
replace('dashboard/framehands-dashboard.cpp',
'''void Paint(bool enabled, bool busy, bool failed) {
    Rect(0, 0, W, H, {13, 22, 38});
    Rect(0, 0, W, 10, {73, 175, 235});
    Text("FRAMEHANDS", 39, 5, {232, 244, 255});
''',
'''void Paint(bool enabled, bool busy, bool failed) {
    Rect(0, 0, W, H, {13, 22, 38});
    Rect(0, 0, W, 10, {73, 175, 235});
    // The X is separate from the toggle: closing never disables tracking.
    Rect(566, 22, 628, 94, {44, 62, 83});
    for (int i = 0; i < 23; ++i) {
        Rect(581 + i, 43 + i, 585 + i, 47 + i, {232, 244, 255});
        Rect(603 - i, 43 + i, 607 - i, 47 + i, {232, 244, 255});
    }
    Text("FRAMEHANDS", 39, 5, {232, 244, 255});
''')

replace('dashboard/framehands-dashboard.cpp',
'''    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
    bool running = true;
    while (running) {
''',
'''    vr::VROverlay()->SetOverlayRaw(mainOverlay, pixels.data(), W, H, 4);
    // Opening FrameHands from + selects this panel immediately.
    vr::VROverlay()->ShowDashboard("framehands.dashboard");
    bool running = true;
    bool wasActive = false;
    while (running) {
''')

replace('dashboard/framehands-dashboard.cpp',
'''        while (vr::VROverlay()->PollNextOverlayEvent(mainOverlay, &event, sizeof event)) {
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const bool toEnable = !enabled;
''',
'''        while (running && vr::VROverlay()->PollNextOverlayEvent(mainOverlay, &event, sizeof event)) {
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
            if (event.eventType == vr::VREvent_MouseButtonDown && !busy) {
                const float x = event.data.mouse.x;
                const float y = event.data.mouse.y;
                // Accept both OpenVR's mouse-coordinate orientations for the X.
                if (x >= 566 && x <= 628 && (y <= 100 || y >= H - 100)) {
                    running = false;
                    break;
                }
                // Otherwise preserve the original full-panel toggle behaviour.
                const bool toEnable = !enabled;
''')

replace('dashboard/framehands-dashboard.cpp',
'''        while (vr::VRSystem()->PollNextEvent(&event, sizeof event))
            if (event.eventType == vr::VREvent_Quit) running = false;
        if (child > 0) {
''',
'''        while (vr::VRSystem()->PollNextEvent(&event, sizeof event))
            if (event.eventType == vr::VREvent_Quit ||
                event.eventType == vr::VREvent_OverlayClosed) running = false;
        if (vr::VROverlay()->IsActiveDashboardOverlay(mainOverlay)) wasActive = true;
        if (!running) break;
        if (child > 0) {
''')

# Explain why the tab now appears only when launched, but settings persist.
path = 'README.md'
data = source(path)
heading = '\n\n## On-demand dashboard app (+ menu)\n'
if heading not in data:
    changes[path] = data.rstrip() + heading + '''
FrameHands is registered with SteamVR by a quick **oneshot** user service. The
service does **not** launch the overlay UI automatically. Open the SteamVR
Dashboard, press **+**, and select **FrameHands** to launch the controls.

Use the ON/OFF button to set tracking, then press the **X** at the top-right
of the panel to close it. You can also leave the panel by selecting another
dashboard tab or dismissing the dashboard; the panel exits and its tab disappears.
The ON/OFF selection is saved in `~/.config/framehands/config` and remains in
effect after the UI exits. Tracking is still auto-started by SteamVR when enabled.

The small `framehands-dashboard.service` now registers the app, rather than
keeping it open. The `framehands-dashboard` binary is launched only when
SteamVR launches the manifest from **+**. On an existing installation, install
a new published ARM64 release and restart SteamVR to replace the old service.

**Important:** Disabling hand tracking also disables the hand pointer. You
will need a physical controller or SSH (`framehands enable`) to switch it back
on. The settings persist even if the panel is closed while changing them.
'''

path = 'tests/test_framehands.py'
data = source(path)
new_test = '''    def test_dashboard_is_registered_but_not_autolaunched(self):
        service = (ROOT / "services/framehands-dashboard.service").read_text()
        self.assertIn("Type=oneshot", service)
        self.assertIn("ExecStart=@ROOT@/framehands register-dashboard", service)
        self.assertNotIn("ExecStart=@ROOT@/framehands run-dashboard\\n", service)
        self.assertIn("register-dashboard", (ROOT / "framehands").read_text())

    def test_dashboard_can_be_closed_without_disabling_tracking(self):
        cpp = (ROOT / "dashboard/framehands-dashboard.cpp").read_text()
        self.assertIn("vr::VREvent_OverlayClosed", cpp)
        self.assertIn("vr::VREvent_OverlayHidden && wasActive", cpp)
        self.assertIn("ShowDashboard(\\"framehands.dashboard\\")", cpp)
        self.assertIn("running = false", cpp)

'''
if 'def test_dashboard_is_registered_but_not_autolaunched' not in data:
    old = '\nif __name__ == "__main__":\n    unittest.main()\n'
    if data.count(old) != 1:
        raise RuntimeError('tests/test_framehands.py has a different ending; no files were written.')
    changes[path] = data.replace(old, '\n' + new_test + old)

# Only after ALL changes have matched; deliberately leave the installed config
# and existing tracking code untouched.
for path, content in changes.items():
    if content != (root / path).read_text(encoding='utf-8'):
        (root / path).write_text(content, encoding='utf-8', newline='\n')
        print('Updated', path)
print('FrameHands on-demand dashboard patch applied. Run tests, commit, then publish a new ARM64 release.')
