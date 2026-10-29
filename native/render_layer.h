// render_layer.h - Echo's EyeFovDepth layer header, read straight from Echo's globals.
// Adapted from EchoCraft (MinecraftEchoVR/native/live_world.h + ovr_observation.h).
//
// echovr.exe (sha256 3dae0cda...df9a) keeps its single EyeFovDepth layer at RVA 0x2022558
// (136-byte header ABI): colour chains +136, viewports +152, FOV +184, render poses +216,
// depth chains +280, ovrTimewarpProjectionDesc (P22,P23,P32) +296. Type at +0 is 2
// (ovrLayerType_EyeFovDepth). Echo commits colour, then depth, then calls ovr_EndFrame
// (fcn 0x14072af60). Both eyes live in colour chain [0] side by side (viewports).
#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace render_layer {

constexpr uintptr_t kLayerRva = 0x2022558;
constexpr size_t kLayerBytes = 308;

struct Rect { int x, y, width, height; };
struct Fov { float up, down, left, right; };
struct Pose { float orientation[4]; float position[3]; };
static_assert(sizeof(Rect) == 16 && sizeof(Fov) == 16 && sizeof(Pose) == 28, "LibOVR ABI");

struct EchoLayer {
    void* color[2] = {};
    void* depth[2] = {};
    Rect viewport[2] = {};
    Fov fov[2] = {};
    Pose pose[2] = {};
    float p22 = 0, p23 = 0, p32 = 0;
};

// Validates everything a draw depends on. Returns false on any unexpected value.
inline bool validLayer(const EchoLayer& l) {
    if (!l.color[0] || !l.depth[0]) return false;
    // Both eyes must be in chain [0] (EchoCraft-verified); a distinct second chain is unexpected.
    if (l.color[1] && l.color[1] != l.color[0]) return false;
    if (l.depth[1] && l.depth[1] != l.depth[0]) return false;
    if (l.color[0] == l.depth[0]) return false;
    for (int i = 0; i < 2; i++) {
        const Rect& r = l.viewport[i];
        if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 || r.x > 16384 - r.width || r.y > 16384 - r.height) return false;
        const float f[] = {l.fov[i].up, l.fov[i].down, l.fov[i].left, l.fov[i].right};
        for (float v : f) if (!std::isfinite(v) || v <= 0 || v > 10) return false;
        float norm = 0;
        for (float v : l.pose[i].orientation) { if (!std::isfinite(v)) return false; norm += v * v; }
        if (norm < .9f || norm > 1.1f) return false;
        for (float v : l.pose[i].position) if (!std::isfinite(v) || std::fabs(v) > 1.0e5f) return false;
    }
    if (!std::isfinite(l.p22) || !std::isfinite(l.p23) || !std::isfinite(l.p32)) return false;
    return true;
}

inline bool parseLayer(const unsigned char* raw, size_t size, EchoLayer& out) {
    if (!raw || size < kLayerBytes) return false;
    int type;
    std::memcpy(&type, raw, 4);
    if (type != 2) return false;  // ovrLayerType_EyeFovDepth
    EchoLayer l;
    std::memcpy(l.color, raw + 136, 16);
    std::memcpy(l.viewport, raw + 152, 32);
    std::memcpy(l.fov, raw + 184, 32);
    std::memcpy(l.pose, raw + 216, 56);
    std::memcpy(l.depth, raw + 280, 16);
    std::memcpy(&l.p22, raw + 296, 4);
    std::memcpy(&l.p23, raw + 300, 4);
    std::memcpy(&l.p32, raw + 304, 4);
    if (!validLayer(l)) return false;
    out = l;
    return true;
}

// Fault-safe read (ReadProcessMemory never faults on a bad address).
inline bool readLayer(uintptr_t address, EchoLayer& out) {
    if (!address) return false;
    unsigned char raw[kLayerBytes] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), raw, sizeof(raw), &got) || got != sizeof(raw))
        return false;
    return parseLayer(raw, sizeof(raw), out);
}

inline uintptr_t readPointer(uintptr_t address) {
    uintptr_t value = 0;
    SIZE_T got = 0;
    if (!address) return 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(value), &got) && got == sizeof(value) ? value : 0;
}

}  // namespace render_layer
