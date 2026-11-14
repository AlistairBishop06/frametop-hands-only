# FrameHands

**Camera-based hand tracking, hand overlays and pinch interaction for the Steam Frame.**

FrameHands uses the Steam Frame's built-in tracking cameras to track both hands, render posed 3D hands in SteamVR and optionally provide a hand-controlled pointer for the SteamVR dashboard. A small **FrameHands** control panel lets you turn tracking on or off without restarting the headset. Tracking continues after you close the panel.

FrameHands is derived from [Frametop](https://github.com/DeeJanuz/frametop) and runs independently of Frametop Desktop, KDE, Distrobox and Podman. It uses SteamVR's existing runtime and does not replace its normal dashboard.

> **Hardware support:** Steam Frame running ARM64 SteamOS and SteamVR. This is not a Windows application, a Steam Deck application or a generic PCVR hand-tracking driver. Check the [Releases](https://github.com/AlistairBishop06/frametop-hands-only/releases) page for published ARM64 builds.

## Features

- **Two-hand tracking:** Tracks 21 landmarks per hand and fits an articulated skeleton using the Frame's cameras.
- **Visible hands in SteamVR:** Renders separate left and right hand overlays, with adjustable appearance.
- **Smoother motion:** Adaptive filtering of hand positions, rotations and finger joints; interpolation and temporary pose holding help reduce jitter and sudden jumps.
- **Pinch and grip recognition:** Detects index pinches, middle-finger pinches and grips. The index pinch defaults to a more forgiving **25 mm** activation threshold.
- **Optional SteamVR dashboard input:** Aim a hand ray, pinch to click, hold to drag and use the supported middle-pinch actions. A physical controller takes precedence when appropriate.
- **On-demand control panel:** Open **FrameHands** from the Steam Frame's **+ → Launch Application** menu, toggle tracking and close the panel. The selected state persists.
- **Separate services:** The hand tracker, renderer, camera broker and optional input helper can stop without removing the application launcher.
- **Optional game pause:** Stop all FrameHands tracking services during an active VR game and resume them when it closes.
- **Local configuration:** Settings live in your home folder and are retained across normal upgrades.

FrameHands provides visual hand overlays and dashboard input, **not** native SteamVR skeletal input for arbitrary games. Games do not automatically gain hand controls merely because the overlays are visible.

## Quick start: install on a Steam Frame

### Requirements

Before installing, make sure:

1. You are on a **Steam Frame** running ARM64 SteamOS with SteamVR installed.
2. A **published** GitHub release exists with both `framehands-aarch64.tar.gz` and `SHA256SUMS`. A successful GitHub Actions run alone is not a published release.
3. You can use **Konsole** on the Frame, either locally or through SSH/remote desktop.
4. You have your SteamOS user's `sudo` password available for setting the camera broker's capabilities.

No system-wide package installation or immutable-root modification is required for installation from a prebuilt release.

### 1. Download and install

Open Konsole **on the Steam Frame** and run:

```bash
git clone https://github.com/AlistairBishop06/frametop-hands-only.git
cd frametop-hands-only
./install.sh
```

The installer checks the hardware and SteamVR prerequisites. If your checkout does not contain locally built ARM64 binaries, it downloads the **latest published release** from this fork, validates its checksum and installs the required files into:

```text
~/.local/share/framehands/app/
```

The release must contain the service files required by the checkout. Newer
release archives also record the exact source commit; the installer rejects a
different checkout before replacing the installed app. If you have pulled
commits newer than the latest published release, publish a matching ARM64
release before rerunning `./install.sh`.

The installer also creates user-level services, installs the `framehands` command in `~/.local/bin/`, registers the FrameHands SteamVR driver and creates the dashboard application's OpenVR manifest. It creates `~/.config/framehands/config` only if that file does not already exist.

> **Existing locally compiled binaries take precedence:** If the checkout contains all expected built binaries, `./install.sh` uses them instead of downloading GitHub Releases. Use a clean source-only checkout if you specifically want the published release binaries.

### 2. Add FrameHands to the Steam Frame **+ → Launch Application** menu

There are **two different application registration mechanisms**: an OpenVR manifest registers the SteamVR overlay, while the Frame's **Launch Application** menu also needs a Linux `.desktop` launcher. The current repository installer creates the OpenVR manifest but does **not** yet create this `.desktop` shortcut, so perform this **one-time** step on the Frame:

```bash
mkdir -p ~/.local/share/applications

cat > ~/.local/share/applications/framehands-dashboard.desktop <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=FrameHands
Comment=Enable or disable Steam Frame hand tracking
Exec=$HOME/.local/share/framehands/app/dashboard/launch.sh
Icon=preferences-system
Terminal=false
Categories=Utility;
EOF
```

Close and reopen the **+ → Launch Application** window. Look for **FrameHands**. If it does not appear immediately, restart SteamVR and try again.

The shortcut launches the existing dashboard control panel; it does **not** start another copy of the tracking service. The `.desktop` file is user-local and does not alter system files.

### 3. Restart SteamVR and verify

After installation, **fully restart SteamVR** so its new input driver and app registration can be detected. Then run:

```bash
~/.local/bin/framehands doctor
~/.local/bin/framehands status
```

Open the SteamVR dashboard, press **+**, choose **Launch Application**, and select **FrameHands**. Your panel should show the current tracking state.

## Using the FrameHands control panel

FrameHands is designed to behave like an on-demand dashboard application rather than a permanent taskbar tab.

1. Open SteamVR's dashboard and select **+ → Launch Application → FrameHands**.
2. Use **Hand Tracking** for cameras and tracking, **Hand Visuals** for the white 3D hands, and **Direct Touch** for fingertip clicks. The visuals switch leaves tracking and hand input running.
3. Adjust **Smoothness** if you want steadier hand motion and pointer aim.
4. Press the **X** in the panel's top-right corner to close it. You can also leave the panel by switching dashboard tabs. The selections remain in `~/.config/framehands/config`.

From SSH, `~/.local/bin/framehands visuals-off` hides the white hands and
`~/.local/bin/framehands visuals-on` shows them again without restarting tracking.

The lightweight `framehands-dashboard.service` is a **oneshot registration service**, not a permanently visible control panel. An **inactive** dashboard registration unit after it has run successfully is therefore not necessarily an error.

> **Important:** Turning tracking **OFF** also removes the virtual hand pointer. To turn it on again, use a physical controller or connect through remote desktop/SSH and run `~/.local/bin/framehands enable`. The panel cannot be operated with a hand pointer that has been switched off.

### Using your hands to operate the dashboard

The optional hand-input service is disabled by default. To enable it, edit:

```bash
nano ~/.config/framehands/config
```

Set:

```ini
GESTURE_INPUT=1
```

Then apply the change:

```bash
~/.local/bin/framehands sync
```

**Restart SteamVR after initial installation** to make sure the `fh_pointer` driver is loaded. With the dashboard open, raise a hand in the headset's camera view to aim a ray. An index pinch acts as the primary click; holding it can drag. Middle-finger pinches are recognised separately for supported secondary input. The virtual pointer is hidden when the dashboard closes, tracking becomes stale, or a physical controller takes precedence.

The rendering code excludes the synthetic `fh_pointer` controller from physical-controller proximity checks, so switching pointer hands should not automatically hide that hand's white overlay.

## Updating an existing installation

You **do not** need to uninstall the previous version. On the Frame:

```bash
cd ~/frametop-hands-only
git pull --ff-only origin main
./install.sh
```

Restart SteamVR, then check:

```bash
~/.local/bin/framehands doctor
~/.local/bin/framehands status
```

The installer stops and replaces the appropriate FrameHands user services, upgrades the installed binaries and preserves your existing `~/.config/framehands/config`.

**Release versus Actions artefact:** A manually triggered workflow builds an artefact, but the installer downloads from `releases/latest`. Make sure the new release is **published**, not a draft. If your checkout has locally compiled binaries, the installer will prefer those over the release download.

If Git reports `Your local changes to ... install.sh would be overwritten by merge`, you have a locally modified file. Check it first:

```bash
git status --short
git diff -- install.sh
```

To **retain** the change while updating, stash that file:

```bash
git stash push -m "Local FrameHands installer changes" -- install.sh
git pull --ff-only origin main
```

Alternatively, only if you **intend to discard** the local edit:

```bash
git restore install.sh
git pull --ff-only origin main
```

These Git commands modify the *source checkout*, not the separate installed application or its saved settings.

## Configuration and gesture sensitivity

Edit the user configuration file:

```bash
nano ~/.config/framehands/config
```

Common settings:

| Setting | Default | Purpose |
| --- | --- | --- |
| `AUTO_START` | `1` | Start selected tracking services with SteamVR. |
| `TRACKING` | `1` | Enable or disable hand tracking. |
| `RENDER` | `1` | Display the white 3D hand overlays; the Hand Visuals control changes this live. |
| `GESTURE_INPUT` | `0` | Enable the virtual pointer for the SteamVR dashboard. |
| `PAUSE_IN_GAMES` | `0` | Pause FrameHands services while SteamVR identifies a Steam game scene. |
| `HANDS_TOUCH` | `0` | Enable fingertip contact on supported SteamVR overlays; also starts the virtual pointer. |
| `HANDS_POINTER_SMOOTH` | `1` | Adaptive smoothing of the dashboard ray; `0` disables it. |
| `HANDS_PINCH_BEGIN` | `0.025` | Index-pinch activation distance in **metres** (25 mm). |
| `HANDS_PINCH_DIP` | `0` | Optional near-contact fast-tap detection; may increase false clicks. |
| `HANDS_MODEL` | `latest` | Pinned fine-tuned model, with stock fallback; `stock` selects stock. |
| `HANDS_VIEW_MODEL` | `asset` | Rendered hand model (`asset`, `babylon`, `mrtk3`, `godot`, `mrtk2`, `robot`, `capsules`). |
| `HANDS_VIEW_STYLE` | `ghost` | Hand appearance (`ghost` or `solid`). |
| `PERFORMANCE` | `balanced` | `balanced`, `quality` or reduced-power `power`. |
| `DEBUG` | `0` | More frequent logs and gesture diagnostics when set to `1`. |

### Make pinches easier to register

The default index pinch starts when the thumb and index fingertip distance falls below **0.025 m**. If deliberate pinches still fail to register, try:

```ini
HANDS_PINCH_BEGIN=0.028
```

Apply tracker changes:

```bash
~/.local/bin/framehands restart
```

Start with small adjustments and test both hands. A larger threshold detects more near-contact pinches but can also increase accidental clicks. `HANDS_PINCH_DIP=1` can catch faster near-miss taps, at the cost of more false activations. Neither option improves the underlying camera model's landmark accuracy.

### Make the pointer steadier

Adaptive filtering is enabled by default:

```ini
HANDS_POINTER_SMOOTH=1
```

It reduces small hand tremors while allowing faster response to deliberate movement. To compare the unsmoothed behaviour, set it to `0`, then restart the input service with `framehands restart` while tracking is enabled. Higher smoothing can trade responsiveness for steadiness; the current build does not expose a general-purpose numeric smoothing slider.

> **Config timing:** `framehands restart` restarts tracking-related services and requires SteamVR to be running and tracking to be enabled. Use `framehands sync` to apply startup, tracking, rendering and optional input-service selections.

## Command reference

Run the commands below using `~/.local/bin/framehands` if `~/.local/bin` is not in your shell's `PATH`.

| Command | What it does |
| --- | --- |
| `framehands enable` | Saves tracking/autostart as enabled and starts the selected services if SteamVR is active. |
| `framehands disable` | Disables autostart and stops the tracking, renderer and input services; keeps the dashboard app registered. |
| `framehands start` | Starts selected services when SteamVR is running. |
| `framehands stop` | Stops the four tracking-related services without changing their saved startup configuration. |
| `framehands restart` | Restarts selected tracking-related services to apply tracker/renderer changes. |
| `framehands sync` | Applies service enablement and starts/stops services to match the current configuration. |
| `framehands games-on` / `games-off` | Enables or disables automatic pausing while a VR game is active. |
| `framehands status` | Displays SteamVR and FrameHands service status and detected hands. |
| `framehands gestures` | Prints tracked index pinch, grip and middle-pinch states. |
| `framehands logs` | Shows recent FrameHands journal entries. |
| `framehands doctor` | Checks binaries, permissions, SteamVR and service health. |

## Troubleshooting

### FrameHands is missing from **+ → Launch Application**

Check the launcher exists:

```bash
ls -l ~/.local/share/applications/framehands-dashboard.desktop
```

If missing, complete the [one-time application shortcut step](#2-add-framehands-to-the-steam-frame--launch-application-menu). Also check the underlying launcher:

```bash
ls -l ~/.local/share/framehands/app/dashboard/launch.sh
```

To test the app independently of the `+` menu, run:

```bash
~/.local/share/framehands/app/dashboard/launch.sh
```

If this prints an error, inspect the terminal output and `framehands logs`. Restart SteamVR after installing or updating the application registration.

### The dashboard panel stays on the tab bar

Make sure you installed a release that includes the **on-demand dashboard** changes. The older implementation ran a persistent `framehands-dashboard.service`; the current service registers the overlay without starting the UI. Reinstall from a new published release and fully restart SteamVR. Use the panel's **X** button or switch dashboard tabs to close it.

### The panel closes but tracking stays on/off

This is the intended behaviour: **closing the app does not change tracking**. Open it again to change the selection, or use `framehands enable` / `framehands disable` over SSH.

### I switched tracking off and cannot turn it on again

The hand pointer stops with tracking. Re-enable through a physical controller or remote desktop/SSH:

```bash
~/.local/bin/framehands enable
```

### Hand pointer is not visible or pinches do not click

Verify `GESTURE_INPUT=1`, apply `framehands sync`, and check that the SteamVR driver was loaded after restarting SteamVR. The hand must be tracked and the normal SteamVR dashboard must be open. Use these diagnostics:

```bash
~/.local/bin/framehands status
~/.local/bin/framehands gestures
~/.local/bin/framehands logs
```

If the pointer is jittery, leave `HANDS_POINTER_SMOOTH=1`. If deliberate clicks are missed, adjust `HANDS_PINCH_BEGIN` in small increments and test again.

### Installer, camera or library errors

```bash
~/.local/bin/framehands doctor
systemctl --user status steamvr.service framehands-camd.service framehands-track.service framehands-view.service framehands-input.service framehands-dashboard.service
getcap ~/.local/share/framehands/app/hands/build/fh-camd
```

The camera broker depends on the Frame's calibration files and user-granted Linux capabilities. A missing SteamVR OpenVR interface, GPU dependency or stale camera stream may also prevent tracking or rendering. Inspect the service journal with `framehands logs`.

## Building and publishing ARM64 releases

The repository builds on GitHub Actions' native ARM64 Ubuntu runner. The build process fetches pinned hand models and OpenVR SDK files, runs offline tests and packages the tracker, overlays, input driver, dashboard app and required licences.

### Automatically create a draft release

From a Windows PC with your changes committed on `main`:

```powershell
git switch main
git pull --ff-only origin main
git push origin main
git tag v0.1.6
git push origin v0.1.6
```

**Replace `v0.1.6` with your next unused version tag.** Pushing a tag beginning with `v` triggers `.github/workflows/release.yml` and creates a **draft** release on successful completion. Visit [GitHub Releases](https://github.com/AlistairBishop06/frametop-hands-only/releases), check that the two assets are attached, and click **Publish release**.

Running the workflow manually with **Run workflow** uploads an Actions artefact instead; it does **not** create a draft release. A passed CI build alone also does not prove the updated app works correctly on the physical headset.

### Build locally (ARM64 Linux only)

The build script requires native ARM64 Linux plus build tools and graphics dependencies. From the repository root:

```bash
bash scripts/build.sh
```

You may set `OPENVR_LIB` to the directory containing an ARM64 `libopenvr_api.so` if SteamVR's library is not available in the default location. The Frame installer itself does not build from source.

## Uninstallation

From the source checkout on the Frame:

```bash
./uninstall.sh
```

To remove the optional models cache or user configuration as well, consult:

```bash
./uninstall.sh --models --config
```

Those extra deletions require confirmation unless you explicitly pass the script's `--yes` option. The default uninstall retains your personal configuration.

**Launcher cleanup:** The current uninstaller removes FrameHands' service files, app directory, CLI link and SteamVR registrations, but does **not** yet remove the manually created `+` menu `.desktop` shortcut. If you created it using the instructions above, remove that shortcut separately:

```bash
rm -f ~/.local/share/applications/framehands-dashboard.desktop
```

The uninstaller does not remove SteamVR, Frametop, desktop environments or unrelated applications.

## Architecture

| Component | Purpose |
| --- | --- |
| Camera broker (`fh-camd`) | Accesses frames from Steam Frame's XRService camera feed. |
| Tracker (`fh-hands`) | Detects and fits hands, smooths joints and publishes tracking/gesture data. |
| Renderer (`framehands-view`) | Displays tracked hand overlays in SteamVR. |
| Input helper (`fh-input`) and `fh_pointer` driver | Provides optional pinch-based SteamVR dashboard pointing. |
| Dashboard app (`framehands-dashboard`) | On-demand ON/OFF panel, separate from the tracking services. |
| CLI (`framehands`) | Manages configuration, systemd user services and diagnostics. |

The tracker publishes shared-memory data under `/run/user/$UID/framehands/`, including hand and gesture information. Relevant data structures are in `hands/include/fh_hands.h` and `hands/include/fh_gestures.h`. The renderer and dashboard interface use OpenVR; the renderer also relies on GPU and shared-texture facilities supported by the Steam Frame runtime.

## Credits and licences

FrameHands retains hand tracking, camera broker and rendering work from [Frametop](https://github.com/DeeJanuz/frametop), copyright © 2026 DeeJanuz, under the repository's MIT [`LICENSE`](LICENSE). The camera feed notice is in `hands/camd/LICENSE.FrameEyeCameraFeed`. Bundled stock models and fine-tuned model notices are provided in `hands/models/NOTICE`, with the fine-tuned model revision pinned by `hands/models.pin`. Rendered hand assets and their licences are under `renderer/assets/hands/`. The release also includes the pinned JsonCpp source licence.

## Floating panels, direct touch, smoothness, and recovery

The **Pause for Games** dashboard toggle enables a separate OpenVR scene monitor.
When SteamVR identifies a Steam game scene, it stops the FrameHands camera broker, tracker, hand
renderer and input helper. It leaves the dashboard and SteamVR running. After
the scene closes, it starts only the services selected by the saved FrameHands
settings. The option is off by default and can also be changed with
`framehands games-on` or `framehands games-off`. SteamVR Home and unidentified
scenes leave tracking running. Streamed games must be checked on the headset:
the monitor relies on the headset's OpenVR scene application key, which has
not been verified for every streaming mode. A desktop Chromium window or
other application shown on a floated screen does not count unless SteamVR
actually makes it the active VR scene with a Steam app key. OpenVR does not
identify whether every such scene is a game, so leave the toggle off for
unusual VR browser or theatre apps until their app keys are checked.

The hand pointer now remains available while the SteamVR dashboard is hidden
(`HANDS_FLOATING_POINTER=1`), permitting SteamVR to route its synthetic
controller ray to compatible **Float in World** panels. On SteamVR builds
that expose it, turn on **Settings > Dashboard > Allow Undocked Dashboard
Overlay Interaction** as well. This relies on the
panel's existing SteamVR laser-mouse support and has not been verified on every
floating overlay implementation. Disable it with `HANDS_FLOATING_POINTER=0`
if you do not want a virtual pointer outside the dashboard.

The FrameHands panel includes **Hand Visuals** (default on), **Direct Touch**
(default off), and a **Smoothness** slider (0–100, default 50). The settings persist in
`~/.config/framehands/config` and update live without restarting camera
tracking. Enabling Direct Touch starts the input service even when
`GESTURE_INPUT=0`; disabling it stops that service unless gesture input is
also enabled. You can also run `~/.local/bin/framehands touch-on` or
`~/.local/bin/framehands touch-off` over SSH. Smoothness affects the rendered
hand interpolation and laser aim; it cannot recover missing camera data.
For direct touch, FrameHands checks visible, known overlays using OpenVR's
`ComputeOverlayIntersection`. When a fingertip approaches the overlay, it
tests a short ray through the fingertip along the panel's surface normal,
then sends a click with contact/release hysteresis. The helper probes known
Valve dashboard and desktop overlay keys (including `system.desktop.1`),
plus overlays seen in SteamVR events, and uses the compositor's 3D intersection; arbitrary
third-party panels still require overlay discovery or integration, so touch
is **not** claimed to work everywhere yet. If a panel's overlay key is known,
add it to `HANDS_TOUCH_OVERLAY_KEYS` in the config and run
`~/.local/bin/framehands restart`. Pinch-at-distance
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
