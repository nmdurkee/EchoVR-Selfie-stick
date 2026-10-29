// selfie_render.h - render module of SelfieStick.dll (draws the stick into Echo's own eye buffers).
// Public API (Install / Uninstall / Status) is declared by the contract in selfie_shared.h.
// The pieces live in render_layer.h (Echo layer header), render_math.h (projection),
// render_mesh.h (procedural mesh) and render_pass.h (D3D12 recording). See ../DESIGN.md §4.
//
// What render expects from core's RenderState (all raw LibOVR tracking space, like the eye poses):
//   stick   : stick-local -> tracking. Origin = BUTT of the handle (y=0), +Y up the shaft.
//   camHead : camera-head-local -> tracking. Origin = CENTRE of the camera box (stick-local
//             (0, length + 0.025, 0) when not re-aimed); lens on local +Z (toward the user).
//   v2/v2.1 : length (shaft drawn to it; 0/NaN/out of range -> kStickLength), shutterHot (dome glows),
//             flash (lens + ring whiten), countdownDigit/Alpha/Pose (blended 7-segment digit, eye buffers only).
//   Both quaternions must be (near) unit length; non-finite or absurd poses skip the frame.
// Threading: selfie::Tick() and selfie::GetRenderState() are called on Echo's render thread from
// inside ovr_CommitTextureSwapChain, once per Echo frame (only for Echo's colour-chain commit). If the
// layer header cannot be read, Tick() is still called on every commit (up to 2x per frame), so core
// must derive dt from its own clock.
#pragma once
// d3d12.h BEFORE selfie_shared.h: the contract declares `struct ID3D12CommandQueue* EchoQueue()` inside
// namespace selfie_render; if ::ID3D12CommandQueue is not declared yet, that elaborated type would name a
// new selfie_render::ID3D12CommandQueue (and MSVC mangles the return type), so every TU that defines or
// calls EchoQueue must see d3d12.h first.
#include <windows.h>
#include <d3d12.h>
#include "selfie_shared.h"

namespace selfie_render {
namespace detail {
// RVA of Echo's D3D12 command queue pointer (the queue registered with LibOVR; EchoCraft's
// desktop mirror compares it against the queue captured from ovr_CreateTextureSwapChainDX).
constexpr uintptr_t kQueueRva = 0x20205a0;

// Validates a raw pointer that should be an ID3D12CommandQueue of type DIRECT on `expected`'s device,
// without trusting it: committed readable memory, vtable + first entries inside loaded executable
// images, QueryInterface under SEH, queue type and device identity. On success *out holds a reference.
bool ValidateQueue(uintptr_t candidate, ID3D12Device* expected, ID3D12CommandQueue** out, const char** why);
}  // namespace detail
}  // namespace selfie_render
