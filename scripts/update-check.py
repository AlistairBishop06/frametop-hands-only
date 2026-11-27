#!/usr/bin/env python3
"""Read-only SteamOS interface checks after an update. Never starts SteamVR."""
from pathlib import Path
import platform
import sys

ROOT = Path(__file__).resolve().parents[1]
checks = {
    "Steam Frame architecture": platform.machine() == "aarch64",
    "SteamVR OpenVR ARM64 API": Path("/opt/steamvr/bin/linuxarm64/libopenvr_api.so").is_file(),
    "SteamVR driver registration": Path("/opt/steamvr/bin/linuxarm64/vrpathreg").is_file(),
    "XRService camera calibration": Path("/persist/xrservice.json").is_file(),
    "device camera calibration": Path("/persist/device_config.json").is_file(),
    "v4l2 dequeue tracepoint": Path("/sys/kernel/tracing/events/v4l2/v4l2_dqbuf/format").is_file(),
}
for name, ok in checks.items():
    print(f"{'ok' if ok else 'MISSING'}: {name}")
if any(not ok for ok in checks.values()):
    print("Retest fh-camd camera attachment and calibration before using FrameHands.")
print("Retest the OpenVR DMA-BUF hand overlay and Room View detection on the headset after a SteamVR update.")
print("Retest the FrameHands driver role handoff, dashboard ray, pinch press/release, and physical controllers after a SteamVR update.")
print("Retest delayed pointer laser selection when opening the dashboard, single-camera hand acquisition, and physical-controller handoff.")
print("Retest dashboard release checks, checksum verification, install failure recovery, and the restart prompt after SteamOS updates.")
print("Retest that game-pause releases virtual pointer inputs, opts out of the hand role, and reports a disconnected pose without restarting SteamVR.")
print("Retest PAUSE_IN_GAMES in SteamVR Home/Aurora, a local game, and a streamed VR game; inspect the scene app key and resume on exit.")
print("Retest Direct Touch overlay discovery, GetTransformForOverlayCoordinates, and fingertip contact on the dashboard and floated panels after a SteamVR update.")
print("Retest Interaction Mode switching, head-pointed gaze hover on the dashboard and floating windows, pinch clicks from either hand, and physical-controller handoff.")
print("Retest that Gaze Assisted shortens the beam at the hovered panel and pinch still clicks the dot when moving between nearby and distant overlays.")
print("Retest that the gaze beam stays out of view away from panels, and that floating windows follow head movement during a pinch and stay in place on release.")
print("Retest that the optional two-fist gesture toggles the SteamVR menu once per hold and becomes ready again after both hands open.")
print("Retest small-button clicks, small head movements while holding the hand still, and deliberate pointer sweeps. Then move and tilt a floated panel and check that it keeps its position and tilt after release.")
sys.exit(0 if all(checks.values()) else 1)
