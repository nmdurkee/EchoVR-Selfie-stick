// render_pass.h - records the selfie stick into a host colour + depth pair on the host's queue.
// Runtime-independent: exercised directly by render_tests.cpp on a WARP device.
//
// Pattern adapted from EchoCraft's HostRenderer (MinecraftEchoVR/native/live_world.h), which
// does exactly this inside echovr.exe in production:
//   * same device + same queue as Echo (an independent device is rejected by LibOVR, -1005);
//   * the eye images arrive and leave in PIXEL_SHADER_RESOURCE (colour and depth);
//   * depth-tested AND depth-written with Echo's own projection row, so Echo's hands/avatar
//     occlude the stick and vice versa; reversed-Z auto-detected from that row;
//   * three command-list slots, each gated by a fence value: a slot whose previous work has
//     not retired is SKIPPED (never waited on), so Echo's render thread never stalls.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "selfie_shared.h"
#include "render_math.h"
#include "render_layer.h"
#include "render_mesh.h"

namespace render_pass {
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------------
// Shaders (compiled at runtime through d3dcompiler_47.dll, resolved dynamically so the DLL has no
// hard import on it). Root constants (36 DWORDs, b0):
//   clip (row-major 4x4), rot0..rot2 (object->tracking rotation rows, for normals; rot0.w = the
//   7-segment mask for digit draws), light (tracking-space unit vector toward the light),
//   params (x = highlight, y = shutterHot, z = flash, w = digit alpha).
// Vertex colour alpha is a part flag (render_mesh.h): 1 handle, 2 shutter dome, 3 lens, 10+s digit
// segment s (white), 20+s its outline. Digit vertices whose segment is not in the mask collapse to
// one point outside the clip volume (zero-area, nothing rasterized).
// ---------------------------------------------------------------------------------------------
static const char kShaderSource[] = R"(
cbuffer Draw : register(b0) {
    row_major float4x4 clip;
    float4 rot0; float4 rot1; float4 rot2;
    float4 light;
    float4 params;
};
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; float3 col : COLOR; nointerpolation float flag : FLAG; };
VSOut vs(VSIn i) {
    VSOut o;
    o.pos = mul(clip, float4(i.pos, 1));
    o.nrm = float3(dot(rot0.xyz, i.nrm), dot(rot1.xyz, i.nrm), dot(rot2.xyz, i.nrm));
    float3 lit = float3(0.200, 0.200, 0.215);
    int f = (int)(i.col.a + 0.5);
    o.col = (f == 1 && params.x > 0.5) ? lit : i.col.rgb;
    o.flag = (float)f;
    if (f >= 10) {
        uint seg = (uint)(f % 10);
        uint mask = (uint)(rot0.w + 0.5);
        if (((mask >> seg) & 1u) == 0u) o.pos = float4(2, 2, 2, 1);
    }
    return o;
}
float4 ps(VSOut i) : SV_TARGET {
    int f = (int)(i.flag + 0.5);
    if (f >= 10) return float4(i.col, saturate(params.w) * (f >= 20 ? 0.65 : 1.0));
    if (f == 2 && params.y > 0.5) return float4(1.000, 0.180, 0.120, 1);
    float3 n = normalize(i.nrm);
    float d = saturate(dot(n, light.xyz));
    float3 c = i.col * (0.40 + 0.60 * d);
    if (f == 3) c = lerp(c, float3(1, 1, 1), saturate(params.z));
    return float4(c, 1);
}
)";

struct ShaderBlobs { ComPtr<ID3DBlob> vs, ps; };

// Compiles both shaders. Safe to call off the render thread (no device needed).
inline bool compileShaders(ShaderBlobs& out, const char** why) {
    using Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compiler) { if (why) *why = "d3dcompiler-missing"; return false; }
    auto compile = reinterpret_cast<Compile>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) { if (why) *why = "d3dcompile-missing"; return false; }
    ShaderBlobs b;
    ComPtr<ID3DBlob> error;
    if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "selfie_stick", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b.vs, &error)) || !b.vs) {
        if (why) *why = "vs-compile-failed";
        return false;
    }
    error.Reset();
    if (FAILED(compile(kShaderSource, sizeof(kShaderSource) - 1, "selfie_stick", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &b.ps, &error)) || !b.ps) {
        if (why) *why = "ps-compile-failed";
        return false;
    }
    out = b;
    return true;
}

// Render-target / depth view formats we accept (EchoCraft: typeless colour -> sRGB view, R24G8 -> D24S8).
inline DXGI_FORMAT colorView(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM: return f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT depthView(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Per-draw root constants.
struct DrawConstants {
    float clip[16];
    float rot[3][4];
    float light[4];
    float params[4];
};
static_assert(sizeof(DrawConstants) == 36 * 4, "root constant count");

// Everything one frame needs, computed on the CPU (no GPU objects). Exposed for tests.
enum Part { kPartFixed = 0, kPartShaft = 1, kPartTop = 2, kPartCam = 3, kPartDigit = 4, kParts = 5 };
struct FramePlan {
    DrawConstants draws[2][kParts];  // [eye][part]
    D3D12_VIEWPORT viewport[2];
    D3D12_RECT scissor[2];
    bool reversed = false;
    bool digit = false;              // draw the self-timer digit (blended pass)
    float length = selfie::kStickLength;
};

inline void rotationRows(const render_math::Affine& a, float rot[3][4]) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) rot[i][j] = float(a.r[i][j]); rot[i][3] = 0; }
}

// The shaft length to draw: RenderState.length when it is a sane value, else the default.
inline float drawLength(float l) {
    if (!std::isfinite(l) || l < selfie::kStickMinLen - 1e-3f || l > selfie::kStickMaxLen + 1e-3f) return selfie::kStickLength;
    return l;
}

// object * T(0, ty, 0) * S(1, sy, 1)  (the unit shaft and the top collar).
inline render_math::Affine stretchY(const render_math::Affine& object, double ty, double sy) {
    render_math::Affine s;
    s.r[1][1] = sy;
    s.t[1] = ty;
    return render_math::compose(object, s);
}

// Builds per-eye matrices. Returns nullptr on success or a short reason.
inline const char* planFrame(const render_layer::EchoLayer& layer, const selfie::RenderState& state, unsigned texWidth, unsigned texHeight, FramePlan& plan) {
    if (!render_layer::validLayer(layer)) return "layer-invalid";
    render_math::Affine stick, cam, digit;
    if (!render_math::fromPose(&state.stick.q.x, &state.stick.p.x, stick)) return "stick-pose-invalid";
    if (!render_math::fromPose(&state.camHead.q.x, &state.camHead.p.x, cam)) return "camhead-pose-invalid";
    const float length = drawLength(state.length);
    plan.length = length;
    const float shaftTop = length - render_mesh::kCollarH;
    const render_math::Affine shaft = stretchY(stick, render_mesh::kShaftBottom, shaftTop - render_mesh::kShaftBottom);
    const render_math::Affine top = stretchY(stick, length, 1.0);
    const unsigned mask = render_mesh::digitMask(state.countdownDigit);
    plan.digit = mask != 0 && std::isfinite(state.countdownAlpha) && state.countdownAlpha > 0.004f &&
                 render_math::fromPose(&state.countdownPose.q.x, &state.countdownPose.p.x, digit);
    // Light: fixed in head space (up and behind the viewer), expressed in tracking space.
    render_math::Affine head;
    if (!render_math::fromPose(layer.pose[0].orientation, layer.pose[0].position, head)) return "eye-pose-invalid";
    const double l0[3] = {0.30, 0.80, 0.52};
    const double ln = std::sqrt(l0[0] * l0[0] + l0[1] * l0[1] + l0[2] * l0[2]);
    float light[4];
    for (int i = 0; i < 3; i++) light[i] = float((head.r[i][0] * l0[0] + head.r[i][1] * l0[1] + head.r[i][2] * l0[2]) / ln);
    light[3] = 0;
    bool reversed = false;
    for (int e = 0; e < 2; e++) {
        auto p = render_math::projection(layer.fov[e].up, layer.fov[e].down, layer.fov[e].left, layer.fov[e].right, layer.p22, layer.p23, layer.p32);
        if (!p.valid) return "projection-invalid";
        if (e == 0) reversed = p.reversed;
        else if (p.reversed != reversed) return "projection-mismatch";
        render_math::Affine eye;
        if (!render_math::fromPose(layer.pose[e].orientation, layer.pose[e].position, eye)) return "eye-pose-invalid";
        const render_math::Affine* objects[kParts] = {&stick, &shaft, &top, &cam, &digit};
        const render_math::Affine* normals[kParts] = {&stick, &stick, &stick, &cam, &digit};   // unscaled rotations
        const float flash = std::isfinite(state.flash) ? std::fmin(std::fmax(state.flash, 0.f), 1.f) : 0.f;
        for (int part = 0; part < kParts; part++) {
            DrawConstants& d = plan.draws[e][part];
            if (part == kPartDigit && !plan.digit) { std::memset(&d, 0, sizeof(d)); continue; }
            if (!render_math::clipFromObject(p, eye, *objects[part], d.clip)) {
                if (part == kPartDigit) { plan.digit = false; std::memset(&d, 0, sizeof(d)); continue; }
                return "matrix-nonfinite";
            }
            rotationRows(*normals[part], d.rot);
            std::memcpy(d.light, light, sizeof(light));
            d.params[0] = state.highlight ? 1.f : 0.f;
            d.params[1] = state.shutterHot ? 1.f : 0.f;
            d.params[2] = flash;
            d.params[3] = 0;
            if (part == kPartDigit) {
                d.rot[0][3] = float(mask);
                d.params[3] = std::fmin(state.countdownAlpha, 1.f);
            }
        }
        const auto& r = layer.viewport[e];
        if (unsigned(r.x) + unsigned(r.width) > texWidth || unsigned(r.y) + unsigned(r.height) > texHeight) return "viewport-outside-texture";
        plan.viewport[e] = {float(r.x), float(r.y), float(r.width), float(r.height), 0.f, 1.f};
        plan.scissor[e] = {LONG(r.x), LONG(r.y), LONG(r.x + r.width), LONG(r.y + r.height)};
    }
    plan.reversed = reversed;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// GPU part. One instance per process, used only from Echo's render thread (inside the commit hook).
// ---------------------------------------------------------------------------------------------
class StickRenderer {
public:
    enum Result { Drawn = 0, Busy = 1, Skipped = 2, Failed = -1 };
    const char* error = "not-initialized";
    std::atomic<unsigned long long> frames{0}, busy{0};
    std::atomic<int> lastColorFormat{0}, lastDepthFormat{0}, lastReversed{-1};

    // queue: Echo's own direct queue. blobs: compiled shaders. Creates only small objects
    // (allocators, lists, fence, tiny descriptor heaps, root signature, one upload-heap vertex buffer).
    bool initialize(ID3D12CommandQueue* q, const ShaderBlobs& blobs) {
        if (!q || !blobs.vs || !blobs.ps) { error = "init-bad-args"; return false; }
        queue = q;
        shaders = blobs;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) { error = "init-get-device"; return false; }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { error = "init-fence"; return false; }
        for (auto& s : slots) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list))) ||
                FAILED(s.list->Close())) { error = "init-command-list"; return false; }
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = kSlots;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)))) { error = "init-rtv-heap"; return false; }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvs)))) { error = "init-dsv-heap"; return false; }
        rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!createRootSignature()) return false;
        // Static mesh in an upload-heap buffer, written once (a few hundred vertices; no copy pass needed).
        mesh = render_mesh::build();
        const UINT64 bytes = UINT64(mesh.vertices.size()) * sizeof(render_mesh::Vertex);
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer)))) {
            error = "init-vertex-buffer"; return false;
        }
        void* mapped = nullptr; D3D12_RANGE none = {0, 0};
        if (FAILED(vertexBuffer->Map(0, &none, &mapped)) || !mapped) { error = "init-vertex-map"; return false; }
        std::memcpy(mapped, mesh.vertices.data(), size_t(bytes));
        vertexBuffer->Unmap(0, nullptr);
        vbView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vbView.SizeInBytes = UINT(bytes);
        vbView.StrideInBytes = sizeof(render_mesh::Vertex);
        ready = true;
        error = nullptr;
        return true;
    }
    bool initialized() const { return ready; }
    // Releases every GPU object. Call only after waitIdle() succeeded (teardown).
    void reset() {
        ready = false;
        for (auto& s : slots) s = Slot{};
        pipeline.Reset(); blendPipeline.Reset(); root.Reset(); rtvs.Reset(); dsvs.Reset(); vertexBuffer.Reset();
        fence.Reset(); queue.Reset(); device.Reset();
        next = 0; serial = 0; vbView = {};
        pipeColor = pipeDepth = DXGI_FORMAT_UNKNOWN;
        error = "not-initialized";
    }
    ID3D12Device* deviceHandle() const { return device.Get(); }
    bool idle() const { return !fence || fence->GetCompletedValue() >= serial; }
    // Teardown only (never per frame).
    bool waitIdle(DWORD ms) {
        if (idle()) return true;
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!e) return false;
        bool ok = SUCCEEDED(fence->SetEventOnCompletion(serial, e)) && WaitForSingleObject(e, ms) == WAIT_OBJECT_0;
        CloseHandle(e);
        return ok;
    }

    // Records and submits one frame. color/depth must be in PIXEL_SHADER_RESOURCE (Echo's hand-off
    // state) and are returned in it. Never blocks. Failed => caller should disable rendering.
    Result draw(ID3D12Resource* color, ID3D12Resource* depth, const render_layer::EchoLayer& layer, const selfie::RenderState& state,
                D3D12_RESOURCE_STATES colorHandoff = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATES depthHandoff = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        if (!ready) { error = "not-initialized"; return Failed; }
        if (!state.visible) return Skipped;
        if (!color || !depth) { error = "no-buffers"; return Skipped; }
        const auto cd = color->GetDesc(), dd = depth->GetDesc();
        const DXGI_FORMAT cf = colorView(cd.Format), df = depthView(dd.Format);
        lastColorFormat = int(cd.Format); lastDepthFormat = int(dd.Format);
        if (cf == DXGI_FORMAT_UNKNOWN) { error = "unsupported-color-format"; return Skipped; }
        if (df == DXGI_FORMAT_UNKNOWN) { error = "unsupported-depth-format"; return Skipped; }
        if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            cd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1 || cd.Width != dd.Width || cd.Height != dd.Height ||
            cd.DepthOrArraySize != 1 || dd.DepthOrArraySize != 1 || cd.Width > 16384 || cd.Height > 16384) { error = "color-depth-shape-mismatch"; return Skipped; }
        if (!(cd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || !(dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) { error = "buffers-not-bindable"; return Skipped; }
        FramePlan plan;
        if (const char* why = planFrame(layer, state, unsigned(cd.Width), cd.Height, plan)) { error = why; return Skipped; }
        lastReversed = plan.reversed ? 1 : 0;
        // Pipeline for this (colour, depth, depth-direction). Rebuilt only when it changes, and only
        // once every earlier draw retired (the old PSO may still be referenced by in-flight lists).
        if (!pipeline || !blendPipeline || pipeColor != cf || pipeDepth != df || pipeReversed != plan.reversed) {
            if (!idle()) { busy++; return Busy; }
            pipeline.Reset();
            blendPipeline.Reset();
            if (!createPipeline(cf, df, plan.reversed)) return Failed;
        }
        Slot& slot = slots[next];
        if (fence->GetCompletedValue() < slot.value) { busy++; return Busy; }  // never stall Echo's render thread
        slot.color.Reset(); slot.depth.Reset();                                 // previous frame's buffers retired
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), pipeline.Get()))) { error = "command-list-reset"; return Failed; }
        ID3D12GraphicsCommandList* list = slot.list.Get();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(next) * rtvStride;
        dsv.ptr += SIZE_T(next) * dsvStride;
        D3D12_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = cf; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(color, &rd, rtv);
        D3D12_DEPTH_STENCIL_VIEW_DESC ddsc = {}; ddsc.Format = df; ddsc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth, &ddsc, dsv);
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (auto& x : b) { x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE; }
        b[0].Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, colorHandoff, D3D12_RESOURCE_STATE_RENDER_TARGET};
        b[1].Transition = {depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, depthHandoff, D3D12_RESOURCE_STATE_DEPTH_WRITE};
        list->ResourceBarrier(2, b);
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->SetGraphicsRootSignature(root.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbView);
        const render_mesh::Range* ranges[4] = {&mesh.fixed, &mesh.shaft, &mesh.top, &mesh.cam};
        for (int e = 0; e < 2; e++) {
            list->RSSetViewports(1, &plan.viewport[e]);
            list->RSSetScissorRects(1, &plan.scissor[e]);
            for (int part = 0; part < 4; part++) {
                list->SetGraphicsRoot32BitConstants(0, 36, &plan.draws[e][part], 0);
                list->DrawInstanced(ranges[part]->count, 1, ranges[part]->first, 0);
            }
        }
        // Self-timer digit: alpha-blended, depth-tested, no depth write; after all opaque geometry.
        if (plan.digit) {
            list->SetPipelineState(blendPipeline.Get());
            for (int e = 0; e < 2; e++) {
                list->RSSetViewports(1, &plan.viewport[e]);
                list->RSSetScissorRects(1, &plan.scissor[e]);
                list->SetGraphicsRoot32BitConstants(0, 36, &plan.draws[e][kPartDigit], 0);
                list->DrawInstanced(mesh.digit.count, 1, mesh.digit.first, 0);
            }
        }
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        list->ResourceBarrier(2, b);
        if (FAILED(list->Close())) { error = "command-list-close"; return Failed; }
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++serial))) { error = "fence-signal"; return Failed; }
        slot.value = serial;
        slot.color = color; slot.depth = depth;  // keep alive until this slot's fence retires
        next = (next + 1) % kSlots;
        frames++;
        if (FAILED(device->GetDeviceRemovedReason())) { error = "device-removed"; return Failed; }
        error = nullptr;
        return Drawn;
    }

private:
    static constexpr unsigned kSlots = 3;
    struct Slot { ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; UINT64 value = 0; ComPtr<ID3D12Resource> color, depth; };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    Slot slots[kSlots];
    unsigned next = 0;
    UINT64 serial = 0;
    ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
    UINT rtvStride = 0, dsvStride = 0;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline, blendPipeline;
    DXGI_FORMAT pipeColor = DXGI_FORMAT_UNKNOWN, pipeDepth = DXGI_FORMAT_UNKNOWN;
    bool pipeReversed = false;
    ShaderBlobs shaders;
    render_mesh::Mesh mesh;
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    bool ready = false;

    bool createRootSignature() {
        using Serialize = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        auto serialize = d3d12 ? reinterpret_cast<Serialize>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
        if (!serialize) { error = "init-no-serializer"; return false; }
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = 36;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 1; desc.pParameters = &param;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || !blob) { error = "init-root-serialize"; return false; }
        if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) { error = "init-root-signature"; return false; }
        return true;
    }
    bool createPipeline(DXGI_FORMAT cf, DXGI_FORMAT df, bool reversed) {
        D3D12_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = {};
        p.pRootSignature = root.Get();
        p.VS = {shaders.vs->GetBufferPointer(), shaders.vs->GetBufferSize()};
        p.PS = {shaders.ps->GetBufferPointer(), shaders.ps->GetBufferSize()};
        auto& blend = p.BlendState.RenderTarget[0];
        blend.BlendEnable = FALSE;
        blend.SrcBlend = D3D12_BLEND_ONE; blend.DestBlend = D3D12_BLEND_ZERO; blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthEnable = TRUE;
        p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        p.DepthStencilState.DepthFunc = reversed ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
        p.DepthStencilState.StencilEnable = FALSE;
        p.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
        p.InputLayout = {elements, 3};
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = cf;
        p.DSVFormat = df;
        p.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)))) { error = "pipeline-create-failed"; return false; }
        // Blended variant for the self-timer digit: src-alpha over, destination alpha kept, depth test only.
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO; blend.DestBlendAlpha = D3D12_BLEND_ONE; blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&blendPipeline)))) { pipeline.Reset(); error = "blend-pipeline-create-failed"; return false; }
        pipeColor = cf; pipeDepth = df; pipeReversed = reversed;
        return true;
    }
};

}  // namespace render_pass
