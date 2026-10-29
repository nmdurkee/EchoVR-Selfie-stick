// selfie_shared.h - the contract between the SelfieStick.dll pieces.  CONTRACT v2 (2026-10-29)
//   stick  (selfie_core.cpp, selfie_camera.cpp, selfie_input.cpp, selfie_sim.h,
//           selfie_render.cpp, render_*.h)                                   - "stick" agent
//   capture (selfie_capture.cpp/.h)                                          - "capture" agent
//   tablet (PersonalDiscTrainer/native/runtime.cpp + TabletMinesweeper build) - "tablet" agent, via the exports
// Do not change this file without updating every owner. See ../DESIGN.md (v2 section).
#pragma once
#include <stdint.h>
struct ID3D12CommandQueue;   // global-scope forward declaration (include order independent of <d3d12.h>)

namespace selfie {

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };          // engine + LibOVR memory order (w last)
struct Pose { Quat q; Vec3 p; };

// Stick geometry, in STICK-LOCAL space (metres). Local +Y runs from the butt of
// the handle to the camera; the camera head sits at the top. Local +Z is the
// side the camera lens faces at spawn (it is re-aimed every frame, see DESIGN).
// v2: the shaft length is ADJUSTABLE at runtime (RenderState.length); kStickLength is the default.
constexpr float kStickLength   = 1.00f;     // default butt (y=0) to top of shaft
constexpr float kStickMinLen   = 0.50f;
constexpr float kStickMaxLen   = 2.00f;
constexpr float kStickLenStep  = 0.25f;
constexpr float kHandleLength  = 0.28f;     // grippable handle y in [0, kHandleLength] (grab works on the whole shaft)
constexpr float kShaftRadius   = 0.012f;
constexpr float kHandleRadius  = 0.020f;
constexpr float kCamBox[3]     = {0.07f, 0.05f, 0.04f};   // full extents, centred at (0, length + 0.025, 0)
constexpr float kGrabRadius    = 0.12f;     // hand-to-shaft distance that counts as touching
// v2 shutter button: a red dome on the handle's +Z side (the side facing the user at spawn),
// near the top of the handle. Pressed by poking it with the OTHER hand's index fingertip.
constexpr float kShutterY      = 0.24f;     // stick-local centre (0, kShutterY, kHandleRadius)
constexpr float kShutterRadius = 0.012f;    // drawn dome radius
constexpr float kShutterPress  = 0.025f;    // fingertip-to-centre distance that fires
constexpr float kShutterHot    = 0.06f;     // fingertip-to-centre distance that lights it up
// v2 desktop FOV (horizontal degrees; the engine's capturefov setting).
constexpr float kFovMin = 60.f, kFovMax = 130.f, kFovStep = 10.f;

// What the renderer draws this frame. Written by core, read by render.
// All poses are in RAW LibOVR TRACKING SPACE (the same space as the eye poses
// in Echo's EyeFovDepth layer header), so render never needs Echo's world.
struct RenderState {
    uint32_t sequence;        // bumps on every publish
    int      visible;         // 0 = draw nothing
    Pose     stick;           // stick-local -> OVR tracking
    Pose     camHead;         // camera-head-local -> OVR tracking (lens = local +Z), for drawing the lens toward the user
    int      highlight;       // 1 = a hand is within kGrabRadius (draw handle brighter)
    // ---- v2 ----
    float    length;          // current shaft length (kStickMinLen..kStickMaxLen); camHead sits at length + 0.025
    int      shutterHot;      // 1 = a fingertip is within kShutterHot (draw the button glowing)
    float    flash;           // 0..1 lens flash intensity after a shot (headset only), fades over ~0.25 s
    // ---- v2.1: 3-second self-timer after a shutter press (headset only, never in the photo) ----
    int      countdownDigit;  // 0 = no countdown; else 3, 2, 1 (each shown for 1 s, then the shot fires)
    float    countdownAlpha;  // 0..1: each digit pops in at full opacity and fades out over its second
    Pose     countdownPose;   // digit-local -> OVR tracking: centre ~0.12 m above the camera head,
                              // billboarded so the digit faces the user's head (local +Z toward the viewer, +Y up)
};
constexpr float kCountdownSeconds = 3.0f;
constexpr float kCountdownDigitH  = 0.10f;  // digit height in metres

// core -> render. Thread-safe (seqlock inside core). Returns false if nothing published yet.
bool GetRenderState(RenderState* out);

// render -> core. Called by render at the top of its per-frame hook (render thread),
// before GetRenderState, so core can advance its state machine once per frame.
void Tick();

} // namespace selfie

// ---- render module API (implemented by selfie_render.cpp) --------------------
namespace selfie_render {
// Install the ovr_CommitTextureSwapChain hook (MinHook already initialised by core).
// From inside that hook render calls selfie::Tick() then selfie::GetRenderState().
bool Install();
void Uninstall();
const char* Status();     // one-line diagnostic for the log
// v2: Echo's validated D3D12 direct queue once render has found it (else nullptr). Not AddRef'd;
// valid for the process lifetime. Capture uses it so it never needs its own lookup.
::ID3D12CommandQueue* EchoQueue();
}

// ---- capture module API (implemented by selfie_capture.cpp) -------------------
namespace selfie_capture {
// Install the desktop-window Present hook (MinHook initialised). Called by core's install worker.
bool Install();
// Request one screenshot of the NEXT desktop frame (the -capturevp2 view). Non-blocking, any thread.
// Returns false if a shot is already in flight or capture is not installed.
bool RequestShot();
uint32_t ShotsTaken();    // completed shots (clipboard + PNG written)
const char* Status();     // one-line diagnostic (last file path or last error)
}

// ---- DLL exports (implemented by core) ---------------------------------------
// All exports are non-blocking, take no locks and never call engine functions (the tablet calls
// them on the game's input thread while holding its own lock).
extern "C" {
__declspec(dllexport) void        SelfieStick_SetEnabled(int on);  // on: spawn 0.5 m in front; off: hide + restore camera
__declspec(dllexport) int         SelfieStick_IsEnabled(void);
__declspec(dllexport) const char* SelfieStick_Status(void);       // diagnostic line
// ---- v2 ----
__declspec(dllexport) float       SelfieStick_GetFov(void);        // current desktop horizontal FOV, degrees
__declspec(dllexport) float       SelfieStick_StepFov(int dir);    // dir -1/+1: +/- kFovStep, clamped; returns the new value
__declspec(dllexport) float       SelfieStick_GetLength(void);     // metres
__declspec(dllexport) float       SelfieStick_StepLength(int dir); // dir -1/+1: +/- kStickLenStep, clamped; returns the new value
__declspec(dllexport) int         SelfieStick_ShotsTaken(void);
}
