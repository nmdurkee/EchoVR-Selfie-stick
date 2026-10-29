// render_math.h - double-precision view math for drawing the selfie stick into
// Echo's own eye images. Adapted from EchoCraft (MinecraftEchoVR/native/echo_view.h),
// whose projection path is headset-verified on the same echovr.exe build.
//
// Spaces: raw LibOVR tracking space (metres, camera looks down -Z, +Y up).
// All 4x4 matrices are row-major and act on column vectors (HLSL row_major mul(M, v)).
#pragma once
#include <cmath>
#include <cstring>

namespace render_math {

struct Affine {
    double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double t[3] = {0, 0, 0};
    void apply(const double in[3], double out[3]) const {
        for (int i = 0; i < 3; i++) out[i] = r[i][0] * in[0] + r[i][1] * in[1] + r[i][2] * in[2] + t[i];
    }
};

inline Affine compose(const Affine& a, const Affine& b) {  // a after b
    Affine o;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        o.t[i] = a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2] + a.t[i];
    }
    return o;
}

// Inverse of a rigid transform (rotation + translation): transpose the rotation.
inline Affine rigidInverse(const Affine& a) {
    Affine o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o.r[i][j] = a.r[j][i];
    for (int i = 0; i < 3; i++) o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
    return o;
}

// Quaternion (x,y,z,w; Hamilton, active rotation - LibOVR and Echo convention) + position.
// Returns false for non-finite input or a quaternion far from unit length.
inline bool fromPose(const float q[4], const float p[3], Affine& a) {
    for (int i = 0; i < 4; i++) if (!std::isfinite(q[i])) return false;
    for (int i = 0; i < 3; i++) if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1.0e5f) return false;
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double n2 = x * x + y * y + z * z + w * w;
    if (!(n2 > 0.5 && n2 < 1.5)) return false;
    double n = std::sqrt(n2);
    x /= n; y /= n; z /= n; w /= n;
    a.r[0][0] = 1 - 2 * (y * y + z * z); a.r[0][1] = 2 * (x * y - z * w);     a.r[0][2] = 2 * (x * z + y * w);
    a.r[1][0] = 2 * (x * y + z * w);     a.r[1][1] = 1 - 2 * (x * x + z * z); a.r[1][2] = 2 * (y * z - x * w);
    a.r[2][0] = 2 * (x * z - y * w);     a.r[2][1] = 2 * (y * z + x * w);     a.r[2][2] = 1 - 2 * (x * x + y * y);
    for (int i = 0; i < 3; i++) a.t[i] = p[i];
    return true;
}

// LibOVR off-axis projection with Echo's own depth row (ovrTimewarpProjectionDesc:
// Projection22, Projection23, Projection32), so our depth values are directly
// comparable with Echo's depth buffer. Right-handed only (w = -z).
struct Projection { double m[16] = {}; bool reversed = false; bool valid = false; };
inline Projection projection(float up, float down, float left, float right, float p22, float p23, float p32) {
    Projection p;
    const float tans[4] = {up, down, left, right};
    for (float v : tans) if (!std::isfinite(v) || !(v > 0.01f) || v > 10.f) return p;
    if (!std::isfinite(p22) || !std::isfinite(p23) || !std::isfinite(p32) || p32 > -0.5f || p32 < -1.5f) return p;
    p.m[0] = 2.0 / (double(left) + right);  p.m[2] = (double(right) - left) / (double(left) + right);
    p.m[5] = 2.0 / (double(up) + down);     p.m[6] = (double(up) - down) / (double(up) + down);
    p.m[10] = p22; p.m[11] = p23; p.m[14] = p32;
    auto depth = [&](double z) { return (p22 * z + p23) / (p32 * z); };
    double nearDepth = depth(-0.25), farDepth = depth(-200.0);
    if (!std::isfinite(nearDepth) || !std::isfinite(farDepth) || std::fabs(nearDepth - farDepth) < 1e-6) return p;
    p.reversed = nearDepth > farDepth;
    p.valid = true;
    return p;
}

// clip = P * inverse(eye) * object. Output is row-major float[16]. False if anything is non-finite.
inline bool clipFromObject(const Projection& p, const Affine& eye, const Affine& object, float out[16]) {
    if (!p.valid) return false;
    Affine m = compose(rigidInverse(eye), object);
    const double v[16] = {m.r[0][0], m.r[0][1], m.r[0][2], m.t[0],
                          m.r[1][0], m.r[1][1], m.r[1][2], m.t[1],
                          m.r[2][0], m.r[2][1], m.r[2][2], m.t[2],
                          0, 0, 0, 1};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += p.m[i * 4 + k] * v[k * 4 + j];
            if (!std::isfinite(s)) return false;
            out[i * 4 + j] = float(s);
        }
    return true;
}

}  // namespace render_math
