// render_tests.cpp - offline GPU tests for the SelfieStick render module (WARP device).
// Pattern from EchoCraft's renderer_test.cpp / host_overlay_test.cpp / gpu_test_support.h.
//
// A synthetic Echo frame: one colour texture holding both eyes side by side (with a gap),
// a matching R24G8 (or R32) depth texture, LibOVR-style FOV/eye poses and Echo's depth row.
// Both arrive and must leave in PIXEL_SHADER_RESOURCE, like Echo's hand-off.
// Expected pixel positions are computed here with an independent pinhole formula.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "selfie_render.h"
#include "render_pass.h"
#include "vendor/minhook/include/MinHook.h"

using Microsoft::WRL::ComPtr;

// ---- local stub of core (the real one lives in selfie_core.cpp) -------------------------------
static selfie::RenderState g_stubState = {};
static int g_stubTicks = 0;
namespace selfie {
void Tick() { g_stubTicks++; }
bool GetRenderState(RenderState* out) { if (!out) return false; *out = g_stubState; return true; }
}  // namespace selfie

static int g_checks = 0;
static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) { std::printf("FAIL: %s\n", what); std::exit(1); }
}

// ---- WARP device helper -------------------------------------------------------------------------
struct GPU {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    UINT64 serial = 0;
    explicit GPU(bool debug) {
        if (debug) {
            ComPtr<ID3D12Debug> d;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) d->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
        check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
        check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12 WARP device");
        if (debug) device.As(&info);
        D3D12_COMMAND_QUEUE_DESC desc = {};
        check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "command objects");
    }
    void flush() {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(e && SUCCEEDED(queue->Signal(fence.Get(), ++serial)) && SUCCEEDED(fence->SetEventOnCompletion(serial, e)) &&
              WaitForSingleObject(e, 10000) == WAIT_OBJECT_0, "GPU completion");
        CloseHandle(e);
    }
    void submit() {
        check(SUCCEEDED(list->Close()), "close list");
        ID3D12CommandList* c[] = {list.Get()};
        queue->ExecuteCommandLists(1, c);
        flush();
        check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "reset list");
    }
    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    }
    // Reads subresource 0 (colour, or the depth plane) as 4 bytes per texel. `state` is both entry and exit state.
    std::vector<uint32_t> read(ID3D12Resource* t, D3D12_RESOURCE_STATES state) {
        auto desc = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> buffer;
        check(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))), "readback buffer");
        transition(t, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = t; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        submit();
        unsigned char* data = nullptr; D3D12_RANGE range = {0, size_t(bytes)};
        check(SUCCEEDED(buffer->Map(0, &range, reinterpret_cast<void**>(&data))), "map readback");
        std::vector<uint32_t> out(size_t(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; y++) std::memcpy(out.data() + size_t(y) * desc.Width, data + fp.Offset + size_t(y) * fp.Footprint.RowPitch, size_t(desc.Width) * 4);
        D3D12_RANGE none = {0, 0}; buffer->Unmap(0, &none);
        return out;
    }
    unsigned debugErrors() {
        if (!info) return 0;
        unsigned errors = 0;
        UINT64 n = info->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; i++) {
            SIZE_T len = 0; info->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (FAILED(info->GetMessage(i, m, &len))) continue;
            if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                errors++;
                std::printf("  debug-layer: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
            }
        }
        info->ClearStoredMessages();
        return errors;
    }
};

// ---- synthetic Echo frame -------------------------------------------------------------------------
constexpr UINT kEyeW = 512, kEyeH = 512, kGap = 16, kTexW = kEyeW * 2 + kGap, kTexH = kEyeH;
constexpr float kNear = 0.05f, kFar = 100.f;
constexpr float kEyeX[2] = {-0.032f, 0.032f};
constexpr float kEyeY = 1.6f;

static render_layer::EchoLayer makeLayer(bool reversed, void* colorChain, void* depthChain) {
    render_layer::EchoLayer l;
    l.color[0] = colorChain; l.depth[0] = depthChain;
    for (int e = 0; e < 2; e++) {
        l.viewport[e] = {int(e * (kEyeW + kGap)), 0, int(kEyeW), int(kEyeH)};
        l.fov[e] = {1, 1, 1, 1};
        l.pose[e] = {{0, 0, 0, 1}, {kEyeX[e], kEyeY, 0}};
    }
    if (reversed) { l.p22 = 0; l.p23 = kNear; l.p32 = -1; }                        // infinite-far reversed Z
    else { l.p22 = kFar / (kNear - kFar); l.p23 = kFar * kNear / (kNear - kFar); l.p32 = -1; }  // LibOVR forward Z
    return l;
}
static float depthAt(bool reversed, float distance) {  // independent of render_math
    const double z = -distance;
    if (reversed) return float(kNear / distance);
    double p22 = kFar / (kNear - kFar), p23 = kFar * kNear / (kNear - kFar);
    return float((p22 * z + p23) / (-z));
}
struct Pixel { int x, y; };
static Pixel project(int eye, float px, float py, float pz) {  // identity eye orientation, tan 1 FOV
    double rx = px - kEyeX[eye], ry = py - kEyeY, rz = pz;
    double nx = rx / -rz, ny = ry / -rz;
    return {int(eye * (kEyeW + kGap) + (nx + 1) * 0.5 * kEyeW), int((1 - ny) * 0.5 * kEyeH)};
}

static selfie::RenderState makeState(int highlight, float length = selfie::kStickLength) {
    selfie::RenderState s = {};
    s.sequence = 1; s.visible = 1; s.highlight = highlight;
    s.length = length;
    s.stick = {{0, 0, 0, 1}, {0, 0.9f, -0.8f}};                                  // butt at y=0.9, vertical
    s.camHead = {{0, 0, 0, 1}, {0, 0.9f + length + 0.025f, -0.8f}};             // lens (+Z) faces the viewer
    s.countdownPose = {{0, 0, 0, 1}, {0, 0, 0}};
    return s;
}

struct Targets {
    ComPtr<ID3D12Resource> color, depth;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    DXGI_FORMAT colorFormat, rtvFormat, depthFormat, dsvFormat;
};
static Targets makeTargets(GPU& gpu, DXGI_FORMAT colorFormat, DXGI_FORMAT rtvFormat, DXGI_FORMAT depthFormat, DXGI_FORMAT dsvFormat,
                           UINT w = kTexW, UINT h = kTexH, UINT dw = kTexW, UINT dh = kTexH) {
    Targets t; t.colorFormat = colorFormat; t.rtvFormat = rtvFormat; t.depthFormat = depthFormat; t.dsvFormat = dsvFormat;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = colorFormat; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check(SUCCEEDED(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&t.color))), "colour texture");
    td.Width = dw; td.Height = dh; td.Format = depthFormat; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    check(SUCCEEDED(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&t.depth))), "depth texture");
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.rtvHeap))), "rtv heap");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.dsvHeap))), "dsv heap");
    D3D12_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = rtvFormat; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    gpu.device->CreateRenderTargetView(t.color.Get(), &rd, t.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DEPTH_STENCIL_VIEW_DESC dd = {}; dd.Format = dsvFormat; dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    gpu.device->CreateDepthStencilView(t.depth.Get(), &dd, t.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return t;
}
// Simulates Echo's frame: green colour, depth cleared to "far", optional nearer occluder rectangle.
static void echoFrame(GPU& gpu, Targets& t, bool reversed, const D3D12_RECT* occluder = nullptr, float occluderDepth = 0) {
    gpu.transition(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu.transition(t.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const float green[4] = {0, 1, 0, 1};
    gpu.list->ClearRenderTargetView(t.rtvHeap->GetCPUDescriptorHandleForHeapStart(), green, 0, nullptr);
    gpu.list->ClearDepthStencilView(t.dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, reversed ? 0.f : 1.f, 0, 0, nullptr);
    if (occluder) gpu.list->ClearDepthStencilView(t.dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, occluderDepth, 0, 1, occluder);
    gpu.transition(t.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu.transition(t.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu.submit();
}

struct RGB { int r, g, b; };
struct Image {
    std::vector<uint32_t> px; bool bgra;
    RGB at(Pixel p) const {
        if (p.x < 0 || p.y < 0 || p.x >= int(kTexW) || p.y >= int(kTexH)) { std::printf("FAIL: test pixel (%d,%d) outside the image\n", p.x, p.y); std::exit(1); }
        uint32_t v = px[size_t(p.y) * kTexW + p.x];
        int c0 = v & 0xff, c1 = (v >> 8) & 0xff, c2 = (v >> 16) & 0xff;
        return bgra ? RGB{c2, c1, c0} : RGB{c0, c1, c2};
    }
    bool background(Pixel p) const { RGB c = at(p); return c.r == 0 && c.g == 255 && c.b == 0; }
};
static float depthValue(const std::vector<uint32_t>& d, Pixel p, DXGI_FORMAT f) {
    uint32_t v = d[size_t(p.y) * kTexW + p.x];
    if (f == DXGI_FORMAT_R24G8_TYPELESS) return float(v & 0xffffff) / 16777215.f;
    float x; std::memcpy(&x, &v, 4); return x;
}

static render_pass::ShaderBlobs g_blobs;

static void drawOk(render_pass::StickRenderer& r, GPU& gpu, Targets& t, const render_layer::EchoLayer& l, const selfie::RenderState& s, const char* what) {
    auto result = r.draw(t.color.Get(), t.depth.Get(), l, s);
    if (result != render_pass::StickRenderer::Drawn) std::printf("  draw result %d error=%s\n", int(result), r.error ? r.error : "-");
    check(result == render_pass::StickRenderer::Drawn, what);
    gpu.flush();
}

// The main scene: stick visible in both eyes, colours, depth, untouched pixels, occlusion, highlight.
static void sceneTests(GPU& gpu, DXGI_FORMAT colorFormat, DXGI_FORMAT rtvFormat, DXGI_FORMAT depthFormat, DXGI_FORMAT dsvFormat, bool reversed, const char* label) {
    std::printf("scene %s (%s Z)\n", label, reversed ? "reversed" : "forward");
    Targets t = makeTargets(gpu, colorFormat, rtvFormat, depthFormat, dsvFormat);
    const bool bgra = colorFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS || colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM || colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    render_pass::StickRenderer r;
    check(r.initialize(gpu.queue.Get(), g_blobs), "renderer initialize");
    int fakeColor = 0, fakeDepth = 0;
    auto layer = makeLayer(reversed, &fakeColor, &fakeDepth);
    auto state = makeState(0);
    const float camY = 0.9f + selfie::kStickLength + 0.025f;
    const float lensZ = -0.8f + selfie::kCamBox[2] * 0.5f + render_mesh::kRingDepth + 0.001f;
    for (int eye = 0; eye < 2; eye++) {
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, state, "draw scene");
        Image img{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), bgra};
        auto depth = gpu.read(t.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const Pixel handle = project(eye, 0, 0.9f + 0.14f, -0.8f + selfie::kHandleRadius);
        const Pixel shaft = project(eye, 0, 0.9f + 0.6f, -0.8f + selfie::kShaftRadius);
        const Pixel lens = project(eye, 0, camY, lensZ);
        const Pixel ring = project(eye, 0.016f, camY, lensZ - 0.001f);
        const Pixel box = project(eye, 0.03f, camY - 0.02f, -0.8f + selfie::kCamBox[2] * 0.5f);
        RGB h = img.at(handle), s = img.at(shaft), le = img.at(lens), ri = img.at(ring), bx = img.at(box);
        std::printf("  eye %d: handle(%d,%d)=%d,%d,%d shaft=%d,%d,%d lens=%d,%d,%d ring=%d,%d,%d box=%d,%d,%d\n", eye, handle.x, handle.y,
                    h.r, h.g, h.b, s.r, s.g, s.b, le.r, le.g, le.b, ri.r, ri.g, ri.b, bx.r, bx.g, bx.b);
        check(!img.background(handle) && h.r < 80 && h.g < 80 && h.b < 80 && h.r > 5, "handle drawn dark grey");
        check(!img.background(shaft) && s.r > 120 && s.g > 120 && s.b > 120, "shaft drawn silver");
        check(!img.background(lens) && le.r < 30 && le.g < 30 && le.b < 40, "lens glass dark at camera centre");
        check(!img.background(ring) && ri.b > 150 && ri.b > ri.r + 100 && ri.b > ri.g + 60, "blue lens ring");
        check(!img.background(box) && bx.r < 45 && bx.g < 45 && bx.b < 45, "black camera box");
        const float dh = depthValue(depth, handle, depthFormat), expect = depthAt(reversed, 0.8f - selfie::kHandleRadius);
        std::printf("  eye %d: handle depth %.5f (pinhole %.5f)\n", eye, dh, expect);
        check(std::fabs(dh - expect) < 0.004f, "stick writes Echo-comparable depth");
        // Untouched pixels: corners, the gap between the eye viewports, left of the stick.
        const Pixel untouched[] = {{0, 0}, {int(kTexW) - 1, int(kTexH) - 1}, {int(kEyeW) + kGap / 2, handle.y}, {handle.x - 60, handle.y}, {int(kEyeW) + 2, 10}};
        for (Pixel p : untouched) {
            check(img.background(p), "untouched colour preserved exactly");
            check(depthValue(depth, p, depthFormat) == (reversed ? 0.f : 1.f), "untouched depth preserved exactly");
        }
        // Count changed pixels: only a small part of the image may change, and nothing in the gap columns.
        size_t changed = 0;
        for (UINT y = 0; y < kTexH; y++) for (UINT x = 0; x < kTexW; x++) {
            bool bg = img.background({int(x), int(y)});
            if (!bg) changed++;
            if (x >= kEyeW && x < kEyeW + kGap) check(bg, "nothing drawn between the eye viewports");
        }
        check(changed > 500 && changed < kTexW * kTexH / 10, "stick covers a small, non-empty area");

        // Highlight: same frame with highlight=1 -> handle brighter, shaft unchanged.
        echoFrame(gpu, t, reversed);
        auto lit = makeState(1);
        drawOk(r, gpu, t, layer, lit, "draw highlighted");
        Image img2{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), bgra};
        RGB h2 = img2.at(handle), s2 = img2.at(shaft);
        check(h2.r > h.r + 30 && h2.g > h.g + 30 && h2.b > h.b + 30, "highlight brightens the handle");
        check(s2.r == s.r && s2.g == s.g && s2.b == s.b, "highlight leaves the shaft alone");

        // Occlusion: Echo geometry (e.g. a hand) 0.3 m away covering the handle wins; the shaft is still drawn.
        D3D12_RECT occ = {handle.x - 20, handle.y - 20, handle.x + 20, handle.y + 20};
        const float occDepth = depthAt(reversed, 0.3f);
        echoFrame(gpu, t, reversed, &occ, occDepth);
        drawOk(r, gpu, t, layer, state, "draw behind occluder");
        Image img3{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), bgra};
        auto depth3 = gpu.read(t.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        check(img3.background(handle), "nearer Echo depth occludes the handle");
        check(std::fabs(depthValue(depth3, handle, depthFormat) - occDepth) < 1e-5f, "occluded pixel keeps Echo's depth");
        check(!img3.background(shaft), "unoccluded shaft still drawn");
        // Something FARTHER than the stick (a wall at 3 m) does not hide it.
        echoFrame(gpu, t, reversed, &occ, depthAt(reversed, 3.0f));
        drawOk(r, gpu, t, layer, state, "draw in front of far geometry");
        Image img4{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), bgra};
        check(!img4.background(handle), "stick drawn over farther Echo geometry");
    }
    check(r.lastReversed.load() == (reversed ? 1 : 0), "depth direction auto-detected from Echo's projection row");
    check(r.waitIdle(5000), "renderer idle");
    check(gpu.debugErrors() == 0, "no D3D12 debug-layer errors (resource states, bindings)");
}

// Every rejected input must leave Echo's image untouched and submit nothing.
static void rejectionTests(GPU& gpu) {
    std::printf("rejections\n");
    Targets t = makeTargets(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
    render_pass::StickRenderer r;
    check(r.initialize(gpu.queue.Get(), g_blobs), "renderer initialize");
    int c = 0, d = 0;
    echoFrame(gpu, t, false);
    auto before = gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const auto good = makeLayer(false, &c, &d);
    using R = render_pass::StickRenderer;
    auto expectSkip = [&](const render_layer::EchoLayer& l, const selfie::RenderState& s, ID3D12Resource* col, ID3D12Resource* dep, const char* what) {
        auto res = r.draw(col, dep, l, s);
        if (res != R::Skipped) std::printf("  %s: result %d\n", what, int(res));
        check(res == R::Skipped, what);
    };
    auto s = makeState(0);
    auto hidden = s; hidden.visible = 0;
    expectSkip(good, hidden, t.color.Get(), t.depth.Get(), "visible=0 draws nothing");
    auto nanPose = s; nanPose.stick.q.x = std::numeric_limits<float>::quiet_NaN();
    expectSkip(good, nanPose, t.color.Get(), t.depth.Get(), "NaN stick pose rejected");
    auto zeroQuat = s; zeroQuat.camHead.q = {0, 0, 0, 0};
    expectSkip(good, zeroQuat, t.color.Get(), t.depth.Get(), "zero camHead quaternion rejected");
    auto farPose = s; farPose.stick.p.x = 1e9f;
    expectSkip(good, farPose, t.color.Get(), t.depth.Get(), "absurd position rejected");
    auto bigViewport = good; bigViewport.viewport[1].width = int(kEyeW) + 64;
    expectSkip(bigViewport, s, t.color.Get(), t.depth.Get(), "viewport outside the texture rejected");
    auto leftHanded = good; leftHanded.p32 = 1;
    expectSkip(leftHanded, s, t.color.Get(), t.depth.Get(), "non-LibOVR projection row rejected");
    auto nanFov = good; nanFov.fov[0].left = std::numeric_limits<float>::quiet_NaN();
    expectSkip(nanFov, s, t.color.Get(), t.depth.Get(), "NaN FOV rejected");
    auto badEye = good; badEye.pose[1].orientation[3] = 3;
    expectSkip(badEye, s, t.color.Get(), t.depth.Get(), "non-unit eye quaternion rejected");
    expectSkip(good, s, nullptr, t.depth.Get(), "missing colour buffer rejected");
    // Shape / format mismatches.
    Targets halfDepth = makeTargets(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
                                kTexW, kTexH, kTexW / 2, kTexH);
    expectSkip(good, s, halfDepth.color.Get(), halfDepth.depth.Get(), "colour/depth size mismatch rejected");
    Targets odd = makeTargets(gpu, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
    expectSkip(good, s, odd.color.Get(), odd.depth.Get(), "unsupported colour format rejected");
    expectSkip(good, s, t.depth.Get(), t.color.Get(), "swapped colour/depth rejected");
    gpu.flush();
    auto after = gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    check(before == after, "rejected frames leave Echo's image byte-identical");
    check(r.frames.load() == 0, "rejected frames submit nothing");
    check(gpu.debugErrors() == 0, "no debug-layer errors in rejections");
}

// The GPU is stalled (queue waits on a fence nobody signals): we must skip, never block.
static void nonStallTests(GPU& gpu) {
    std::printf("non-stall\n");
    Targets t = makeTargets(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
    render_pass::StickRenderer r;
    check(r.initialize(gpu.queue.Get(), g_blobs), "renderer initialize");
    int c = 0, d = 0;
    auto l = makeLayer(false, &c, &d);
    auto s = makeState(0);
    echoFrame(gpu, t, false);
    ComPtr<ID3D12Fence> blocker;
    check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker))), "blocker fence");
    check(SUCCEEDED(gpu.queue->Wait(blocker.Get(), 1)), "stall the queue");
    using R = render_pass::StickRenderer;
    const auto start = std::chrono::steady_clock::now();
    int drawn = 0, busy = 0;
    for (int i = 0; i < 6; i++) {
        auto res = r.draw(t.color.Get(), t.depth.Get(), l, s);
        if (res == R::Drawn) drawn++;
        else if (res == R::Busy) busy++;
        else check(false, "unexpected result while the GPU is stalled");
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  stalled GPU: drawn=%d busy=%d in %.2f ms\n", drawn, busy, ms);
    check(drawn == 3 && busy == 3, "three in-flight frames, then skip (fence-gated slots)");
    check(ms < 250, "draw() never waits for the GPU");
    check(!r.idle(), "work really is pending");
    check(SUCCEEDED(blocker->Signal(1)), "release the queue");
    check(r.waitIdle(10000), "stalled work retires");
    check(r.draw(t.color.Get(), t.depth.Get(), l, s) == R::Drawn, "drawing resumes once slots retire");
    gpu.flush();
    // A projection-direction change needs a new pipeline: allowed only when idle (else Busy, never waits).
    auto rev = makeLayer(true, &c, &d);
    check(SUCCEEDED(gpu.queue->Wait(blocker.Get(), 2)), "stall again");
    check(r.draw(t.color.Get(), t.depth.Get(), l, s) == R::Drawn, "draw with the queue stalled");
    check(r.draw(t.color.Get(), t.depth.Get(), rev, s) == R::Busy, "pipeline swap deferred while the GPU is busy");
    check(SUCCEEDED(blocker->Signal(2)), "release again");
    check(r.waitIdle(10000), "idle");
    echoFrame(gpu, t, true);
    check(r.draw(t.color.Get(), t.depth.Get(), rev, s) == R::Drawn, "pipeline swapped once idle");
    check(r.waitIdle(10000), "idle after swap");
    check(r.lastReversed.load() == 1, "reversed Z picked up");
    check(gpu.debugErrors() == 0, "no debug-layer errors in non-stall tests");
}

static void layerTests() {
    std::printf("layer header\n");
    unsigned char raw[render_layer::kLayerBytes] = {};
    int type = 2; std::memcpy(raw, &type, 4);
    void* chains[4] = {reinterpret_cast<void*>(0x1000), nullptr, reinterpret_cast<void*>(0x2000), nullptr};
    std::memcpy(raw + 136, chains, 16); std::memcpy(raw + 280, chains + 2, 16);
    render_layer::Rect vp[2] = {{0, 0, 1000, 1100}, {1000, 0, 1000, 1100}};
    std::memcpy(raw + 152, vp, 32);
    render_layer::Fov fov[2] = {{1.2f, 1.3f, 1.1f, 0.9f}, {1.2f, 1.3f, 0.9f, 1.1f}};
    std::memcpy(raw + 184, fov, 32);
    render_layer::Pose pose[2] = {{{0, 0, 0, 1}, {-0.03f, 1.7f, 0.1f}}, {{0, 0, 0, 1}, {0.03f, 1.7f, 0.1f}}};
    std::memcpy(raw + 216, pose, 56);
    float proj[3] = {-1.0005f, -0.05f, -1.f};
    std::memcpy(raw + 296, proj, 12);
    render_layer::EchoLayer l;
    check(render_layer::parseLayer(raw, sizeof(raw), l), "valid header parses");
    check(l.color[0] == chains[0] && l.depth[0] == chains[2] && l.viewport[1].x == 1000 && l.fov[0].down == 1.3f &&
          l.pose[1].position[0] == 0.03f && l.p22 == -1.0005f && l.p32 == -1.f, "fields at the documented offsets");
    check(render_layer::readLayer(reinterpret_cast<uintptr_t>(raw), l), "fault-safe read of a valid header");
    check(!render_layer::readLayer(0, l) && !render_layer::readLayer(0x10, l), "unreadable address rejected without faulting");
    auto mutate = [&](size_t offset, const void* value, size_t n, const char* what) {
        unsigned char copy[sizeof(raw)]; std::memcpy(copy, raw, sizeof(raw)); std::memcpy(copy + offset, value, n);
        render_layer::EchoLayer x; check(!render_layer::parseLayer(copy, sizeof(copy), x), what);
    };
    int quad = 1; mutate(0, &quad, 4, "non-EyeFovDepth layer type rejected");
    void* null = nullptr; mutate(280, &null, 8, "missing depth chain rejected");
    void* other = reinterpret_cast<void*>(0x3000); mutate(144, &other, 8, "distinct second colour chain rejected");
    mutate(280, &chains[0], 8, "colour chain == depth chain rejected");
    int neg = -5; mutate(152, &neg, 4, "negative viewport rejected");
    float nan = std::numeric_limits<float>::quiet_NaN(); mutate(184, &nan, 4, "NaN FOV rejected");
    mutate(296, &nan, 4, "NaN projection rejected");
    float two = 2.f; mutate(216 + 12, &two, 4, "non-unit eye quaternion rejected");
    check(!render_layer::parseLayer(raw, sizeof(raw) - 1, l), "short buffer rejected");
}

static void mathAndMeshTests() {
    std::printf("math + mesh\n");
    // clipFromObject against the pinhole formula, rotated eye and object.
    auto p = render_math::projection(1.1f, 0.9f, 1.2f, 0.8f, -1.0005f, -0.05f, -1);
    check(p.valid && !p.reversed, "forward projection valid");
    auto pr = render_math::projection(1, 1, 1, 1, 0, 0.05f, -1);
    check(pr.valid && pr.reversed, "reversed projection detected");
    check(!render_math::projection(1, 1, 1, 1, -1, -0.05f, 1).valid, "left-handed projection rejected");
    const float yaw = 0.4f;
    const float eq[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, ep[3] = {0.2f, 1.5f, 0.3f};
    const float oq[4] = {std::sin(0.3f), 0, 0, std::cos(0.3f)}, op[3] = {0.1f, 1.2f, -0.6f};
    render_math::Affine eye, obj;
    check(render_math::fromPose(eq, ep, eye) && render_math::fromPose(oq, op, obj), "poses");
    float m[16];
    check(render_math::clipFromObject(p, eye, obj, m), "clip matrix");
    const double local[3] = {0.01, 0.2, 0.03};
    double world[3]; obj.apply(local, world);
    // Into eye space by hand: v = R_eye^T (world - eyePos), R_eye = yaw about Y.
    double d[3] = {world[0] - ep[0], world[1] - ep[1], world[2] - ep[2]};
    double c = std::cos(yaw), s = std::sin(yaw);
    double ex = c * d[0] - s * d[2], ey = d[1], ez = s * d[0] + c * d[2];
    // Asymmetric FOV (tangents up=1.1 down=0.9 left=1.2 right=0.8): the image edges are at x/(-z) = -left and +right,
    // so x' = (2x/(-z) + (l-r)) / (l+r) and y' = (2y/(-z) + (d-u)) / (u+d).
    const double ndcX = (2 * (ex / -ez) + (1.2 - 0.8)) / 2.0;
    const double ndcY = (2 * (ey / -ez) + (0.9 - 1.1)) / 2.0;
    double cx = m[0] * local[0] + m[1] * local[1] + m[2] * local[2] + m[3];
    double cy = m[4] * local[0] + m[5] * local[1] + m[6] * local[2] + m[7];
    double cw = m[12] * local[0] + m[13] * local[1] + m[14] * local[2] + m[15];
    check(std::fabs(cx / cw - ndcX) < 1e-4 && std::fabs(cy / cw - ndcY) < 1e-4, "clip matrix matches the LibOVR off-axis pinhole");
    check(std::fabs(cw - (-ez)) < 1e-4, "w = -z (right-handed LibOVR)");

    auto mesh = render_mesh::build();
    const render_mesh::Range* order[] = {&mesh.fixed, &mesh.shaft, &mesh.top, &mesh.cam, &mesh.digit};
    unsigned expectFirst = 0;
    for (const auto* r : order) {
        check(r->first == expectFirst && r->count > 0 && r->count % 3 == 0, "mesh ranges contiguous, whole triangles");
        expectFirst += r->count;
    }
    check(mesh.vertices.size() == expectFirst, "mesh ranges cover every vertex");
    check(mesh.vertices.size() < 6000, "mesh is tiny");
    auto in = [](const render_mesh::Range& r, size_t i) { return i >= r.first && i < r.first + r.count; };
    const float button[3] = {0, selfie::kShutterY, selfie::kHandleRadius - 0.002f};
    size_t buttonVerts = 0, digitWhite = 0, digitDark = 0;
    for (size_t i = 0; i < mesh.vertices.size(); i++) {
        const auto& v = mesh.vertices[i];
        for (float x : v.p) check(std::isfinite(x), "finite position");
        double n = std::sqrt(double(v.n[0]) * v.n[0] + double(v.n[1]) * v.n[1] + double(v.n[2]) * v.n[2]);
        check(std::fabs(n - 1) < 1e-4, "unit normals");
        double radial = std::sqrt(double(v.p[0]) * v.p[0] + double(v.p[2]) * v.p[2]);
        const int flag = int(v.c[3] + 0.5f);
        if (in(mesh.fixed, i)) {
            check(v.p[1] >= -1e-6f && v.p[1] <= selfie::kHandleLength + 1e-6f, "fixed part within the handle length (length-independent)");
            if (flag == 2) {
                buttonVerts++;
                double dist = std::sqrt(std::pow(v.p[0] - button[0], 2) + std::pow(v.p[1] - button[1], 2) + std::pow(v.p[2] - button[2], 2));
                check(std::fabs(dist - selfie::kShutterRadius) < 1e-5 && v.p[2] >= button[2] - 1e-6f, "shutter dome: hemisphere on the +Z side");
            } else {
                check(flag == 1 && radial <= selfie::kHandleRadius + 1e-5, "handle vertices flagged for highlight, within the handle radius");
            }
        } else if (in(mesh.shaft, i)) {
            check(v.p[1] >= -1e-6f && v.p[1] <= 1 + 1e-6f && std::fabs(radial - selfie::kShaftRadius) < 1e-5 && flag == 0 && std::fabs(v.n[1]) < 1e-6f,
                  "unit shaft: y in [0,1], open tube, radial normals (scale-invariant)");
        } else if (in(mesh.top, i)) {
            check(v.p[1] >= -render_mesh::kCollarH - 1e-6f && v.p[1] <= 1e-6f && radial <= selfie::kHandleRadius && flag == 0, "collar below the top");
        } else if (in(mesh.cam, i)) {
            check(std::fabs(v.p[0]) <= selfie::kCamBox[0] / 2 + 1e-6f && std::fabs(v.p[1]) <= selfie::kCamBox[1] / 2 + 1e-6f, "camera within the box outline");
            check(v.p[2] >= -selfie::kCamBox[2] / 2 - 1e-6f && v.p[2] <= selfie::kCamBox[2] / 2 + render_mesh::kRingDepth + 0.0011f, "lens on local +Z");
            check(flag == 0 || (flag == 3 && v.p[2] >= selfie::kCamBox[2] / 2 - 0.0021f), "only lens ring/glass flagged for the flash");
        } else {
            check((flag >= 10 && flag <= 16) || (flag >= 20 && flag <= 26), "digit flags");
            if (flag >= 20) { digitDark++; check(std::fabs(v.p[2] + render_mesh::kOutlineBack) < 1e-6f, "outline behind the face"); }
            else { digitWhite++; check(v.p[2] == 0 && v.c[0] == 1 && v.c[1] == 1 && v.c[2] == 1, "white face at z=0"); }
            check(std::fabs(v.p[0]) <= render_mesh::kDigitW / 2 + render_mesh::kOutline + 1e-6f &&
                  std::fabs(v.p[1]) <= render_mesh::kDigitH / 2 + render_mesh::kOutline + 1e-6f, "digit within its box");
        }
    }
    check(buttonVerts > 0 && digitWhite == 7 * 6 && digitDark == 7 * 6, "button and 7 segments (face + outline) present");
    auto bits = [](unsigned m) { int c = 0; for (; m; m &= m - 1) c++; return c; };
    check(bits(render_mesh::digitMask(1)) == 2 && bits(render_mesh::digitMask(2)) == 5 && bits(render_mesh::digitMask(3)) == 5 &&
          render_mesh::digitMask(0) == 0 && render_mesh::digitMask(4) == 0, "7-segment masks");
    check((render_mesh::digitMask(1) & 0x6) == 0x6 && (render_mesh::digitMask(2) & 0x20) == 0 && (render_mesh::digitMask(3) & 0x10) == 0,
          "1 = b+c, 2 has no f, 3 has no e");

    // planFrame: the unit shaft's ends land at kShaftBottom and length - kCollarH in stick space; collar top at length.
    int c0 = 0, d0 = 0;
    auto layer = makeLayer(false, &c0, &d0);
    for (float len : {0.5f, 1.0f, 1.75f, 2.0f}) {
        auto st = makeState(0, len);
        st.stick.q = {std::sin(0.2f), 0, std::sin(0.1f), 1};   // tilted (normalized by fromPose)
        render_pass::FramePlan plan;
        check(render_pass::planFrame(layer, st, kTexW, kTexH, plan) == nullptr && plan.length == len, "plan with length");
        auto apply = [](const float m[16], double x, double y, double z, double out[4]) {
            for (int r = 0; r < 4; r++) out[r] = m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3];
        };
        auto same = [&](const float a[16], double ay, const float b[16], double by) {
            double pa[4], pb[4];
            apply(a, 0.012, ay, 0.0, pa); apply(b, 0.012, by, 0.0, pb);
            for (int k = 0; k < 4; k++) if (std::fabs(pa[k] - pb[k]) > 1e-4) return false;
            return true;
        };
        const auto& dr = plan.draws[0];
        check(same(dr[render_pass::kPartShaft].clip, 1.0, dr[render_pass::kPartFixed].clip, len - render_mesh::kCollarH), "shaft top at length - collar");
        check(same(dr[render_pass::kPartShaft].clip, 0.0, dr[render_pass::kPartFixed].clip, render_mesh::kShaftBottom), "shaft bottom fixed in the handle");
        check(same(dr[render_pass::kPartTop].clip, 0.0, dr[render_pass::kPartFixed].clip, len), "collar top at the length");
        check(std::memcmp(dr[render_pass::kPartShaft].rot, dr[render_pass::kPartFixed].rot, sizeof(dr[0].rot)) == 0, "shaft normals use the unscaled rotation");
        check(!plan.digit, "no digit without a countdown");
    }
    auto bad = makeState(0, 0.0f);
    render_pass::FramePlan plan;
    check(render_pass::planFrame(layer, bad, kTexW, kTexH, plan) == nullptr && plan.length == selfie::kStickLength, "length 0 (unset) -> default");
    bad.length = std::numeric_limits<float>::quiet_NaN();
    check(render_pass::planFrame(layer, bad, kTexW, kTexH, plan) == nullptr && plan.length == selfie::kStickLength, "NaN length -> default");
    bad.length = 7.f;
    check(render_pass::planFrame(layer, bad, kTexW, kTexH, plan) == nullptr && plan.length == selfie::kStickLength, "absurd length -> default");
    auto dig = makeState(0);
    dig.countdownDigit = 2; dig.countdownAlpha = 0.5f; dig.countdownPose = {{0, 0, 0, 1}, {0, 1.6f, -0.6f}};
    check(render_pass::planFrame(layer, dig, kTexW, kTexH, plan) == nullptr && plan.digit &&
          plan.draws[1][render_pass::kPartDigit].rot[0][3] == float(render_mesh::digitMask(2)) && plan.draws[1][render_pass::kPartDigit].params[3] == 0.5f,
          "digit planned with its mask and alpha");
    dig.countdownPose.q = {0, 0, 0, 0};
    check(render_pass::planFrame(layer, dig, kTexW, kTexH, plan) == nullptr && !plan.digit, "bad digit pose: digit dropped, stick still planned");
    dig.countdownPose.q = {0, 0, 0, 1}; dig.countdownAlpha = 0;
    check(render_pass::planFrame(layer, dig, kTexW, kTexH, plan) == nullptr && !plan.digit, "alpha 0: no digit");
    dig.countdownAlpha = 1; dig.countdownDigit = 7;
    check(render_pass::planFrame(layer, dig, kTexW, kTexH, plan) == nullptr && !plan.digit, "digit outside 1..3: no digit");
}

// ---- v2 GPU tests: length, shutter button, flash, self-timer digit --------------------------------
static int lum(RGB c) { return c.r + c.g + c.b; }

static void v2Tests(GPU& gpu) {
    std::printf("v2: length, button, flash, digit\n");
    const bool reversed = false;
    Targets t = makeTargets(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
    render_pass::StickRenderer r;
    check(r.initialize(gpu.queue.Get(), g_blobs), "renderer initialize");
    int fc = 0, fd = 0;
    auto layer = makeLayer(reversed, &fc, &fd);
    auto frame = [&](const selfie::RenderState& s, const char* what) {
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, s, what);
        return Image{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
    };
    for (int eye = 0; eye < 2; eye++) {
        // ---- length: the shaft is drawn up to the current length, the handle stays put ----
        const Pixel high = project(eye, 0, 0.9f + 1.3f, -0.8f + selfie::kShaftRadius);
        const Pixel mid = project(eye, 0, 0.9f + 0.6f, -0.8f + selfie::kShaftRadius);
        const Pixel handle = project(eye, 0, 0.9f + 0.14f, -0.8f + selfie::kHandleRadius);
        Image a = frame(makeState(0, 1.0f), "draw length 1.0");
        Image b = frame(makeState(0, 1.4f), "draw length 1.4");
        Image c = frame(makeState(0, 0.5f), "draw length 0.5");
        RGB bh = b.at(high);
        check(a.background(high) && !b.background(high) && bh.r > 120 && bh.g > 120 && bh.b > 120, "1.4 m: silver shaft at 1.3 m, absent at 1.0 m");
        check(!a.background(mid) && !b.background(mid) && c.background(mid), "0.6 m: shaft on the 1.0/1.4 m sticks, not on the 0.5 m one");
        RGB ha = a.at(handle), hb = b.at(handle), hc = c.at(handle);
        check(ha.r == hb.r && ha.g == hb.g && ha.b == hb.b && ha.r == hc.r, "handle identical at every length");
        for (float len : {0.5f, 1.0f, 1.4f}) {
            Image im = len == 0.5f ? c : len == 1.0f ? a : b;
            const float camY = 0.9f + len + 0.025f;
            const Pixel box = project(eye, 0.03f, camY - 0.02f, -0.8f + selfie::kCamBox[2] * 0.5f);
            RGB bx = im.at(box);
            check(!im.background(box) && bx.r < 45 && bx.g < 45 && bx.b < 45, "camera box at length + 0.025");
            const Pixel collar = project(eye, 0.014f, 0.9f + len - 0.006f, -0.8f + 0.005f);   // outside the shaft, inside the collar
            const Pixel aboveBox = project(eye, 0, camY + 0.08f, -0.8f);   // clear of the lens, which protrudes toward the (lower) eye
            check(!im.background(collar), "collar just below the camera head");
            if (aboveBox.y >= 0) check(im.background(aboveBox), "nothing above the camera box");
        }
        // ---- shutter button: red dome on the handle's +Z face, glowing when hot ----
        const Pixel btn = project(eye, 0, 0.9f + selfie::kShutterY, -0.8f + selfie::kHandleRadius - 0.002f + selfie::kShutterRadius);
        const Pixel btnEdge = project(eye, 0, 0.9f + selfie::kShutterY + selfie::kShutterRadius * 2.0f, -0.8f + selfie::kHandleRadius);
        Image cold = a;
        auto hotState = makeState(0, 1.0f); hotState.shutterHot = 1;
        Image hot = frame(hotState, "draw hot button");
        RGB bc = cold.at(btn), bhot = hot.at(btn);
        std::printf("  eye %d: button cold=%d,%d,%d hot=%d,%d,%d\n", eye, bc.r, bc.g, bc.b, bhot.r, bhot.g, bhot.b);
        check(!cold.background(btn) && bc.r > 90 && bc.r > bc.g + 60 && bc.r > bc.b + 60, "button drawn red");
        check(bhot.r >= 250 && bhot.g > 90 && bhot.g < 140 && bhot.b > 70 && bhot.b < 120, "hot button glows (emissive)");
        check(lum(bhot) > lum(bc) + 60, "glow is brighter");
        RGB e1 = cold.at(btnEdge), e2 = hot.at(btnEdge);
        check(e1.r == e2.r && e1.g == e2.g && e1.b == e2.b && e1.r < 80, "handle above the button unchanged by the glow");
        RGB hh = hot.at(handle);
        check(hh.r == ha.r && hh.g == ha.g, "hot flag does not touch the handle");
        // ---- flash: lens and ring whiten, the box does not ----
        const float camY = 0.9f + 1.0f + 0.025f;
        const float lensZ = -0.8f + selfie::kCamBox[2] * 0.5f + render_mesh::kRingDepth + 0.001f;
        const Pixel lens = project(eye, 0, camY, lensZ);
        const Pixel ring = project(eye, 0.016f, camY, lensZ - 0.001f);
        const Pixel box = project(eye, 0.03f, camY - 0.02f, -0.8f + selfie::kCamBox[2] * 0.5f);
        auto fl = makeState(0, 1.0f); fl.flash = 1.0f;
        Image full = frame(fl, "draw flash 1");
        fl.flash = 0.5f;
        Image half = frame(fl, "draw flash 0.5");
        RGB l0 = a.at(lens), l1 = full.at(lens), lh = half.at(lens), r1 = full.at(ring), b0 = a.at(box), b1 = full.at(box);
        std::printf("  eye %d: lens flash0=%d,%d,%d flash.5=%d,%d,%d flash1=%d,%d,%d ring1=%d,%d,%d\n", eye, l0.r, l0.g, l0.b, lh.r, lh.g, lh.b, l1.r, l1.g, l1.b, r1.r, r1.g, r1.b);
        check(l1.r >= 250 && l1.g >= 250 && l1.b >= 250 && r1.r >= 250 && r1.g >= 250 && r1.b >= 250, "flash 1: lens and ring white");
        check(lum(lh) > lum(l0) + 150 && lum(lh) < lum(l1) - 30, "flash 0.5: in between");
        check(b0.r == b1.r && b0.g == b1.g && b0.b == b1.b, "flash leaves the camera box alone");
        auto neg = makeState(0, 1.0f); neg.flash = std::numeric_limits<float>::quiet_NaN();
        Image nf = frame(neg, "draw NaN flash");
        RGB ln = nf.at(lens);
        check(ln.r == l0.r && ln.g == l0.g && ln.b == l0.b, "NaN flash ignored");
    }

    // ---- self-timer digit: blended, depth-tested, no depth write ----
    const float dz = -0.35f, dy = 1.6f;
    const float segX = render_mesh::kDigitW * 0.5f - render_mesh::kDigitT * 0.5f;   // b / f centre x
    const float upperY = dy + render_mesh::kDigitH * 0.25f;
    const float topY = dy + render_mesh::kDigitH * 0.5f - render_mesh::kDigitT * 0.5f;
    auto digitState = [&](int d, float alpha) {
        auto s = makeState(0, 1.0f);
        s.stick.p.x = 0.4f; s.camHead.p.x = 0.4f;   // stick off to the side
        s.countdownDigit = d; s.countdownAlpha = alpha;
        s.countdownPose = {{0, 0, 0, 1}, {0, dy, dz}};   // +Z toward the viewer
        return s;
    };
    for (int eye = 0; eye < 2; eye++) {
        const Pixel b = project(eye, segX, upperY, dz), f = project(eye, -segX, upperY, dz), a = project(eye, 0, topY, dz);
        const Pixel outline = project(eye, segX + render_mesh::kDigitT * 0.5f + render_mesh::kOutline * 0.6f, upperY, dz);
        const Pixel centre = project(eye, 0, dy, dz);
        check(b.x > f.x, "segment b is on the viewer's right");
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(1, 1.0f), "draw digit 1");
        auto raw1 = gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Image one{raw1, false};
        auto depth1 = gpu.read(t.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        RGB w = one.at(b), o = one.at(outline);
        std::printf("  eye %d: digit1 b=%d,%d,%d outline=%d,%d,%d f=%d,%d,%d\n", eye, w.r, w.g, w.b, o.r, o.g, o.b, one.at(f).r, one.at(f).g, one.at(f).b);
        check(w.r >= 250 && w.g >= 250 && w.b >= 250, "digit 1: segment b white at alpha 1");
        check(one.background(f) && one.background(a), "digit 1: segments f and a not drawn");
        check(o.r < 20 && o.b < 20 && o.g > 120 && o.g < 200, "dark outline (65% black over green) beside the segment");
        check(depthValue(depth1, b, DXGI_FORMAT_R24G8_TYPELESS) == 1.0f, "digit writes no depth");
        check((raw1[size_t(b.y) * kTexW + b.x] >> 24) == 255, "destination alpha kept");
       check(one.background(centre), "digit 1 has no middle segment");

        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(3, 1.0f), "draw digit 3");
        Image three{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        check(!three.background(a) && !three.background(b) && !three.background(centre) && three.background(f), "digit 3: a, b, g lit; f dark");
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(2, 1.0f), "draw digit 2");
        Image two{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        const Pixel e = project(eye, -segX, dy - render_mesh::kDigitH * 0.25f, dz), cseg = project(eye, segX, dy - render_mesh::kDigitH * 0.25f, dz);
        check(!two.background(e) && two.background(cseg) && !two.background(b), "digit 2: e lit, c dark, b lit");

        // alpha fades the digit over Echo's image
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(1, 0.5f), "draw digit alpha 0.5");
        Image halfA{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        RGB hw = halfA.at(b);
        std::printf("  eye %d: digit alpha .5 = %d,%d,%d\n", eye, hw.r, hw.g, hw.b);
        // linear: outline first (green * (1 - 0.65*0.5) = 0.675), then white at 0.5 -> r=b=0.5 (sRGB 188), g=0.8375 (sRGB 236)
        check(hw.g > 228 && hw.g < 244 && hw.r > 180 && hw.r < 196 && std::abs(hw.r - hw.b) <= 2, "alpha 0.5: half white over the half-faded outline (linear blend)");
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(1, 0.1f), "draw digit alpha 0.1");
        Image faint{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        RGB fw = faint.at(b);
        check(fw.r > 0 && fw.r < hw.r - 60, "alpha 0.1: faint");
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(1, 0.0f), "draw digit alpha 0");
        Image none{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        check(none.background(b) && none.background(outline), "alpha 0: nothing");
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, digitState(0, 1.0f), "draw no countdown");
        Image off{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        check(off.background(b) && off.background(centre), "digit 0: nothing");

        // depth test: nearer Echo geometry hides the digit; farther geometry does not
        D3D12_RECT occ = {b.x - 6, b.y - 6, b.x + 6, b.y + 6};
        echoFrame(gpu, t, reversed, &occ, depthAt(reversed, 0.3f));
        drawOk(r, gpu, t, layer, digitState(1, 1.0f), "draw digit behind occluder");
        Image hid{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        check(hid.background(b), "nearer Echo depth hides the digit");
        echoFrame(gpu, t, reversed, &occ, depthAt(reversed, 2.0f));
        drawOk(r, gpu, t, layer, digitState(1, 1.0f), "draw digit before far geometry");
        Image shown{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        check(!shown.background(b), "farther Echo geometry does not hide the digit");

        // the stick is still drawn with a digit, and a broken digit pose never costs the stick
        auto broken = digitState(1, 1.0f);
        broken.countdownPose.q.x = std::numeric_limits<float>::quiet_NaN();
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, broken, "draw with NaN digit pose");
        Image br{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        const Pixel stickShaft = project(eye, 0.4f, 0.9f + 0.6f, -0.8f + selfie::kShaftRadius);
        check(br.background(b) && !br.background(stickShaft), "NaN digit pose: no digit, stick drawn");
    }
    // a digit turned away (+Z away from the viewer) is still visible (double sided) but mirrored: b on the left
    {
        auto s = digitState(1, 1.0f);
        s.countdownPose.q = {0, 1, 0, 0};
        echoFrame(gpu, t, reversed);
        drawOk(r, gpu, t, layer, s, "draw digit from behind");
        Image back{gpu.read(t.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), false};
        check(!back.background(project(0, -segX, upperY, dz)) && back.background(project(0, segX, upperY, dz)), "billboard orientation matters (mirrored from behind)");
    }
    check(r.waitIdle(5000), "renderer idle");
    check(gpu.debugErrors() == 0, "no D3D12 debug-layer errors in v2 tests");
}

static void queueValidationTests(GPU& gpu) {
    std::printf("queue validation\n");
    using selfie_render::detail::ValidateQueue;
    ID3D12CommandQueue* q = nullptr; const char* why = nullptr;
    check(ValidateQueue(reinterpret_cast<uintptr_t>(gpu.queue.Get()), gpu.device.Get(), &q, &why) && q == gpu.queue.Get(), "real direct queue accepted");
    q->Release();
    check(ValidateQueue(reinterpret_cast<uintptr_t>(gpu.queue.Get()), nullptr, &q, &why), "accepted without a device to compare"); q->Release();
    auto reject = [&](uintptr_t candidate, ID3D12Device* dev, const char* expectWhy, const char* what) {
        ID3D12CommandQueue* out = reinterpret_cast<ID3D12CommandQueue*>(1); const char* w = nullptr;
        bool ok = ValidateQueue(candidate, dev, &out, &w);
        if (ok || !w || std::strcmp(w, expectWhy) != 0) std::printf("  %s: ok=%d why=%s\n", what, ok, w ? w : "-");
        check(!ok && out == nullptr && w && std::strcmp(w, expectWhy) == 0, what);
    };
    reject(0, gpu.device.Get(), "queue-null-or-misaligned", "null rejected");
    reject(reinterpret_cast<uintptr_t>(gpu.queue.Get()) + 1, gpu.device.Get(), "queue-null-or-misaligned", "misaligned rejected");
    reject(0x10000, gpu.device.Get(), "queue-unreadable", "unmapped address rejected without faulting");
    std::vector<uintptr_t> heapJunk(16, 0x12345678);
    reject(reinterpret_cast<uintptr_t>(heapJunk.data()), gpu.device.Get(), "queue-vtable-not-in-image", "heap junk rejected");
    static const char rdata[] = "not a vtable, just read-only data in the image....";
    uintptr_t fakeObject[2] = {reinterpret_cast<uintptr_t>(rdata), 0};
    reject(reinterpret_cast<uintptr_t>(fakeObject), gpu.device.Get(), "queue-vtable-entry-not-code", "data-only fake vtable rejected");
    reject(reinterpret_cast<uintptr_t>(gpu.device.Get()), gpu.device.Get(), "queue-qi-failed", "device (not a queue) rejected");
    ComPtr<ID3D12CommandQueue> copyQueue;
    D3D12_COMMAND_QUEUE_DESC cd = {}; cd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    check(SUCCEEDED(gpu.device->CreateCommandQueue(&cd, IID_PPV_ARGS(&copyQueue))), "copy queue");
    reject(reinterpret_cast<uintptr_t>(copyQueue.Get()), gpu.device.Get(), "queue-not-direct", "copy queue rejected");
    // Device identity: D3D12 returns ONE device per adapter, so the "other" device must come from another adapter.
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> hw; ComPtr<ID3D12Device> otherDevice;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        for (UINT i = 0; factory->EnumAdapters1(i, &hw) != DXGI_ERROR_NOT_FOUND; i++, hw.Reset()) {
            DXGI_ADAPTER_DESC1 ad; hw->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(hw.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice)))) break;
        }
    if (otherDevice && otherDevice.Get() != gpu.device.Get()) {
        reject(reinterpret_cast<uintptr_t>(gpu.queue.Get()), otherDevice.Get(), "queue-device-mismatch", "queue of another device rejected");
    } else {
        std::printf("  (no second adapter: device-mismatch case not exercised)\n");
    }
}

static void installTests() {
    std::printf("install (no LibOVR in this process)\n");
    check(MH_Initialize() == MH_OK, "MinHook init (core does this in the DLL)");
    check(!selfie_render::Install(), "Install refuses without LibOVR");
    const char* status = selfie_render::Status();
    std::printf("  %s\n", status);
    check(std::strstr(status, "libovr-not-loaded") != nullptr, "status explains why");
    selfie_render::Uninstall();  // no-op, must not crash
    check(g_stubTicks == 0, "no ticks without a hook");
    check(selfie_render::EchoQueue() == nullptr, "EchoQueue is null until Echo's queue was validated");
    MH_Uninitialize();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // keep output when a test crashes
    const char* why = nullptr;
    check(render_pass::compileShaders(g_blobs, &why), why ? why : "shader compile");
    layerTests();
    mathAndMeshTests();
    GPU gpu(true);
    std::printf("debug layer: %s\n", gpu.info ? "on" : "unavailable (install Graphics Tools for state validation)");
    sceneTests(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, false, "R8G8B8A8_TYPELESS + R24G8 (Echo)");
    sceneTests(gpu, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, true, "R8G8B8A8_TYPELESS + R24G8");
    sceneTests(gpu, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, false, "B8G8R8A8_SRGB + R32");
    v2Tests(gpu);
    rejectionTests(gpu);
    nonStallTests(gpu);
    queueValidationTests(gpu);
    installTests();
    check(SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "device healthy at the end");
    std::printf("PASS: %d checks (stereo draw into Echo-style colour+depth, Echo projection depth, occlusion, untouched pixels, "
                "highlight, v2 length/button/flash/self-timer digit, rejections, fence-gated non-stall, layer parsing, queue validation, install refusal)\n", g_checks);
    return 0;
}
