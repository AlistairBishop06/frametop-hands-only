# FrameHands

FrameHands brings camera-based hand tracking to the Steam Frame. It tracks both hands using the headset's cameras and shows them in SteamVR. It is a standalone fork of [Frametop](https://github.com/DeeJanuz/frametop) and does not replace the normal SteamVR dashboard.

## Video

https://github.com/user-attachments/assets/c54dd16b-259a-4c39-ab1a-a1fc931f3f94

## What it does

- Shows tracked 3D hands in SteamVR, with eight colour presets and adjustable smoothing.
- Lets you point, pinch to click, drag, and optionally touch supported dashboard panels with your fingertips.
- Adds wrist rotation to pinch drags so SteamVR can tilt a floating window from its grab handle.
- Provides a FrameHands dashboard panel to control tracking, hand visuals, touch, and updates.
- Can pause tracking while a local VR game is active. Detection of PC-streamed games is not yet reliable.

FrameHands does not yet provide native hand controls or skeletal input to VR games such as *Half-Life: Alyx*.

## Install

On a **Steam Frame** running ARM64 SteamOS, open Konsole in the logged-in steamos session. You need your `sudo` password. Run the installer as `steamos`, not with `sudo`:

```bash
git clone https://github.com/AlistairBishop06/frametop-hands-only.git
cd frametop-hands-only
./install.sh
```

Open **FrameHands** from the Steam Frame's **+ → Launch Application** menu. To update later, use **Check for Updates** in that panel.

## Uninstall

From the source checkout on the Frame:

```bash
cd ~/frametop-hands-only
./uninstall.sh
```

This removes the FrameHands app, launcher, services, and SteamVR registration while keeping your settings. To also remove saved settings and cached models, run `./uninstall.sh --config --models`; the script asks before deleting them. SteamVR and Frametop are unaffected.
