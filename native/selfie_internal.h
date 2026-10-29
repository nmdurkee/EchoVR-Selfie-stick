// selfie_internal.h - wiring between the core pieces of SelfieStick.dll
// (selfie_core.cpp, selfie_camera.cpp, selfie_input.cpp). Not part of the render contract.
#pragma once
#include <stdint.h>

namespace selfie {
namespace internal {

// ---- echovr.exe addresses (image base 0x140000000; RVA = VA - base) ----
constexpr uintptr_t kImageBase       = 0x140000000ull;
constexpr uintptr_t kRvaGetCamPose   = 0x140629c30ull - kImageBase;   // FUN_140629c30(cam0, out)
constexpr uintptr_t kRvaVp2Ret       = 0x14011fa08ull - kImageBase;   // return address of the -capturevp2 call
constexpr uintptr_t kRvaVp2Call      = 0x14011fa03ull - kImageBase;   // the call instruction itself
constexpr uintptr_t kRvaRendererPtr  = 0x1420a0470ull - kImageBase;
constexpr uintptr_t kRendererCam0    = 0x2ce80;
constexpr uintptr_t kRvaHeadPos      = 0x14072cd50ull - kImageBase;
constexpr uintptr_t kRvaHeadRot      = 0x14072cc10ull - kImageBase;
constexpr uintptr_t kRvaRightPos     = 0x14072df40ull - kImageBase;
constexpr uintptr_t kRvaRightRot     = 0x14072de90ull - kImageBase;
constexpr uintptr_t kRvaLeftPos      = 0x14072c780ull - kImageBase;
constexpr uintptr_t kRvaLeftRot      = 0x14072c6d0ull - kImageBase;
constexpr uintptr_t kRvaTrackLive    = 0x1420c77a0ull - kImageBase;   // qword, != 0 when tracking copied
constexpr uintptr_t kRvaHeadLive     = 0x1420223c8ull - kImageBase;   // dword
constexpr uintptr_t kRvaHandsLive    = 0x1420223ccull - kImageBase;   // dword
constexpr uintptr_t kRvaRecenterQ    = 0x1420223f8ull - kImageBase;   // quat
constexpr uintptr_t kRvaPlayOffset   = 0x1420c7790ull - kImageBase;   // vec3
// v2 FOV: game = *(base + kRvaGamePtr); FUN_14011f9c0(game) tests game+0x7ae0 bit 29 (-capturevp2) and,
// after the vp2 camera-pose call, reads capturefov (float degrees) with MOVSS xmm0,[rsi+0x8e2c] at 0x14011fa73.
constexpr uintptr_t kRvaGamePtr      = 0x1420a0478ull - kImageBase;
constexpr uintptr_t kGameFlags       = 0x7ae0;     // qword, bit 29 = -capturevp2
constexpr uintptr_t kGameCaptureFov  = 0x8e2c;     // float, degrees
constexpr uintptr_t kRvaVp2FlagRead  = 0x14011f9c6ull - kImageBase;   // MOV RDX,[RCX+0x7ae0]
constexpr uintptr_t kRvaFovRead      = 0x14011fa73ull - kImageBase;   // MOVSS XMM0,[RSI+0x8e2c]

// Set when SelfieStick_SetEnabled(1) was the last call.
bool Enabled();

// core, called from the camera hook on the game thread for the vp2 call site only,
// after the original ran (every vp2 frame, enabled or not). Applies / restores the
// desktop FOV, then overwrites out[0..3] (quat) / out[4..6] (pos) when the selfie
// camera is active. Never throws; guards its own memory reads.
void OnVp2Frame(void* cam0, float* out);

// camera hook (selfie_camera.cpp)
bool CameraInstall(uintptr_t base);     // MinHook must be initialised
bool CameraInstalled();
uint64_t CameraVp2Calls();               // vp2 call-site hits
uint64_t CameraOverrides();              // frames we rewrote
uint64_t CameraFaults();
void NoteCameraOverride();               // core -> camera counter

// grip input (selfie_input.cpp)
bool InputInstall();                     // false if LibOVRRT64_1.dll is not loaded yet
bool InputInstalled();
// HandTrigger values (0 = left, 1 = right); returns false if nothing fresh (< 250 ms).
bool InputGrips(float out[2]);
// Touches bitfield (ovrInputState +12) as last seen for each hand; known[h] = fresh (< 250 ms).
void InputTouches(uint32_t touches[2], bool known[2]);
uint64_t InputCalls();
constexpr uint32_t kTouchIndexPointing[2] = {0x2000u, 0x20u};   // ovrTouch_LIndexPointing, ovrTouch_RIndexPointing

} // namespace internal
} // namespace selfie
