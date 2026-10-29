// selfie_capture.h - screenshot module of SelfieStick.dll (DESIGN.md §7).
// The public API (Install / RequestShot / ShotsTaken / Status) is declared by the contract in
// selfie_shared.h. Pieces: capture_util.h (conversion, DIB, naming, state machine),
// capture_gpu.h (readback copy on Echo's queue), capture_io.h (PNG via WIC, clipboard).
//
// How it works:
//   Install()     reads Echo's desktop swap chain (*(*(base+0x2022690)+0x1970), EchoCraft's proven
//                 pointer on this echovr.exe build), checks its vtable lives in a loaded image, and
//                 patches the IDXGISwapChain::Present vtable slot (8). Starts one worker thread.
//   RequestShot() flips an atomic Idle -> Requested. Nothing else.
//   Present hook  (Echo's render thread, desktop swap chain only, before the original Present):
//                 if Requested, records PRESENT -> COPY_SOURCE, copy to a READBACK buffer,
//                 COPY_SOURCE -> PRESENT on Echo's queue, Signal(fence). Never waits.
//   worker        waits on the fence (5 s timeout), maps + converts, puts CF_DIB on the clipboard
//                 and writes <Pictures>\Echo Selfies\EchoSelfie_YYYYMMDD_HHMMSS[_n].png, where <Pictures> is
//                 FOLDERID_Pictures (follows OneDrive redirection), else %USERPROFILE%\Pictures.
//
// selfie_shared.h forward-declares ::ID3D12CommandQueue at global scope, so EchoQueue() has one
// mangled name whatever the include order (capture_tests is built both ways to check it).
#pragma once
#include "selfie_shared.h"

namespace selfie_capture {
namespace testing {
// Test-only hooks (used by capture_tests.cpp; inert unless called).
bool InstallOn(void* swapChain);          // hook this IDXGISwapChain instead of Echo's (skips the Echo pointer refresh)
void SetOutputDir(const wchar_t* dir);    // nullptr = default <Pictures known folder>\Echo Selfies
void SetRequestTimeoutMs(unsigned ms);    // default 3000: a request with no desktop Present is dropped
bool WaitIdle(unsigned ms);               // true once no shot is pending/in flight
int  GpuFailures();                       // consecutive GPU/API failures (3 disables capture)
bool Disabled();
}  // namespace testing
}  // namespace selfie_capture
