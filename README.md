> [!CAUTION]
> ## Experimental prototype - expect missing core features
>
> This is **not** a finished mod.
>
> Current builds may only test whether head tracking can drive the camera. Bug fixes and core features like decoupled look/aim, independent reticle behavior, correct shot direction, off-screen reticle support, movement handling, and comfort tuning may be missing at this early stage of development.

# Resident Evil 3 Head Tracking

Head tracking for Resident Evil 3 Remake that decouples looking from aiming, so your head moves the camera view through OpenTrack while the mouse or controller still aims independently, with no VR headset required.

<!-- ![Mod GIF](https://raw.githubusercontent.com/itsloopyo/resident-evil-3-headtracking/main/assets/readme-clip.gif) -->

## Features

- **Decoupled look and aim** - head tracking moves the camera; aim stays on your mouse/controller
- **6DOF positional tracking** - lean and peek with head position

## Requirements

- [Resident Evil 3 Remake](https://store.steampowered.com/app/952060/Resident_Evil_3/) (Steam)
- [OpenTrack](https://github.com/opentrack/opentrack) or a compatible head tracking app (smartphone, webcam, or dedicated hardware)
- Windows 10/11 (64-bit)

## Installation

1. Download the latest release from the [Releases page](https://github.com/itsloopyo/resident-evil-3-headtracking/releases)
2. Extract the ZIP anywhere
3. Double-click `install.cmd`
4. The installer auto-detects your game and installs REFramework if needed
5. Configure OpenTrack to output UDP to `127.0.0.1:4242`
6. Launch the game. Head tracking is enabled automatically.

The installer automatically finds your game via Steam registry lookup. If it can't find the game:
- Set the `RE3_PATH` environment variable to your game folder, or
- Run from command prompt: `install.cmd "D:\Games\RE3"`

### Manual Installation

1. Install [REFramework](https://github.com/praydog/REFramework-nightly/releases) for RE3 (extract to game root)
2. Copy `RE3HeadTracking.dll` and `HeadTracking.ini` to `<game>/reframework/plugins/`

## Setting Up OpenTrack

1. Download and install [OpenTrack](https://github.com/opentrack/opentrack/releases)
2. Configure your tracker as input
3. Set output to **UDP over network**
4. Host: `127.0.0.1`, Port: `4242`
5. Start tracking before launching the game

### VR Headset Setup

A VR headset makes an excellent tracker for flat games.

1. Connect your headset to the PC via Air Link (Quest) or Virtual Desktop
2. Launch SteamVR
3. In OpenTrack, set the input to **SteamVR**
4. Set output to **UDP over network** (`127.0.0.1:4242`)
5. Start tracking before launching the game

### Webcam Setup

No special hardware needed. OpenTrack's built-in **neuralnet tracker** uses any webcam for 6DOF face tracking.

1. In OpenTrack, set the input to **neuralnet tracker**
2. Select your webcam in the tracker settings
3. Set output to **UDP over network** (`127.0.0.1:4242`)
4. Start tracking before launching the game
5. Centre your head and use OpenTrack's own Center hotkey. The mod keeps no centre of its own, it applies whatever pose the tracker sends

### Phone App Setup

This mod smooths remote connections by default (`RemoteSmoothing=0.15`), so you can send directly from your phone on port 4242 without needing OpenTrack on PC.

1. Install an OpenTrack-compatible head tracking app
2. Configure it to send to your PC's IP on port 4242 (run `ipconfig` to find it)
3. Set the protocol to OpenTrack/UDP

**With OpenTrack (optional):** If you want curve mapping or visual preview, route through OpenTrack. Set OpenTrack's input to "UDP over network" on a different port (e.g. 5252), point your phone app at that port, and set OpenTrack's output to `127.0.0.1:4242`. Make sure your firewall allows incoming UDP on the input port.

## Controls

Two equivalent binding sets - use whichever your keyboard has:

| Action              | Nav-cluster | Chord           |
|---------------------|-------------|-----------------|
| Toggle tracking     | `End`       | `Ctrl+Shift+Y`  |
| Cycle tracking mode | `Page Up`   | `Ctrl+Shift+G`  |
| Toggle yaw mode     | `Page Down` | `Ctrl+Shift+H`  |
| Toggle reticle      | `Insert`    | `Ctrl+Shift+U`  |

`Page Up` / `Ctrl+Shift+G` cycles tracking mode:

1. Normal head-tracked gameplay
2. Positional tracking disabled, rotational tracking enabled
3. Rotational tracking disabled, positional tracking enabled
4. Back to normal

`Toggle yaw mode` switches between world-space (horizon-locked) and camera-local yaw.

## Configuration

The mod creates a config file at `reframework/plugins/HeadTracking.ini` on first run. Edit it to customize:

A comment has to sit on its own line, above the key. The parser hands the whole
text after `=` to the value reader. For a `true`/`false` or text setting that
text is compared as a whole, so a trailing `; note` makes the comparison fail
and the setting silently keeps its default. Numeric settings survive a trailing
comment because the number is read off the front of the text, which is why some
lines below still carry one. Putting every comment on its own line always works.

```ini
[Network]
UDPPort=4242                    ; Must match OpenTrack output port

[Sensitivity]
YawMultiplier=1.0               ; Horizontal rotation (0.1-5.0)
PitchMultiplier=1.0             ; Vertical rotation (0.1-5.0)
RollMultiplier=1.0              ; Head tilt (0.0-2.0)

[Smoothing]
LocalSmoothing=0.0              ; Tracker on this machine, loopback (0.0-1.0)
RemoteSmoothing=0.15            ; Tracker is a remote network device (0.0-1.0)

[Position]
SensitivityX=2.0                ; Lateral sensitivity (0.1-10.0)
SensitivityY=2.0                ; Vertical sensitivity (0.1-10.0)
SensitivityZ=2.0                ; Depth sensitivity (0.1-10.0)
LimitX=0.30                     ; Max lateral offset in meters
LimitY=0.20                     ; Max vertical offset in meters
LimitZ=0.40                     ; Max forward offset in meters
LimitZBack=0.10                 ; Max backward offset (prevents camera clipping)
; Invert lateral axis
InvertX=false
; Invert vertical axis
InvertY=false
; Invert depth axis
InvertZ=false
; Enable/disable 6DOF position tracking
Enabled=true

[Hotkeys]
; Virtual key codes (hex)
ToggleKey=0x23                  ; End - Enable/disable
PositionToggleKey=0x21          ; Page Up - Cycle tracking mode
ReticleToggleKey=0x2D           ; Insert - Toggle reticle
YawModeKey=0x22                 ; Page Down - Toggle world/local yaw

[Reticle]
; Show the head tracking reticle overlay
Enabled=true

[General]
; Auto-enable tracking on game start
AutoEnable=true
; true = horizon-locked yaw (default), false = camera-local
WorldSpaceYaw=true
```

Delete the file to reset to defaults.

## Troubleshooting

**Sending a log:**
- REFramework writes one log per game launch at `<game>/re2_framework_log.txt`. That generic name is used for every RE Engine title, so it is the right file for this game too. If the game folder is not writable it lands in `%APPDATA%\REFramework\<exe name>\` instead.
- The file is truncated on every launch, so it only ever holds the current session. Attach it as-is to a bug report.
- This mod's lines are prefixed `[RE3HT]`. The startup sequence to look for is: `Plugin loaded`, `Config loaded from ...`, `UDP receiver started on port ...`, `Initialization complete`, then `First tracker pose received: ...` once the tracker sends anything.

**Mod not loading:**
- Ensure REFramework is installed (`dinput8.dll` in game root)
- Check `reframework/` folder exists with `plugins/RE3HeadTracking.dll` inside
- Try running the game as administrator once

**No tracking response:**
- Verify OpenTrack is running and outputting data
- Check UDP port matches (default 4242)
- Press **End** to enable tracking
- If the view sits off to one side, centre it in your tracker app. The mod has no centre of its own
- Check firewall isn't blocking UDP port 4242

**Jitter:**
- Increase `RemoteSmoothing` (phone or other network tracker) or `LocalSmoothing` (tracker on this PC) in the `[Smoothing]` section of HeadTracking.ini
- If using a phone app over WiFi, some jitter is expected. The built-in interpolation helps.

**Wrong rotation axis:**
- Adjust sensitivity multipliers or use the Invert settings in the Position section

**Yaw feels wrong when looking up or down at extreme angles:**
- Try toggling between world-locked and camera-local yaw with `Page Down`. World-locked (default) is horizon-stable; camera-local follows the camera's current up-axis.

## Updating

Download the new release and run `install.cmd` again. Your config is preserved.

## Uninstalling

Run `uninstall.cmd` from the release folder. This removes the mod DLLs. REFramework is only removed if it was originally installed by this mod. To force-remove REFramework:

```
uninstall.cmd /force
```

## Building from Source

### Prerequisites

- [CMake](https://cmake.org/) 3.20+
- [Visual Studio 2022](https://visualstudio.microsoft.com/) with C++ desktop workload
- [pixi](https://pixi.sh) task runner
- Resident Evil 3 Remake installed (for deployment only)

### Build

```bash
git clone --recurse-submodules https://github.com/itsloopyo/resident-evil-3-headtracking.git
cd resident-evil-3-headtracking

# Build and deploy to game (release)
pixi run install

# Build only (debug)
pixi run build

# Package for release
pixi run package
```

## Community & Support

- Discord: [Loop's Head Tracking Hangout](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch for the released head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your iPhone or Android phone into the head tracker

## License

MIT License - see [LICENSE](LICENSE) for details.

## Credits

- [Capcom](https://www.capcom.com/) - Resident Evil 3 Remake
- [praydog](https://github.com/praydog/REFramework) - REFramework
- [OpenTrack](https://github.com/opentrack/opentrack) - Head tracking software
- [CameraUnlock](https://github.com/itsloopyo/cameraunlock-core) - Shared head tracking library

## Disclaimer

This mod is not affiliated with, endorsed by, or supported by Capcom. Use at your own risk.
