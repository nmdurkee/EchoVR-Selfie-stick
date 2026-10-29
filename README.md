# EchoVR Selfie Stick

A selfie stick for Echo VR on PC VR. You can grab it with one or both hands. The camera on top
always aims at your chest, and **the PC/desktop view (what you record or stream) comes from that
camera**. Your headset view stays first-person. Poke the red shutter button and a floating
**3, 2, 1** counts down; then the shot goes to your clipboard and is saved as a PNG.

> [!WARNING]
> **Research prototype, unofficial.** It hooks a closed-source game at runtime, using addresses verified
> against one `echovr.exe` build (sha256 `3dae0cda…df9a`, goldmaster 631547). It ships no game files.
> **Status (2026-10-29): it is built and its offline tests pass, but it has not been tested in the headset yet.**

It is **client-side only**. The stick is drawn into your own eye buffers, and nothing is spawned in the
game, so nothing reaches other players.

---

## Features

| | |
|---|---|
| **Spawn** | Tablet → Tools → **SELFIE STICK** → ON. The stick appears 0.5 m in front of you. |
| **Grab** | Squeeze grip within 12 cm anywhere along the shaft. With two hands the stick follows the line between them, so lifting either hand tilts that end. Let go and it floats in place (zero-g). |
| **PC camera** | It sits on top of the stick and **always aims at your upper chest**, whether the stick is held or floating like a tripod. |
| **Shutter** | Poke the red button on the handle with your **free** hand's index finger. The hand holding the stick can't fire it. It glows when your finger is close and has a cooldown. |
| **Self-timer** | 3, 2, 1 floating digits pop in and fade above the camera. Then the lens flashes and you hear a click. The digits and flash show only in the headset, never in the photo. |
| **Screenshot** | The desktop frame goes to the clipboard (CF_DIB) and to `<Pictures>\Echo Selfies\EchoSelfie_YYYYMMDD_HHMMSS.png`. |
| **Tablet page** | `SELFIE STICK: ON/OFF`, `FOV − [112 DEG] +` (60–130°), `LENGTH − [1.00 M] +` (0.5–2.0 m), `SHOTS TAKEN`, `< BACK`. |

## How it works

**The discovery that made it possible:** Echo VR ships an undocumented launch flag, **`-capturevp2`**
("Run the game in recording mode, using a second viewport"). In VR the desktop window is normally just
a copy of your eye. With this flag, the engine renders a **second mono viewport** to the desktop, and
its camera copies your HMD pose every frame.

```
FUN_14011f9c0 (per frame, only when game+0x7ae0 bit 29 = -capturevp2):
    cam2 = *(renderer + 0x2ce88)                        // viewport-2 camera
    FUN_140629c30(*(renderer + 0x2ce80), &pose)         // call at 0x14011fa03 -> returns to 0x14011fa08
    cam2->SetPosition(pose.pos); cam2->SetRotation(pose.quat)
    cam2 fov <- capturefov (game+0x8e2c, degrees)
```

`SelfieStick.dll` hooks `FUN_140629c30` and rewrites the pose **only for calls that return to
`0x14011fa08`**, putting the camera on the stick and aiming it at your chest. Every other caller passes
through untouched. (The HTTP `/camera_transform` endpoint drives the *spectator* camera instead, and only
for spectator-team clients not in VR, so it can't do this.)

The rest of the DLL:

- **Poses:** the engine's own getters for head and hands (`0x14072cd50/cc10`, `0x14072df40/de90`,
  `0x14072c780/c6d0`), combined with the camera rig base. Everything is in engine world space, with conversions to
  raw LibOVR tracking space for drawing.
- **Drawing the stick:** an `ovr_CommitTextureSwapChain` hook that records on **Echo's own D3D12 queue** (`*(base+0x20205a0)`,
  validated) into Echo's colour **and depth** buffers, so your hands occlude it correctly. The approach is adapted
  from [EchoCraft](https://github.com/nmdurkee)'s renderer. It's fence-gated and never stalls the game.
- **Grip and shutter input:** an observe-only `ovr_GetInputState` hook (HandTrigger and Touches); input reaches Echo unmodified.
- **Screenshot:** a hook on the desktop swapchain's `Present` (`*(*(base+0x2022690)+0x1970)`). It copies the backbuffer
  to a readback buffer on Echo's queue, and a worker thread writes the clipboard and PNG (WIC).
- **Safety:** before hooking anything, the DLL checks the code bytes at every engine address it uses against
  the known build. Every engine read is SEH-guarded. A render fault disables only drawing; the game
  keeps running.

Full reverse-engineering notes and the contract between modules are in [DESIGN.md](DESIGN.md).

## Repository layout

```
native/                 SelfieStick.dll (C++17, MSVC, MinHook)
  selfie_shared.h       contract between the modules + DLL exports
  selfie_core.cpp       pose sampling, state machine driver, exports, lazy hook install
  selfie_sim.h          pure state machine: spawn, grab (1/2 hands), shutter, self-timer, camera aim
  selfie_camera.cpp     -capturevp2 viewport-2 pose hook
  selfie_input.cpp      ovr_GetInputState observer
  selfie_render.cpp     ovr_CommitTextureSwapChain hook + Echo queue validation
  render_*.h            D3D12 stick/digit renderer, mesh, layer parsing, projection
  selfie_capture.cpp    desktop Present hook, clipboard + PNG (capture_*.h helpers)
  *_tests.cpp           unit + WARP GPU tests
  build*.cmd            builds (each one runs its tests)
tablet/                 the tablet side (EchoTabletTrainer Selfie page logic + tests), for reference
tools/selfie_cam.py     Frida prototype of the camera hook (hand cam looking at your face)
tools/load_selfie.py    Frida loader: inject SelfieStick.dll without the tablet (keys e/d/s/q)
deploy/                 install/restore scripts with exact backups (written for the author's workspace)
```

The tablet integration is part of the larger EchoTabletTrainer mod (Tools tab, canvas patching). Only
the Selfie-specific sources are mirrored here.

## Building

Windows, Visual Studio 2022+ (`vcvars64.bat`).

```bat
cd native
build.cmd            :: core tests + dist\SelfieStick.dll
build_render.cmd     :: D3D12 render tests on WARP (debug layer on)
build_capture.cmd    :: capture tests (WARP, clipboard, PNG)
```

Last run: core **7,270**, render **59,975** and capture **211** checks, 0 failures.

## Running

1. Start Echo with **`echovr.exe -capturevp2`** from its install folder. Without the flag the stick still
   works in the headset, but the PC view won't move. A player client needs a connected headset
   (`-noovr` requires `-spectatorstream`).
2. Either install via the tablet mod (`deploy/`), or inject for testing:
   `python tools/load_selfie.py --launch` (Frida 17), then press `e`.
3. `SelfieStick_Status()` (printed by `load_selfie.py` and logged by the tablet) shows hooks, poses,
   grips, FOV, countdown, shots and render/capture state on one line.

Tuning: the fingertip estimate for the shutter can be overridden without rebuilding with
`SELFIESTICK_TIP="x,y,z"` (controller-local metres, default `0,-0.02,0.07`).

## Known risks (pending the headset test)

- Your own avatar head may be hidden in viewport 2. Echo hides it in first person, and scene sets carry a per-viewport `viewportmask`.
- The fingertip offset and the index-pointing gating may need tuning.
- Sounds play on the default Windows output device.

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause, `native/vendor/minhook/LICENSE.txt`)
- Rendering inside Echo's frame is adapted from the author's EchoCraft project.
- Reverse engineering: Ghidra plus the ReVault MCP.

Echo VR is © Ready At Dawn / Meta. This project is not affiliated with either.

Built with [Claude Code](https://claude.com/claude-code)
