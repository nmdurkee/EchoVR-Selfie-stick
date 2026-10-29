// selfie_camera.cpp - MinHook on FUN_140629c30(cam0, out) ("camera world pose = rig * HMD").
// Only the call from the -capturevp2 viewport-2 update (returns to base+0x11fa08) is
// touched; every other caller passes straight through. See ../DESIGN.md section 1.
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include "MinHook.h"
#include "selfie_internal.h"

#pragma intrinsic(_ReturnAddress)

namespace selfie {
namespace internal {
namespace {

using GetCamPoseFn = float*(__fastcall*)(void* cam0, float* out);
GetCamPoseFn g_original = nullptr;
uintptr_t g_vp2Ret = 0;
std::atomic<bool> g_installed{false};
std::atomic<uint64_t> g_vp2Calls{0}, g_overrides{0}, g_faults{0};

// Kept free of C++ objects so __try is allowed.
void GuardedOverride(void* cam0, float* out) {
    __try {
        OnVp2Frame(cam0, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

float* __fastcall Detour(void* cam0, float* out) {
    void* ret = _ReturnAddress();
    float* r = g_original(cam0, out);
    if (reinterpret_cast<uintptr_t>(ret) == g_vp2Ret && out) {
        g_vp2Calls.fetch_add(1, std::memory_order_relaxed);
        GuardedOverride(cam0, out);   // core checks Enabled() itself (it also restores the FOV after a disable)
    }
    return r;
}

} // namespace

void NoteCameraOverride() { g_overrides.fetch_add(1, std::memory_order_relaxed); }

bool CameraInstall(uintptr_t base) {
    if (g_installed.load()) return true;
    void* target = reinterpret_cast<void*>(base + kRvaGetCamPose);
    g_vp2Ret = base + kRvaVp2Ret;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original)) != MH_OK)
        return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_original = nullptr;
        return false;
    }
    g_installed = true;
    return true;
}

bool CameraInstalled() { return g_installed.load(); }
uint64_t CameraVp2Calls() { return g_vp2Calls.load(); }
uint64_t CameraOverrides() { return g_overrides.load(); }
uint64_t CameraFaults() { return g_faults.load(); }

} // namespace internal
} // namespace selfie
