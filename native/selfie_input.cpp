// selfie_input.cpp - observe Touch grip (HandTrigger) via a MinHook on LibOVR's
// ovr_GetInputState. The input is passed through to Echo UNMODIFIED.
//
// ovrInputState (LibOVR 1.x, as read by EchoCraft client_observer.cpp pollInput):
//   +0  double TimeInSeconds   +8 unsigned Buttons   +12 unsigned Touches
//   +16 float IndexTrigger[2]  +24 float HandTrigger[2]   +32 ovrVector2f Thumbstick[2]
//   +48 ovrControllerType ControllerType
// ovrControllerType: LTouch 0x01, RTouch 0x02, Touch 0x03, Active 0xFFFFFFFF.
// v2 also records Touches (+12; ovrTouch_LIndexPointing 0x2000, ovrTouch_RIndexPointing 0x20).
#include <windows.h>
#include <atomic>
#include <string.h>
#include <math.h>
#include "MinHook.h"
#include "selfie_internal.h"

namespace selfie {
namespace internal {
namespace {

using GetInputFn = int(__cdecl*)(void* session, unsigned type, void* state);
GetInputFn g_original = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_grip[2] = {{0}, {0}};       // float bits
std::atomic<uint64_t> g_stamp[2] = {{0}, {0}};      // GetTickCount64 of last update
std::atomic<uint64_t> g_calls{0};
std::atomic<uint32_t> g_touch[2] = {{0}, {0}};      // Touches (+12) as seen for each hand
std::atomic<uint64_t> g_touchStamp[2] = {{0}, {0}};
// The pointing requirement applies only once this hand has reported the pointing bit at least once:
// a runtime/controller that never sets capacitive bits must not make the button unpressable.
std::atomic<bool> g_everPointing[2] = {{false}, {false}};

void Observe(unsigned type, const void* state) {
    __try {
        const unsigned char* s = static_cast<const unsigned char*>(state);
        unsigned mask = type;
        if (type == 0xFFFFFFFFu) memcpy(&mask, s + 48, 4);   // Active: the struct says which
        mask &= 3u;
        if (!mask) return;
        float hand[2];
        memcpy(hand, s + 24, sizeof(hand));
        uint32_t touches;
        memcpy(&touches, s + 12, 4);
        uint64_t now = GetTickCount64();
        for (int h = 0; h < 2; h++) {
            if (!(mask & (1u << h))) continue;
            g_touch[h].store(touches, std::memory_order_relaxed);
            g_touchStamp[h].store(now, std::memory_order_release);
            if (touches & kTouchIndexPointing[h]) g_everPointing[h].store(true, std::memory_order_relaxed);
            float v = hand[h];
            if (!isfinite(v) || v < -0.01f || v > 1.01f) continue;
            uint32_t bits;
            memcpy(&bits, &v, 4);
            g_grip[h].store(bits, std::memory_order_relaxed);
            g_stamp[h].store(now, std::memory_order_release);
        }
        g_calls.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int __cdecl Detour(void* session, unsigned type, void* state) {
    int r = g_original(session, type, state);
    if (r >= 0 && state) Observe(type, state);
    return r;
}

} // namespace

bool InputInstall() {
    if (g_installed.load()) return true;
    HMODULE rt = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!rt) return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(rt, "ovr_GetInputState"));
    if (!target) return false;
    // Keep the runtime loaded while our trampoline points into it.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(target), &pinned);
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

bool InputInstalled() { return g_installed.load(); }

bool InputGrips(float out[2]) {
    uint64_t now = GetTickCount64();
    bool any = false;
    for (int h = 0; h < 2; h++) {
        uint64_t st = g_stamp[h].load(std::memory_order_acquire);
        if (st && now - st < 250) {
            uint32_t bits = g_grip[h].load(std::memory_order_relaxed);
            memcpy(&out[h], &bits, 4);
            any = true;
        } else {
            out[h] = 0.0f;
        }
    }
    return any;
}

void InputTouches(uint32_t touches[2], bool known[2]) {
    uint64_t now = GetTickCount64();
    for (int h = 0; h < 2; h++) {
        uint64_t st = g_touchStamp[h].load(std::memory_order_acquire);
        known[h] = st && now - st < 250 && g_everPointing[h].load(std::memory_order_relaxed);
        touches[h] = known[h] ? g_touch[h].load(std::memory_order_relaxed) : 0u;
    }
}

uint64_t InputCalls() { return g_calls.load(); }

} // namespace internal
} // namespace selfie
