// selfie_render.cpp - ovr_CommitTextureSwapChain hook: draws the selfie stick into Echo's own
// colour + depth eye buffers on Echo's own D3D12 queue. Nothing is spawned in the engine.
//
// Per Echo frame (render thread, inside Echo's commit of its colour chain):
//   1. read Echo's EyeFovDepth layer header (base+0x2022558) with ReadProcessMemory and validate it;
//      ignore commits of any other chain (Echo's depth chain, other mods' layers);
//   2. selfie::Tick(); selfie::GetRenderState();  nothing visible -> pass through;
//   3. fetch the current colour + depth buffers from LibOVR;
//   4. (first time) take Echo's queue from base+0x20205a0, validate it, build the renderer;
//   5. record + submit on Echo's queue (fence-gated, never waits), then call the original commit.
// Any unexpected value bails silently for that frame; any GPU/API failure disables rendering
// for the rest of the session (the game keeps running untouched).
#include "selfie_render.h"
#include "render_pass.h"
#include "vendor/minhook/include/MinHook.h"
#include <atomic>
#include <cstdio>
#include <new>

namespace selfie_render {
namespace detail {

namespace {
bool imageAddress(uintptr_t a, bool executable) {
    if (!a) return false;
    MEMORY_BASIC_INFORMATION m = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(a), &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || m.Type != MEM_IMAGE || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD p = m.Protect & 0xff;
    if (executable) return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    return p != 0;
}
bool readableData(uintptr_t a, size_t bytes) {
    MEMORY_BASIC_INFORMATION m = {};
    if (!a || !VirtualQuery(reinterpret_cast<const void*>(a), &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & 0xff)) return false;
    return a + bytes <= reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
}
// No C++ objects with destructors in this function (SEH frame).
HRESULT sehQueryInterface(IUnknown* object, REFIID iid, void** out) {
    __try {
        return object->QueryInterface(iid, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return E_FAIL;
    }
}
}  // namespace

bool ValidateQueue(uintptr_t candidate, ID3D12Device* expected, ID3D12CommandQueue** out, const char** why) {
    auto fail = [&](const char* reason) { if (why) *why = reason; return false; };
    if (!out) return fail("bad-args");
    *out = nullptr;
    if (!candidate || (candidate & 7)) return fail("queue-null-or-misaligned");
    if (!readableData(candidate, sizeof(void*))) return fail("queue-unreadable");
    const uintptr_t vtable = render_layer::readPointer(candidate);
    if (!(vtable & ~uintptr_t(7)) || (vtable & 7) || !imageAddress(vtable, false) || !readableData(vtable, 3 * sizeof(void*)))
        return fail("queue-vtable-not-in-image");
    for (int i = 0; i < 3; i++)  // QueryInterface, AddRef, Release
        if (!imageAddress(render_layer::readPointer(vtable + i * sizeof(void*)), true)) return fail("queue-vtable-entry-not-code");
    void* raw = nullptr;
    if (FAILED(sehQueryInterface(reinterpret_cast<IUnknown*>(candidate), __uuidof(ID3D12CommandQueue), &raw)) || !raw)
        return fail("queue-qi-failed");
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    queue.Attach(static_cast<ID3D12CommandQueue*>(raw));
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return fail("queue-not-direct");
    if (expected) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<IUnknown> a, b;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return fail("queue-no-device");
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&a))) || FAILED(expected->QueryInterface(IID_PPV_ARGS(&b))) || a.Get() != b.Get())
            return fail("queue-device-mismatch");
    }
    *out = queue.Detach();
    if (why) *why = nullptr;
    return true;
}

}  // namespace detail

namespace {
using Commit = int(__cdecl*)(void*, void*);
using CurrentIndex = int(__cdecl*)(void*, void*, int*);
using GetBuffer = int(__cdecl*)(void*, void*, int, REFIID, void**);

void* g_target = nullptr;
Commit g_original = nullptr;
CurrentIndex g_currentIndex = nullptr;
GetBuffer g_getBuffer = nullptr;
std::atomic<bool> g_installed{false}, g_failed{false};
std::atomic<int> g_inHook{0};
std::atomic<bool> g_drawing{false};  // one thread draws at a time; others pass straight through
std::atomic<const char*> g_state{"not-installed"};
std::atomic<unsigned long long> g_ticks{0}, g_hidden{0}, g_skipped{0};
render_pass::ShaderBlobs g_shaders;
std::atomic<render_pass::StickRenderer*> g_renderer{nullptr};  // never deleted (Status reads it); abandoned if the GPU never retires it
unsigned long long g_nextQueueTry = 0;
const char* g_queueWhy = "not-tried";
thread_local bool t_inside = false;
// Echo's direct queue once validated (ValidateQueue). Echo owns it for the process lifetime; the
// renderer also holds a reference. Never cleared: capture may keep using it.
std::atomic<ID3D12CommandQueue*> g_echoQueue{nullptr};

void frame(void* session, void* chain) {
    render_pass::StickRenderer* renderer = g_renderer.load();
    if (!renderer) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    render_layer::EchoLayer layer;
    const bool layerOk = render_layer::readLayer(base + render_layer::kLayerRva, layer);
    if (layerOk && chain != layer.color[0]) return;  // Echo's depth chain, or a chain that is not Echo's eye colour
    selfie::Tick();
    g_ticks++;
    if (!layerOk) { g_state = "echo-layer-unreadable"; return; }
    selfie::RenderState state = {};
    if (!selfie::GetRenderState(&state)) { g_state = "no-render-state"; return; }
    if (!state.visible) { g_hidden++; g_state = "hidden"; return; }
    int colorIndex = -1, depthIndex = -1;
    if (g_currentIndex(session, layer.color[0], &colorIndex) < 0 || g_currentIndex(session, layer.depth[0], &depthIndex) < 0 ||
        colorIndex < 0 || colorIndex > 16 || depthIndex < 0 || depthIndex > 16) { g_skipped++; g_state = "swapchain-index-failed"; return; }
    Microsoft::WRL::ComPtr<ID3D12Resource> color, depth;
    if (g_getBuffer(session, layer.color[0], colorIndex, __uuidof(ID3D12Resource), reinterpret_cast<void**>(color.GetAddressOf())) < 0 || !color ||
        g_getBuffer(session, layer.depth[0], depthIndex, __uuidof(ID3D12Resource), reinterpret_cast<void**>(depth.GetAddressOf())) < 0 || !depth) {
        g_skipped++; g_state = "swapchain-buffer-failed"; return;  // e.g. not a D3D12 swap chain
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(color->GetDevice(IID_PPV_ARGS(&device))) || !device) { g_skipped++; g_state = "color-no-device"; return; }
    if (!renderer->initialized()) {
        const unsigned long long now = GetTickCount64();
        if (now < g_nextQueueTry) return;
        g_nextQueueTry = now + 2000;  // retry the queue lookup at most every 2 s
        ID3D12CommandQueue* raw = nullptr;
        if (!selfie_render::detail::ValidateQueue(render_layer::readPointer(base + selfie_render::detail::kQueueRva), device.Get(), &raw, &g_queueWhy)) {
            g_state = "echo-queue-not-validated"; return;
        }
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        queue.Attach(raw);
        // Validated: publish it for capture (EchoQueue) even if our own renderer then fails to build.
        g_echoQueue.store(queue.Get(), std::memory_order_release);
        if (!renderer->initialize(queue.Get(), g_shaders)) { g_failed = true; g_state = renderer->error ? renderer->error : "renderer-init-failed"; return; }
    }
    if (device.Get() != renderer->deviceHandle()) { g_failed = true; g_state = "echo-device-changed-disabled"; return; }
    switch (renderer->draw(color.Get(), depth.Get(), layer, state)) {
    case render_pass::StickRenderer::Drawn: g_state = "rendering"; break;
    case render_pass::StickRenderer::Busy: g_state = "rendering-gpu-busy-skip"; break;
    case render_pass::StickRenderer::Skipped: g_skipped++; g_state = renderer->error ? renderer->error : "skipped"; break;
    default: g_failed = true; g_state = renderer->error ? renderer->error : "draw-failed-disabled"; break;
    }
}

void guardedCpp(void* session, void* chain) {
    try {
        frame(session, chain);
    } catch (...) {
        g_failed = true;
        g_state = "exception-disabled";
    }
}
// SEH last resort (no C++ objects in this frame): an access violation disables rendering instead of killing the game.
void guarded(void* session, void* chain) {
    __try {
        guardedCpp(session, chain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;
        g_state = "seh-exception-disabled";
    }
}

int __cdecl onCommit(void* session, void* chain) {
    g_inHook.fetch_add(1);
    if (g_installed.load() && !g_failed.load() && !t_inside && session && chain) {
        bool expected = false;
        if (g_drawing.compare_exchange_strong(expected, true)) {
            t_inside = true;
            guarded(session, chain);
            t_inside = false;
            g_drawing = false;
        }
    }
    const int result = g_original(session, chain);
    g_inHook.fetch_sub(1);
    return result;
}
}  // namespace

bool Install() {
    if (g_installed.load()) return true;
    HMODULE ovr = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!ovr) { g_state = "libovr-not-loaded"; return false; }
    auto target = reinterpret_cast<void*>(GetProcAddress(ovr, "ovr_CommitTextureSwapChain"));
    g_currentIndex = reinterpret_cast<CurrentIndex>(GetProcAddress(ovr, "ovr_GetTextureSwapChainCurrentIndex"));
    g_getBuffer = reinterpret_cast<GetBuffer>(GetProcAddress(ovr, "ovr_GetTextureSwapChainBufferDX"));
    if (!target || !g_currentIndex || !g_getBuffer) { g_state = "libovr-exports-missing"; return false; }
    if (!GetModuleHandleW(L"d3d12.dll")) { g_state = "d3d12-not-loaded"; return false; }
    const char* why = nullptr;
    if (!g_shaders.vs && !render_pass::compileShaders(g_shaders, &why)) { g_state = why ? why : "shader-compile-failed"; return false; }
    // Keep LibOVR and this module mapped for the process lifetime: the trampoline and detour must never dangle.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(target), &pinned) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Install), &pinned)) {
        g_state = "pin-failed"; return false;
    }
    if (!g_renderer.load()) g_renderer = new (std::nothrow) render_pass::StickRenderer();
    if (!g_renderer.load()) { g_state = "out-of-memory"; return false; }
    g_failed = false;
    g_nextQueueTry = 0;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&onCommit), reinterpret_cast<void**>(&g_original)) != MH_OK) { g_state = "hook-create-failed"; return false; }
    g_target = target;
    g_installed = true;  // set before enabling: the detour checks it
    if (MH_EnableHook(target) != MH_OK) {
        g_installed = false;
        MH_RemoveHook(target);
        g_target = nullptr; g_original = nullptr;
        g_state = "hook-enable-failed";
        return false;
    }
    g_state = "installed-waiting-for-frame";
    return true;
}

void Uninstall() {
    if (!g_installed.load() || !g_target) return;
    g_installed = false;  // detour stops drawing immediately
    MH_DisableHook(g_target);
    Sleep(20);
    for (int i = 0; i < 100 && g_inHook.load() != 0; i++) Sleep(10);
    if (g_inHook.load() == 0) {
        MH_RemoveHook(g_target);  // trampoline freed only when nobody is inside it
        g_target = nullptr;
    }
    // Free GPU objects only after our last submission retired; otherwise leak them (safe).
    // The small StickRenderer object itself is kept (Status() may read it from another thread).
    if (render_pass::StickRenderer* r = g_renderer.load()) {
        if (r->waitIdle(2000)) r->reset();
        else g_renderer = new (std::nothrow) render_pass::StickRenderer();  // abandon the busy one (leak, never freed under the GPU)
    }
    g_state = "uninstalled";
}

ID3D12CommandQueue* EchoQueue() { return g_echoQueue.load(std::memory_order_acquire); }

const char* Status() {
    thread_local char text[320];
    const render_pass::StickRenderer* r = g_renderer.load();
    std::snprintf(text, sizeof(text),
                  "render: %s%s installed=%d ticks=%llu frames=%llu busy=%llu skipped=%llu hidden=%llu colorFmt=%d depthFmt=%d reversedZ=%d queue=%s",
                  g_state.load(), g_failed.load() ? " (disabled)" : "", g_installed.load() ? 1 : 0, g_ticks.load(),
                  r ? r->frames.load() : 0ULL, r ? r->busy.load() : 0ULL, g_skipped.load(), g_hidden.load(),
                  r ? r->lastColorFormat.load() : 0, r ? r->lastDepthFormat.load() : 0, r ? r->lastReversed.load() : -1,
                  r && r->initialized() ? "echo-validated" : (g_queueWhy ? g_queueWhy : "?"));
    return text;
}

}  // namespace selfie_render
