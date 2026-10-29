// selfie_capture.cpp - SelfieStick screenshot module: grabs Echo's next desktop frame (the
// -capturevp2 selfie view) at the desktop swap chain's Present, then puts it on the clipboard
// (CF_DIB) and writes a PNG. See selfie_capture.h for the overview and DESIGN.md §7.
//
// Threads:
//   any thread    RequestShot(): atomic Idle -> Requested only.
//   render thread Present hook (desktop swap chain only): records + submits one copy on Echo's queue,
//                 Signal(fence). Never waits, never allocates more than the lazily (re)sized
//                 readback buffer, SEH-guarded; any failure abandons that shot and Present proceeds.
//   worker        fence wait (5 s), map/convert, clipboard, PNG, status. Owns a message-only
//                 window (clipboard owner) and pumps its messages.
// Capture disables itself only after kMaxGpuFailures consecutive GPU/API failures.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "selfie_capture.h"
#include "capture_gpu.h"
#include "capture_io.h"
#include <cstdio>
#include <new>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

namespace selfie_capture {
namespace {

using Microsoft::WRL::ComPtr;
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

// ---- echovr.exe (sha256 3dae0cda...df9a) desktop swap chain, from EchoCraft client_observer.cpp ----
constexpr uintptr_t kDesktopWrapperRva = 0x2022690;   // *(base+rva) = desktop presenter
constexpr uintptr_t kDesktopSwapOffset = 0x1970;      // *(presenter+off) = IDXGISwapChain*
constexpr int kPresentSlot = 8;                       // IDXGISwapChain::Present
constexpr int kMaxGpuFailures = 3;
constexpr DWORD kFenceTimeoutMs = 5000;
constexpr int kMaxVtables = 4;

struct PatchedVtable { void** vtable; PresentFn original; };
PatchedVtable g_patched[kMaxVtables] = {};
std::atomic<int> g_patchedCount{0};

SRWLOCK g_installLock = SRWLOCK_INIT;
SRWLOCK g_textLock = SRWLOCK_INIT;
std::atomic<uintptr_t> g_desktopSwap{0};
std::atomic<bool> g_installed{false}, g_disabled{false}, g_testMode{false}, g_workerStarted{false};
std::atomic<int> g_gpuFailures{0};
std::atomic<uint32_t> g_shots{0};
std::atomic<unsigned long long> g_presents{0}, g_requests{0};
std::atomic<unsigned> g_requestTimeoutMs{3000};
std::atomic<const char*> g_reason{"not-installed"};
capture::ShotState g_shot;
capture::ReadbackCopier* g_copier = nullptr;     // never deleted (the GPU may still reference its objects)
HANDLE g_wake = nullptr, g_fenceEvent = nullptr;
wchar_t g_outDir[MAX_PATH] = {};                 // empty = default (g_textLock)
char g_lastPath[MAX_PATH * 3] = {};              // UTF-8 (g_textLock)
thread_local bool t_inside = false;

// ---- memory validation (no dereference of unverified pointers) -------------------------------
uintptr_t readPointer(uintptr_t address) {
    uintptr_t value = 0;
    SIZE_T read = 0;
    if (!address) return 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(value), &read) &&
                   read == sizeof(value) ? value : 0;
}
bool imageAddress(uintptr_t a, bool executable) {
    if (!a) return false;
    MEMORY_BASIC_INFORMATION m = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(a), &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || m.Type != MEM_IMAGE || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD p = m.Protect & 0xff;
    if (executable) return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    return p != 0;
}
bool pinModuleAt(const void* address) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              static_cast<LPCWSTR>(address), &m) != FALSE;
}

void noteGpuFailure(const char* why) {
    g_reason = why;
    if (g_gpuFailures.fetch_add(1) + 1 >= kMaxGpuFailures) g_disabled = true;
}

// ---- Present hook --------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE onPresent(IDXGISwapChain* swap, UINT interval, UINT flags);

PresentFn originalFor(IDXGISwapChain* swap) {
    const int n = g_patchedCount.load(std::memory_order_acquire);
    void** vtable = swap ? *reinterpret_cast<void***>(swap) : nullptr;  // `swap` is the live object being presented
    for (int i = 0; i < n; i++)
        if (g_patched[i].vtable == vtable) return g_patched[i].original;
    return n ? g_patched[0].original : nullptr;  // someone copied our patched vtable: same chain
}

// Render thread. C++ part; may throw (caught) or fault (SEH-caught in guardedShot).
void shotCpp(IDXGISwapChain* swap) {
    if (!g_shot.BeginRecord()) return;
    auto abandon = [](const char* why, bool gpu) {
        if (gpu) noteGpuFailure(why); else g_reason = why;
        g_shot.AbortRecord();
    };
    if (g_disabled.load()) return abandon("disabled-after-gpu-failures", false);
    ID3D12CommandQueue* echoQueue = selfie_render::EchoQueue();
    if (!echoQueue) return abandon("echo-queue-unknown (render has not validated it yet)", false);
    ComPtr<IDXGISwapChain3> chain;
    if (FAILED(swap->QueryInterface(IID_PPV_ARGS(&chain))) || !chain) return abandon("desktop-swapchain-not-dxgi1.4", false);
    ComPtr<ID3D12Resource> backbuffer;
    if (FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))) || !backbuffer)
        return abandon("backbuffer-unavailable", false);
    // The copy must go on the queue this swap chain presents on: same-queue ordering is our only sync
    // (Echo's rendering is before us, its Present after us). DXGI does not expose a D3D12 swap chain's
    // queue (GetDevice returns the device; verified in capture_tests), so, like EchoCraft's production
    // desktop draw, we rely on Echo presenting on its main direct queue (base+0x20205a0 = EchoQueue()).
    // What we can verify: the backbuffer lives on that queue's device (ReadbackCopier::Record checks it).
    switch (g_copier->Record(echoQueue, backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT)) {
    case capture::ReadbackCopier::Submitted:
        g_shot.Submitted();
        g_reason = "copy-submitted";
        SetEvent(g_wake);
        return;
    case capture::ReadbackCopier::Busy: return abandon("gpu-busy-previous-copy", false);
    case capture::ReadbackCopier::Unsupported: return abandon(g_copier->error ? g_copier->error : "unsupported", false);
    default: return abandon(g_copier->error ? g_copier->error : "record-failed", true);
    }
}
void shotGuardedCpp(IDXGISwapChain* swap) {
    try {
        shotCpp(swap);
    } catch (...) {
        noteGpuFailure("exception-in-present-hook");
        g_shot.AbortRecord();
    }
}
// SEH last resort; no C++ objects with destructors in this frame.
void guardedShot(IDXGISwapChain* swap) {
    __try {
        shotGuardedCpp(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteGpuFailure("seh-exception-in-present-hook");
        g_shot.AbortRecord();
    }
}

HRESULT STDMETHODCALLTYPE onPresent(IDXGISwapChain* swap, UINT interval, UINT flags) {
    const PresentFn original = originalFor(swap);
    if (swap && reinterpret_cast<uintptr_t>(swap) == g_desktopSwap.load(std::memory_order_relaxed)) {
        g_presents.fetch_add(1, std::memory_order_relaxed);
        if (!(flags & DXGI_PRESENT_TEST) && !t_inside && g_shot.Get() == capture::ShotState::Requested && g_copier) {
            t_inside = true;
            guardedShot(swap);
            t_inside = false;
        }
    }
    return original ? original(swap, interval, flags) : DXGI_ERROR_INVALID_CALL;
}

// Patches slot 8 of `vtable` (once per distinct vtable). Caller holds g_installLock.
bool patchVtable(void** vtable, const char** why) {
    const int n = g_patchedCount.load();
    for (int i = 0; i < n; i++)
        if (g_patched[i].vtable == vtable) return true;
    if (n >= kMaxVtables) { *why = "too-many-swapchain-vtables"; return false; }
    void** slot = vtable + kPresentSlot;
    void* current = reinterpret_cast<void*>(readPointer(reinterpret_cast<uintptr_t>(slot)));
    if (current == reinterpret_cast<void*>(&onPresent)) { *why = "present-slot-already-ours"; return false; }
    if (!imageAddress(reinterpret_cast<uintptr_t>(current), true)) { *why = "present-slot-not-code"; return false; }
    // Keep the original Present's module (and ours) mapped forever: the slot must never dangle.
    if (!pinModuleAt(current) || !pinModuleAt(reinterpret_cast<const void*>(&onPresent))) { *why = "pin-failed"; return false; }
    g_patched[n].vtable = vtable;
    g_patched[n].original = reinterpret_cast<PresentFn>(current);
    g_patchedCount.store(n + 1, std::memory_order_release);   // published before the slot points at us
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
        g_patchedCount.store(n);
        *why = "vtable-protect-failed";
        return false;
    }
    void* previous = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&onPresent), current);
    DWORD unused = 0;
    VirtualProtect(slot, sizeof(void*), protection, &unused);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    if (previous != current) { g_patchedCount.store(n); *why = "present-slot-changed-under-us"; return false; }
    return true;
}

// Validates a swap chain pointer read from Echo's memory without calling it: aligned, readable,
// vtable inside a loaded image, Present slot is executable image code.
bool validSwapPointer(uintptr_t swap, void*** vtableOut, const char** why) {
    if (!swap || (swap & 7)) { *why = "desktop-swapchain-not-created-yet"; return false; }
    const uintptr_t vtable = readPointer(swap);
    if (!vtable || (vtable & 7) || !imageAddress(vtable, false)) { *why = "desktop-swapchain-vtable-not-in-image"; return false; }
    for (int i = 0; i <= kPresentSlot; i++)
        if (!imageAddress(readPointer(vtable + i * sizeof(void*)), true)) { *why = "desktop-swapchain-vtable-entry-not-code"; return false; }
    *vtableOut = reinterpret_cast<void**>(vtable);
    return true;
}

uintptr_t echoDesktopSwap() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t presenter = readPointer(base + kDesktopWrapperRva);
    return presenter ? readPointer(presenter + kDesktopSwapOffset) : 0;
}

// ---- worker --------------------------------------------------------------------------------
LRESULT CALLBACK clipWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd, msg, wp, lp); }

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
// Waits for `h` while dispatching this thread's messages (clipboard owner messages are sent to us).
bool waitPumping(HANDLE h, DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= deadline ? 0 : DWORD(deadline - now);
        const DWORD r = MsgWaitForMultipleObjectsEx(1, &h, left, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) return true;
        if (r == WAIT_OBJECT_0 + 1) { pump(); if (left == 0) return WaitForSingleObject(h, 0) == WAIT_OBJECT_0; continue; }
        return false;
    }
}

void setLastPath(const std::wstring& path) {
    char utf8[sizeof(g_lastPath)] = {};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, int(sizeof(utf8)) - 1, nullptr, nullptr);
    AcquireSRWLockExclusive(&g_textLock);
    std::memcpy(g_lastPath, utf8, sizeof(g_lastPath));
    ReleaseSRWLockExclusive(&g_textLock);
}
std::wstring outputDir() {
    AcquireSRWLockShared(&g_textLock);
    std::wstring dir = g_outDir;
    ReleaseSRWLockShared(&g_textLock);
    return dir.empty() ? capture::DefaultShotDir() : dir;
}

void finishShot(HWND clipOwner) {
    ID3D12Fence* fence = g_copier->Fence();
    const UINT64 serial = g_copier->Serial();
    if (!fence) { g_reason = "no-fence"; g_shot.AbortInFlight(); return; }
    bool done = fence->GetCompletedValue() >= serial;
    if (!done) ResetEvent(g_fenceEvent);  // drop a stale signal from an earlier timed-out shot
    if (!done && SUCCEEDED(fence->SetEventOnCompletion(serial, g_fenceEvent))) done = waitPumping(g_fenceEvent, kFenceTimeoutMs);
    if (!done) done = fence->GetCompletedValue() >= serial;
    if (!done) {
        // The copier stays !Idle() until the GPU catches up, so its buffer is never reused early.
        noteGpuFailure("gpu-copy-timeout");
        g_shot.AbortInFlight();
        return;
    }
    if (!g_shot.BeginProcess()) return;
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    if (!g_copier->ReadPixels(bgra, w, h)) {
        noteGpuFailure(g_copier->error ? g_copier->error : "readback-failed");
        g_shot.Finish();
        return;
    }
    g_gpuFailures = 0;  // a full GPU round trip worked
    const char* clipWhy = nullptr;
    std::vector<uint8_t> dib;
    const bool clipped = capture::BuildDib24(bgra.data(), w, h, dib) && capture::PutDibOnClipboard(clipOwner, dib, &clipWhy);
    if (!clipped && !clipWhy) clipWhy = "dib-build-failed";
    dib.clear(); dib.shrink_to_fit();

    const char* pngWhy = nullptr;
    std::wstring dir = outputDir(), path;
    SYSTEMTIME now;
    GetLocalTime(&now);
    bool saved = false;
    if (!capture::EnsureDirectory(dir)) pngWhy = "cannot-create-output-folder";
    else if (!capture::ReserveShotPath(dir, now, path)) pngWhy = "cannot-reserve-file-name";
    else if (!(saved = capture::WritePng(path, bgra.data(), w, h, &pngWhy))) DeleteFileW(path.c_str());

    if (saved) {
        setLastPath(path);
        g_shots.fetch_add(1);
        g_reason = clipped ? "saved+clipboard" : "saved-png-only (clipboard failed)";
    } else {
        g_reason = clipped ? "clipboard-only (png failed)" : "shot-failed (png and clipboard)";
        if (pngWhy) {
            AcquireSRWLockExclusive(&g_textLock);
            std::snprintf(g_lastPath, sizeof(g_lastPath), "png error: %s%s%s", pngWhy, clipWhy ? ", clipboard: " : "", clipWhy ? clipWhy : "");
            ReleaseSRWLockExclusive(&g_textLock);
        }
    }
    g_shot.Finish();
}

DWORD WINAPI workerMain(void*) {
    // MTA for WIC; never uninitialised (the worker lives for the process lifetime; the module is pinned).
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) g_reason = "worker-com-init-failed";
    HINSTANCE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&workerMain), &self);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = clipWindowProc;
    wc.hInstance = self;
    wc.lpszClassName = L"SelfieStickCaptureClipboard";
    RegisterClassW(&wc);
    HWND clipOwner = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, self, nullptr);
    ULONGLONG lastRefresh = 0;
    for (;;) {
        waitPumping(g_wake, 250);
        const ULONGLONG now = GetTickCount64();
        if (g_shot.Expire(now, g_requestTimeoutMs.load())) g_reason = "no-desktop-present (request dropped)";
        if (!g_testMode.load() && now - lastRefresh >= 1000) {
            // Follow Echo if it ever recreates its desktop swap chain (identity compare only in the hook).
            lastRefresh = now;
            const uintptr_t swap = echoDesktopSwap();
            void** vtable = nullptr;
            const char* why = nullptr;
            if (swap && swap != g_desktopSwap.load() && validSwapPointer(swap, &vtable, &why)) {
                AcquireSRWLockExclusive(&g_installLock);
                if (patchVtable(vtable, &why)) g_desktopSwap = swap;
                ReleaseSRWLockExclusive(&g_installLock);
            }
        }
        if (g_shot.Get() == capture::ShotState::InFlight) {
            try { finishShot(clipOwner); } catch (...) { g_reason = "worker-exception"; g_shot.Finish(); }
        }
    }
}

bool startWorker(const char** why) {
    if (g_workerStarted.load()) return true;
    if (!g_copier) g_copier = new (std::nothrow) capture::ReadbackCopier();
    if (!g_copier) { *why = "out-of-memory"; return false; }
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake || !g_fenceEvent) { *why = "event-create-failed"; return false; }
    if (!pinModuleAt(reinterpret_cast<const void*>(&workerMain))) { *why = "pin-failed"; return false; }
    HANDLE t = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
    if (!t) { *why = "worker-create-failed"; return false; }
    CloseHandle(t);
    g_workerStarted = true;
    return true;
}

bool installOn(uintptr_t swap) {
    const char* why = nullptr;
    void** vtable = nullptr;
    if (!validSwapPointer(swap, &vtable, &why) || !startWorker(&why) || !patchVtable(vtable, &why)) {
        g_reason = why ? why : "install-failed";
        return false;
    }
    g_desktopSwap = swap;
    g_installed = true;
    g_reason = "installed-ready";
    return true;
}

}  // namespace

bool Install() {
    if (g_installed.load()) return true;
    AcquireSRWLockExclusive(&g_installLock);
    bool ok = g_installed.load();
    if (!ok) {
        if (!GetModuleHandleW(L"dxgi.dll")) g_reason = "dxgi-not-loaded";
        else if (!GetModuleHandleW(L"d3d12.dll")) g_reason = "d3d12-not-loaded";
        else ok = installOn(echoDesktopSwap());
    }
    ReleaseSRWLockExclusive(&g_installLock);
    return ok;
}

bool RequestShot() {
    if (!g_installed.load() || g_disabled.load()) return false;
    if (!g_shot.Request(GetTickCount64())) return false;
    g_requests++;
    g_reason = "shot-requested";
    return true;
}

uint32_t ShotsTaken() { return g_shots.load(); }

const char* Status() {
    thread_local char text[512];
    char last[sizeof(g_lastPath)];
    AcquireSRWLockShared(&g_textLock);
    std::memcpy(last, g_lastPath, sizeof(last));
    ReleaseSRWLockShared(&g_textLock);
    last[sizeof(last) - 1] = 0;
    std::snprintf(text, sizeof(text), "capture: %s%s installed=%d shots=%u requests=%llu desktopPresents=%llu gpuFailures=%d last=%s",
                  g_reason.load(), g_disabled.load() ? " (DISABLED)" : "", g_installed.load() ? 1 : 0, g_shots.load(),
                  g_requests.load(), g_presents.load(), g_gpuFailures.load(), last[0] ? last : "-");
    return text;
}

namespace testing {
bool InstallOn(void* swapChain) {
    AcquireSRWLockExclusive(&g_installLock);
    g_testMode = true;
    const bool ok = installOn(reinterpret_cast<uintptr_t>(swapChain));
    ReleaseSRWLockExclusive(&g_installLock);
    return ok;
}
void SetOutputDir(const wchar_t* dir) {
    AcquireSRWLockExclusive(&g_textLock);
    wcsncpy_s(g_outDir, dir ? dir : L"", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_textLock);
}
void SetRequestTimeoutMs(unsigned ms) { g_requestTimeoutMs = ms; }
bool WaitIdle(unsigned ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    while (g_shot.Get() != capture::ShotState::Idle) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(5);
    }
    return true;
}
int GpuFailures() { return g_gpuFailures.load(); }
bool Disabled() { return g_disabled.load(); }
}  // namespace testing

}  // namespace selfie_capture
