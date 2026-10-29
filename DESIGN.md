# SelfieCam / SelfieStick: design and verified facts

Goal (user, 2026-10-29): a selfie stick you can hold. When it's enabled from the Echo Tools
tablet, it spawns in front of you. You can grab it with one hand or both. The camera on
top always points at the top of your chest, and the **PC/desktop camera** moves to that
camera. **The headset view is unchanged.**

Hard rule (project-wide): **client-side only.** Nothing may spawn, write or send anything
that could replicate to other players. The stick is drawn by us into local GPU buffers.
It is not an engine actor, and it never touches engine physics.

Binary: `echovr.exe` sha256 `3dae0cda…df9a` (both the arena install and the EchoCraft install;
image base 0x140000000). Every RVA below is VA − 0x140000000.

## 1. The PC camera: `-capturevp2` (verified statically 2026-10-29)

- Launch flag `-capturevp2` ("Run the game in recording mode, using a second viewport").
  Parsed in the CR15Game arg parser: it sets `game+0x7ae0 |= 0x20000000` (bit 29). At setup
  (`FUN_1400ff4b0`) it then sets `game+0x23c = 1` and `game+0x2e0 = 2` (viewport count).
  `game = *(base+0x20a0478)`. **The flag is only honoured at launch.**
- Per frame, `FUN_14011f9c0` runs when bit 29 is set:
  ```
  renderer = *(base+0x20a0470)
  cam2 = *(renderer+0x2ce88)                       // viewport-2 camera
  FUN_140629c30(*(renderer+0x2ce80), &pose)        // call at 0x14011fa03, returns to 0x14011fa08
  cam2->vfn[0x68](&pose+0x10 /*pos*/); cam2->vfn[0x70](&pose /*quat*/)
  cam2+0x9c = 0.05 (near), +0xa0 = 1024 (far), +0x98 = vfov from capturefov (settings, default 112) and vp2 size renderer+0x2b498
  ```
- `FUN_140629c30(cam0, out)` writes `out[0..3]` = world quat (xyzw) and `out[4..6]` = world
  position. It computes `base(rig) * HMD`. Rig base: `idx = u16 cam0+0x78`,
  `entry = *(*(cam0+0x70)+0x28) + idx*0x30`, quat at entry+0, pos at entry+0x10. The HMD is
  applied only if `cam0+0xb0 & 1`. Composition: `pos = basePos + QRot(baseQ, hmdPos)`,
  `q = QMul(baseQ, hmdQ)`.
- **Hook:** MinHook on `0x140629c30`. Only when `_ReturnAddress() == base+0x11fa08`, call the
  original, then overwrite `out` with the selfie camera pose. Every other caller passes
  through untouched. The Frida prototype is `../selfie_cam.py` (same logic, look-at-head).
- The local HTTP `/camera_transform` drives the *spectator* camera (`R14NetSpectatorCamera`).
  It works only for spectator-team, non-VR clients, so **not usable** for us.

## 2. Poses (engine getters; all in Echo's *tracking* frame)

The getters read the copied LibOVR tracking state and convert it to engine space. They
negate x and z of positions and of quat x/z (a 180° yaw), add the playspace offset
`DAT_1420c7790` (vec3), then rotate by the recenter quat `DAT_1420223f8`.

| | position | rotation |
|---|---|---|
| head | `FUN_14072cd50(out)` | `FUN_14072cc10(out)` |
| right hand | `FUN_14072df40(out, 1)` | `FUN_14072de90(out, 1)` |
| left hand | `FUN_14072c780(out, 1)` | `FUN_14072c6d0(out, 1)` |

- Raw LibOVR copy: head pose `0x1420c78b8`, hand poses `0x1420c7918` (L) and `0x1420c7970` (R),
  stride 0x58 (`ovrPoseStatef`). Hands are live only if `DAT_1420c77a0 != 0 && DAT_1420223cc != 0`.
- Hand position is clamped to arm's reach of the head (`FUN_14072e4c0`).
- Engine conventions: **forward = +Z, up = +Y, right = −X**. The untracked right-hand fallback
  is (−0.25, 0, 0.5).
- Math helpers in the exe: `QuaternionRotateVector(out, v, q)` at `0x1400f93b0`; quat multiply
  `(out, a, b)` = a*b at `0x1400f94e0`. Use them, or match them exactly; then verify with a
  unit test against the decompiled formulas.
- **World ← tracking** = the rig base above (`QMul(baseQ, ·)` / `basePos + QRot(baseQ, ·)`).
- **Raw OVR tracking ← engine tracking:** invert the getter chain. `v_eng = R_recenter*(flip(v_ovr) + offset)`,
  so `v_ovr = flip(R_recenter⁻¹*v_eng − offset)`; `q_eng = R_recenter * flipq(q_ovr)`, so
  `q_ovr = flipq(conj(R_recenter) * q_eng)`, where `flip(x,y,z)=(−x,y,−z)` and
  `flipq(x,y,z,w)=(−x,y,−z,w)`. Unit-test it as a round trip against the getters' formulas.

## 3. Stick behaviour (core)

- **Enable** (tablet → `SelfieStick_SetEnabled(1)`): spawn the stick floating 0.5 m in front of
  the head (along head +Z), handle at chest height, shaft vertical along head-up, camera on top.
- **States:** FREE (floats in **world** space where it was left; zero-g, no physics), HELD1
  (one hand), HELD2 (both hands).
- **Grab:** a hand's grip axis rises above 0.6 (release below 0.4, hysteresis) while that hand
  is within `kGrabRadius` of the shaft segment. On grab, store stick-relative-to-hand. HELD2
  places the stick so it keeps the grab offsets of both hands. Use the midpoint, and point
  the shaft axis along the hand-to-hand line whenever the hands are more than 10 cm apart
  along the shaft; otherwise behave like HELD1 with the first hand.
- **Camera:** sits at the camera head (top). **It always looks at the top of the chest:**
  `chest = headPos + QRot(headQ, (0, −0.25, +0.05))` in world. Use a look-at quat with world
  up = the head's up vector (no roll from wrist twist). Light smoothing (default 0.3), but
  never so much that the stick and the camera visibly disagree.
- The drawn camera head uses the same look-at orientation, so the lens visibly faces you.
- **Disable:** stop overriding vp2 (stock head cam returns) and hide the stick.
- Grip input: hook `ovr_GetInputState` in `LibOVRRT64_1.dll` (as EchoCraft does in
  `MinecraftEchoVR/native/client_observer.cpp`, about line 697) and read `HandTrigger[0/1]`
  (0 = left, 1 = right). **Pass the input through unmodified.**
- Echo still sees the grip, so the player may also grab a wall. That's accepted for v1.

## 4. Rendering (render)

Adapt EchoCraft's proven path (`C:\Users\nmdur\OneDrive\Desktop\MinecraftEchoVR\native`:
`client_layer.h`, `client_projection.h`, `mesh_renderer.*`, `client_observer.cpp`, README §1):

- Hook `ovr_CommitTextureSwapChain` for Echo's colour chain. Read the EyeFovDepth layer header
  at `base+0x2022558` (136-byte header; colour chains +136, viewports +152, FOV +184, eye poses
  +216, depth chains +280, ProjectionDesc +296).
- Record on Echo's own queue into Echo's colour **and depth** (R24G8, forward Z, Echo's
  projection), so hands and avatar occlude the stick correctly. Leave resources in the
  states EchoCraft found.
- Draw a procedural mesh: handle cylinder, thin shaft, camera box, and a lens disc on the box's
  local +Z. Use flat-shaded simple colours: dark grey handle (brighter when `highlight`), silver
  shaft, black box, blue lens ring.
- The stick does not need to appear in the desktop viewport (the camera is on it).

## 5. Tablet (tablet)

Add a **Selfie Stick** ON/OFF switch to the existing Tools page of the hand tablet
(`PersonalDiscTrainer/native/runtime.cpp`, canvas patch tooling under `EchoVr-Tablet-Probing/`).
On press, `LoadLibraryW` `SelfieStick.dll` from the directory of `EchoTabletTrainer.dll`
(once), then `GetProcAddress` and call `SelfieStick_SetEnabled`. Default OFF each launch.
Missing DLL: show "SELFIE STICK: MISSING" and don't crash.

## 6. Install / test order

1. `python .\SelfieCam\selfie_cam.py --launch` (Frida prototype): proves vp2 reaches the
   desktop and the hook works. **Not yet run in game.**
2. SelfieStick.dll loaded by `SelfieCam/load_selfie.py` (Frida `LoadLibraryW` + call
   SetEnabled), no tablet needed.
3. Tablet build install.

Nothing is installed into the game folder by any agent; installs happen with the user.

## 7. v2 (2026-10-29): shutter button, screenshots, FOV and length menu

Contract v2 is in `native/selfie_shared.h`. v1 is installed in game (via `tools/deploy_selfie_stick.py`); v2 is built offline and NOT installed by any agent.

- **Shutter button** (user's choice): a red dome on the handle (`kShutterY`, +Z face). It fires when the index fingertip of a hand that is **not currently holding the stick** comes within `kShutterPress` of it. That hand must also be in the pointing pose: if Touch `Touches` data is available, the ovrTouch_*IndexPointing bit (L 0x2000 / R 0x20) must be set. Firing is on the rising edge, with a 1 s cooldown. The button glows (`shutterHot`) within `kShutterHot`. The fingertip is an estimate: controller pose plus a configurable index-tip offset in controller-local engine space (forward +Z); start at about (0, −0.02, 0.06) and justify it.
- **Feedback:** the lens flashes in the headset only (`RenderState.flash`), with a short shutter click through the default audio device (async, never blocks).
- **Screenshot:** capture the desktop window's backbuffer, which shows the `-capturevp2` view, at Echo's desktop `Present`. Copy it to a readback buffer on Echo's own queue (`selfie_render::EchoQueue()`), never wait on the render thread, and finish on a worker thread. Put it on the clipboard (CF_DIB) **and** save a PNG (WIC) to `%USERPROFILE%\Pictures\Echo Selfies\EchoSelfie_YYYYMMDD_HHMMSS.png`. EchoCraft's desktop-mirror code (`MinecraftEchoVR\native\desktop_mirror.h`, `desktop_surface.h`, `client_observer.cpp`) already hooks Echo's desktop Present; adapt it.
- **FOV:** the engine reads `capturefov` every frame from `game+0x8e2c`, a float in degrees with `game = *(base+0x20a0478)` (`FUN_14011f9c0`; MOVSS at `0x14011fa73`). Core writes it while the stick is enabled, restores the original when it's disabled, and clamps to `kFovMin..kFovMax`.
- **Length:** runtime value `kStickMinLen..kStickMaxLen`, stepped by `kStickLenStep`. The grab segment, camera head (length + 0.025) and drawn shaft all follow it. The handle and button stay fixed at the bottom.
- **Tablet:** pressing the Tools-page SELFIE STICK switch now **opens a Selfie page** with `SELFIE STICK: ON/OFF`, `FOV − [112°] +`, `LENGTH − [1.00 m] +` and `BACK`. Values are re-read from the getters after every press.
- **Self-timer (v2.1, user request):** a shutter press starts a 3 s countdown. Floating digits 3 → 2 → 1 appear about 0.12 m above the camera head and turn to face the user. Each digit pops in and fades out over its second. The shot fires when "1" ends. Presses during a countdown are ignored, and disabling the stick cancels it. The digits are drawn in the headset only, never in the photo.
