// render_mesh.h - procedural selfie-stick mesh (CPU side, built ONCE; never rebuilt per frame).
//
// Parts, each drawn with its own object transform (render_pass.h):
//   fixed part  : STICK-local (selfie_shared.h), +Y from the butt (y=0): handle cylinder and the red
//                 shutter dome at (0, kShutterY, kHandleRadius). Independent of the stick length.
//   shaft part  : a UNIT shaft, y in [0, 1], open ends. Drawn with stick * T(0, y0, 0) * S(1, y1 - y0, 1),
//                 y0 = kShaftBottom, y1 = length - kCollarH, so the shaft follows RenderState.length
//                 (side normals have n.y = 0, so the Y scale leaves them unchanged).
//   top part    : mount collar, y in [-kCollarH, 0]; drawn with stick * T(0, length, 0).
//   camera part : CAMERA-HEAD-local, origin = centre of the camera box (stick-local
//                 (0, length + 0.025, 0)), lens on local +Z. Black box kCamBox, blue lens ring, dark glass.
//   digit part  : 7-segment self-timer digit in DIGIT-local space (origin = digit centre, +Y up,
//                 +Z toward the viewer, viewer's right = +X), height kCountdownDigitH. Each segment is a
//                 dark outline quad 2 mm behind a white quad. Drawn with the blended pipeline.
// Colours are LINEAR RGB (the eye buffer is written through an sRGB view when it is typeless/sRGB).
// Vertex colour alpha carries a FLAG (see the shader in render_pass.h):
//   0 plain, 1 handle (brighter when highlight), 2 shutter dome (glows when shutterHot),
//   3 lens ring / glass (whitened by flash), 10+s white digit segment s, 20+s digit outline of segment s.
#pragma once
#include "selfie_shared.h"
#include <cmath>
#include <vector>

namespace render_mesh {

struct Vertex { float p[3]; float n[3]; float c[4]; };
static_assert(sizeof(Vertex) == 40, "vertex layout used by the input layout");

struct Range { unsigned first = 0, count = 0; };
struct Mesh {
    std::vector<Vertex> vertices;
    Range fixed, shaft, top, cam, digit;
};

struct Color { float r, g, b; };
// Linear colours (DESIGN §4: dark grey handle, silver shaft, black box, blue lens ring; §7 red button).
constexpr Color kHandle      = {0.030f, 0.030f, 0.034f};
constexpr Color kHandleLit   = {0.200f, 0.200f, 0.215f};   // used by the shader when highlight=1
constexpr Color kShaft       = {0.550f, 0.560f, 0.580f};
constexpr Color kCollar      = {0.180f, 0.180f, 0.190f};
constexpr Color kBox         = {0.010f, 0.010f, 0.010f};
constexpr Color kRing        = {0.020f, 0.120f, 0.850f};
constexpr Color kLens        = {0.004f, 0.005f, 0.012f};
constexpr Color kButton      = {0.450f, 0.012f, 0.010f};   // red dome (lit)
constexpr Color kButtonHot   = {1.000f, 0.180f, 0.120f};   // emissive glow when shutterHot (shader constant)

// Flags (vertex colour alpha).
constexpr float kFlagPlain = 0, kFlagHandle = 1, kFlagButton = 2, kFlagLens = 3, kFlagDigit = 10, kFlagOutline = 20;

// Stick geometry (stick-local).
constexpr float kShaftBottom = selfie::kHandleLength - 0.005f;   // shaft starts just inside the handle
constexpr float kCollarH     = 0.012f;
// Camera-head geometry (camera-head local).
constexpr float kRingRadius  = 0.019f;
constexpr float kRingDepth   = 0.010f;   // protrudes this far from the box front face
constexpr float kLensRadius  = 0.013f;
constexpr int   kSegments    = 20;
// Digit geometry (digit-local, metres).
constexpr float kDigitH      = selfie::kCountdownDigitH;
constexpr float kDigitW      = kDigitH * 0.56f;
constexpr float kDigitT      = kDigitH * 0.15f;      // segment thickness
constexpr float kOutline     = 0.005f;               // outline margin around each segment
constexpr float kOutlineBack = 0.002f;               // outline sits this far behind the white face
// Segment masks for 1, 2, 3 (bit s = segment s: 0 a top, 1 b upper-right, 2 c lower-right,
// 3 d bottom, 4 e lower-left, 5 f upper-left, 6 g middle).
inline unsigned digitMask(int d) {
    switch (d) {
    case 1: return (1u << 1) | (1u << 2);
    case 2: return (1u << 0) | (1u << 1) | (1u << 6) | (1u << 4) | (1u << 3);
    case 3: return (1u << 0) | (1u << 1) | (1u << 6) | (1u << 2) | (1u << 3);
    default: return 0;
    }
}
// Segment rectangle (centre x, centre y, half width, half height) in digit-local space.
inline void segmentRect(int s, float r[4]) {
    const float hw = kDigitW * 0.5f, hh = kDigitH * 0.5f, t = kDigitT * 0.5f;
    const float vlen = hh * 0.5f;   // half-length of a vertical segment (each covers half the height)
    switch (s) {
    case 0: r[0] = 0;   r[1] = hh - t;        r[2] = hw; r[3] = t; break;      // a
    case 1: r[0] = hw - t;  r[1] = hh * 0.5f; r[2] = t; r[3] = vlen; break;    // b (right = +X)
    case 2: r[0] = hw - t;  r[1] = -hh * 0.5f; r[2] = t; r[3] = vlen; break;   // c
    case 3: r[0] = 0;   r[1] = -hh + t;       r[2] = hw; r[3] = t; break;      // d
    case 4: r[0] = -hw + t; r[1] = -hh * 0.5f; r[2] = t; r[3] = vlen; break;   // e
    case 5: r[0] = -hw + t; r[1] = hh * 0.5f; r[2] = t; r[3] = vlen; break;    // f
    default: r[0] = 0;  r[1] = 0;             r[2] = hw; r[3] = t; break;      // g
    }
}

namespace detail {
inline void push(std::vector<Vertex>& v, const float p[3], const float n[3], Color c, float flag) {
    Vertex x;
    for (int i = 0; i < 3; i++) { x.p[i] = p[i]; x.n[i] = n[i]; }
    x.c[0] = c.r; x.c[1] = c.g; x.c[2] = c.b; x.c[3] = flag;
    v.push_back(x);
}
// axis: 1 = along +Y (stick), 2 = along +Z (lens). For axis 2 local (x, y, z) -> (x, -z, y): a proper rotation.
inline void map(int axis, const float in[3], float out[3]) {
    if (axis == 1) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; }
    else           { out[0] = in[0]; out[1] = -in[2]; out[2] = in[1]; }
}
inline void tri(std::vector<Vertex>& v, int axis, const float a[3], const float an[3], const float b[3], const float bn[3],
                const float c[3], const float cn[3], Color col, float flag) {
    float p[3], n[3];
    map(axis, a, p); map(axis, an, n); push(v, p, n, col, flag);
    map(axis, b, p); map(axis, bn, n); push(v, p, n, col, flag);
    map(axis, c, p); map(axis, cn, n); push(v, p, n, col, flag);
}
// Cylinder along the axis from h0 to h1 (smooth side normals, flat caps).
inline void cylinder(std::vector<Vertex>& v, int axis, float radius, float h0, float h1, bool cap0, bool cap1, Color col, float flag) {
    const float pi2 = 6.28318530718f;
    for (int i = 0; i < kSegments; i++) {
        float a0 = pi2 * i / kSegments, a1 = pi2 * (i + 1) / kSegments;
        float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        float p00[3] = {radius * c0, h0, radius * s0}, p01[3] = {radius * c0, h1, radius * s0};
        float p10[3] = {radius * c1, h0, radius * s1}, p11[3] = {radius * c1, h1, radius * s1};
        float n0[3] = {c0, 0, s0}, n1[3] = {c1, 0, s1};
        tri(v, axis, p00, n0, p01, n0, p11, n1, col, flag);
        tri(v, axis, p00, n0, p11, n1, p10, n1, col, flag);
        if (cap0) {
            float centre[3] = {0, h0, 0}, down[3] = {0, -1, 0};
            tri(v, axis, centre, down, p10, down, p00, down, col, flag);
        }
        if (cap1) {
            float centre[3] = {0, h1, 0}, up[3] = {0, 1, 0};
            tri(v, axis, centre, up, p01, up, p11, up, col, flag);
        }
    }
}
// Axis-aligned box centred at the origin with half extents h.
inline void box(std::vector<Vertex>& v, const float h[3], Color col) {
    for (int axis = 0; axis < 3; axis++)
        for (int sign = -1; sign <= 1; sign += 2) {
            int u = (axis + 1) % 3, w = (axis + 2) % 3;
            float n[3] = {0, 0, 0};
            n[axis] = float(sign);
            float corner[4][3];
            const int du[4] = {-1, 1, 1, -1}, dw[4] = {-1, -1, 1, 1};
            for (int k = 0; k < 4; k++) {
                corner[k][axis] = sign * h[axis];
                corner[k][u] = du[k] * h[u];
                corner[k][w] = dw[k] * h[w];
            }
            tri(v, 1, corner[0], n, corner[1], n, corner[2], n, col, 0);
            tri(v, 1, corner[0], n, corner[2], n, corner[3], n, col, 0);
        }
}
// Hemisphere of radius r centred at c, bulging along +Z (the shutter dome; its base is hidden in the handle).
inline void dome(std::vector<Vertex>& v, const float c[3], float r, Color col, float flag) {
    const int rings = 6, segs = 16;
    const float pi = 3.14159265359f;
    auto point = [&](int ring, int seg, float p[3], float n[3]) {
        float th = (pi * 0.5f) * float(ring) / rings;   // 0 at the pole (+Z), pi/2 at the base
        float ph = 2 * pi * float(seg) / segs;
        n[0] = std::sin(th) * std::cos(ph); n[1] = std::sin(th) * std::sin(ph); n[2] = std::cos(th);
        for (int i = 0; i < 3; i++) p[i] = c[i] + r * n[i];
    };
    for (int ring = 0; ring < rings; ring++)
        for (int seg = 0; seg < segs; seg++) {
            float p00[3], n00[3], p01[3], n01[3], p10[3], n10[3], p11[3], n11[3];
            point(ring, seg, p00, n00); point(ring, seg + 1, p01, n01);
            point(ring + 1, seg, p10, n10); point(ring + 1, seg + 1, p11, n11);
            tri(v, 1, p00, n00, p10, n10, p11, n11, col, flag);
            if (ring > 0) tri(v, 1, p00, n00, p11, n11, p01, n01, col, flag);   // ring 0: p00 == p01 (the pole)
        }
}
// Flat quad in the z = z0 plane (normal +Z), centre (x, y), half sizes (hx, hy).
inline void quad(std::vector<Vertex>& v, float x, float y, float hx, float hy, float z0, Color col, float flag) {
    const float n[3] = {0, 0, 1};
    const float a[3] = {x - hx, y - hy, z0}, b[3] = {x + hx, y - hy, z0}, c[3] = {x + hx, y + hy, z0}, d[3] = {x - hx, y + hy, z0};
    tri(v, 1, a, n, b, n, c, n, col, flag);
    tri(v, 1, a, n, c, n, d, n, col, flag);
}
}  // namespace detail

inline Mesh build() {
    using namespace selfie;
    Mesh m;
    auto& v = m.vertices;
    v.reserve(4096);
    auto begin = [&](Range& r) { r.first = unsigned(v.size()); };
    auto end = [&](Range& r) { r.count = unsigned(v.size()) - r.first; };
    // ---- fixed part (stick-local): handle + shutter dome ----
    begin(m.fixed);
    detail::cylinder(v, 1, kHandleRadius, 0.0f, kHandleLength, true, true, kHandle, kFlagHandle);
    const float button[3] = {0, kShutterY, kHandleRadius - 0.002f};   // seated 2 mm into the handle surface
    detail::dome(v, button, kShutterRadius, kButton, kFlagButton);
    end(m.fixed);
    // ---- unit shaft ----
    begin(m.shaft);
    detail::cylinder(v, 1, kShaftRadius, 0.0f, 1.0f, false, false, kShaft, kFlagPlain);
    end(m.shaft);
    // ---- top part: collar below the camera mount ----
    begin(m.top);
    detail::cylinder(v, 1, kShaftRadius + 0.004f, -kCollarH, 0.0f, true, true, kCollar, kFlagPlain);
    end(m.top);
    // ---- camera part (camera-head-local, origin at the box centre, lens on +Z) ----
    begin(m.cam);
    const float half[3] = {kCamBox[0] * 0.5f, kCamBox[1] * 0.5f, kCamBox[2] * 0.5f};
    detail::box(v, half, kBox);
    const float front = half[2];
    detail::cylinder(v, 2, kRingRadius, front - 0.002f, front + kRingDepth, false, true, kRing, kFlagLens);
    // Glass disc 1 mm proud of the ring's front face (no z-fighting).
    detail::cylinder(v, 2, kLensRadius, front + kRingDepth, front + kRingDepth + 0.001f, false, true, kLens, kFlagLens);
    end(m.cam);
    // ---- digit part: per segment an outline quad (behind) then the white quad; order matters (blending) ----
    begin(m.digit);
    for (int s = 0; s < 7; s++) {
        float r[4];
        segmentRect(s, r);
        detail::quad(v, r[0], r[1], r[2] + kOutline, r[3] + kOutline, -kOutlineBack, Color{0.0f, 0.0f, 0.0f}, kFlagOutline + s);
    }
    for (int s = 0; s < 7; s++) {
        float r[4];
        segmentRect(s, r);
        detail::quad(v, r[0], r[1], r[2], r[3], 0.0f, Color{1.0f, 1.0f, 1.0f}, kFlagDigit + s);
    }
    end(m.digit);
    return m;
}

}  // namespace render_mesh
