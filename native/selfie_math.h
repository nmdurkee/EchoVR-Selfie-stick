// selfie_math.h - vector/quaternion math for SelfieStick, matched to echovr.exe.
//
// Conventions (verified against the decompile, echovr.exe sha256 3dae0cda...df9a):
//   * Quat memory order x,y,z,w; Hamilton product.
//   * QMul(a,b) is a transcription of FUN_1400f94e0(out,a,b):
//       x = bx*aw + ax*bw + bz*ay - by*az
//       y = by*aw + ay*bw + az*bx - bz*ax
//       z = bz*aw + az*bw + by*ax - ay*bx
//       w = aw*bw - bx*ax - by*ay - az*bz
//   * QRot(q,v) is a transcription of FUN_1400f93b0(out,v,q) (active rotation, v' = q v q*):
//       t = 2*(q.xyz x v);  out = v + w*t + q.xyz x t
//   * Engine/camera basis: forward = +Z, up = +Y, right = -X  (so local +X is "left").
//   * World <- engine tracking: the rig base (FUN_140629c30): p_w = baseP + QRot(baseQ,p_t), q_w = baseQ*q_t.
//   * Engine tracking <- raw OVR tracking (getters FUN_14072cd50/cc10/...):
//       p_e = QRot(R, flip(p_o) + offset),   q_e = R * flipq(q_o)
//     flip(x,y,z) = (-x,y,-z), flipq(x,y,z,w) = (-x,y,-z,w), R = DAT_1420223f8, offset = DAT_1420c7790.
#pragma once
#include <math.h>
#include "selfie_shared.h"

namespace selfie {

inline Vec3 V3(float x, float y, float z) { return Vec3{x, y, z}; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(Vec3 a) { return sqrtf(Dot(a, a)); }
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline bool Finite(float f) { return isfinite(f) != 0; }
inline bool Finite(Vec3 v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
inline bool Finite(Quat q) { return Finite(q.x) && Finite(q.y) && Finite(q.z) && Finite(q.w); }
// Returns false (and leaves *out alone) if a is ~zero.
inline bool Normalize(Vec3 a, Vec3* out) {
    float l = Len(a);
    if (!(l > 1e-6f) || !Finite(l)) return false;
    *out = a * (1.0f / l);
    return true;
}

inline Quat QIdentity() { return Quat{0, 0, 0, 1}; }
inline Quat QConj(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }
inline float QDot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Quat QNormalize(Quat q) {
    float l = sqrtf(QDot(q, q));
    if (!(l > 1e-8f) || !Finite(l)) return QIdentity();
    float s = 1.0f / l;
    return Quat{q.x * s, q.y * s, q.z * s, q.w * s};
}

// Transcription of FUN_1400f94e0(out, a, b).
inline Quat QMul(Quat a, Quat b) {
    const float ax = a.x, ay = a.y, az = a.z, aw = a.w;
    const float bx = b.x, by = b.y, bz = b.z, bw = b.w;
    Quat r;
    r.x = (bx * aw + ax * bw + bz * ay) - by * az;
    r.y = (by * aw + ay * bw + az * bx) - bz * ax;
    r.z = (bz * aw + az * bw + by * ax) - ay * bx;
    r.w = ((aw * bw - bx * ax) - by * ay) - az * bz;
    return r;
}

// Transcription of FUN_1400f93b0(out, v, q) (variable names follow the decompile).
inline Vec3 QRot(Quat q, Vec3 v) {
    const float f1 = q.z, f2 = v.x, f3 = q.w, f4 = q.x, f5 = q.y;
    float f9 = v.z * f5 - v.y * f1;
    float f8 = f2 * f1 - v.z * f4;
    f9 = f9 + f9;
    float f7 = v.y * f4 - f2 * f5;
    f8 = f8 + f8;
    f7 = f7 + f7;
    Vec3 r;
    r.x = (f7 * f5 - f8 * f1) + f9 * f3 + f2;
    r.z = (f8 * f4 - f9 * f5) + f7 * f3 + v.z;
    r.y = (f9 * f1 - f7 * f4) + f8 * f3 + v.y;
    return r;
}

inline Quat QAxisAngle(Vec3 axis, float rad) {
    Vec3 n;
    if (!Normalize(axis, &n)) return QIdentity();
    float s = sinf(rad * 0.5f);
    return Quat{n.x * s, n.y * s, n.z * s, cosf(rad * 0.5f)};
}

// Shortest-arc rotation taking unit vector a onto unit vector b.
inline Quat QFromTo(Vec3 a, Vec3 b) {
    float d = Dot(a, b);
    if (d < -0.999999f) {
        Vec3 axis = Cross(V3(1, 0, 0), a);
        if (Len(axis) < 1e-4f) axis = Cross(V3(0, 1, 0), a);
        return QAxisAngle(axis, 3.14159265358979f);
    }
    Vec3 c = Cross(a, b);
    return QNormalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

// Normalized lerp along the shorter arc; t = weight of b.
inline Quat QNlerp(Quat a, Quat b, float t) {
    float sg = QDot(a, b) < 0 ? -1.0f : 1.0f;
    return QNormalize(Quat{a.x + (sg * b.x - a.x) * t, a.y + (sg * b.y - a.y) * t,
                           a.z + (sg * b.z - a.z) * t, a.w + (sg * b.w - a.w) * t});
}

// Angle between two rotations, radians [0, pi].
inline float QAngle(Quat a, Quat b) {
    float d = fabsf(QDot(QNormalize(a), QNormalize(b)));
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d);
}

// Rotation from an orthonormal right-handed basis given as columns (local X, Y, Z in world).
inline Quat QFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    const float m00 = x.x, m11 = y.y, m22 = z.z, t = m00 + m11 + m22;
    Quat q;
    if (t > 0) {
        float s = sqrtf(t + 1.0f) * 2.0f;
        q = Quat{(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q = Quat{0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q = Quat{(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q = Quat{(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return QNormalize(q);
}

// Look-at in the engine convention: the result maps local +Z onto `fwd` and local +Y
// as close as possible to `up` (local -X = right). False if fwd is ~0 or parallel to up.
inline bool LookAt(Vec3 fwd, Vec3 up, Quat* out) {
    Vec3 z, x;
    if (!Normalize(fwd, &z)) return false;
    if (!Normalize(Cross(up, z), &x)) return false;
    if (Len(Cross(up, z)) < 1e-3f * (Len(up) > 0 ? Len(up) : 1.0f)) return false;
    Vec3 y = Cross(z, x);
    *out = QFromBasis(x, y, z);
    return true;
}

// ---- poses ----------------------------------------------------------------
inline Pose MakePose(Quat q, Vec3 p) { Pose r; r.q = q; r.p = p; return r; }
inline Pose PIdentity() { return MakePose(QIdentity(), V3(0, 0, 0)); }
// (a*b): apply b first, then a.  a = parent<-child.
inline Pose PMul(const Pose& a, const Pose& b) { return MakePose(QMul(a.q, b.q), a.p + QRot(a.q, b.p)); }
inline Pose PInv(const Pose& a) { Quat c = QConj(a.q); return MakePose(c, QRot(c, -a.p)); }
inline Vec3 PApply(const Pose& a, Vec3 v) { return a.p + QRot(a.q, v); }

// ---- engine tracking <-> raw OVR tracking -----------------------------------
inline Vec3 Flip(Vec3 v) { return V3(-v.x, v.y, -v.z); }
inline Quat FlipQ(Quat q) { return Quat{-q.x, q.y, -q.z, q.w}; }

struct TrackFrame {        // the getters' recenter rotation and playspace offset
    Quat recenter;         // DAT_1420223f8
    Vec3 offset;           // DAT_1420c7790
};

// Exactly what the engine getters compute (FUN_14072cd50 / FUN_14072cc10 and the hand ones).
inline Vec3 EngFromOvrPos(const TrackFrame& f, Vec3 o) { return QRot(f.recenter, Flip(o) + f.offset); }
inline Quat EngFromOvrQ(const TrackFrame& f, Quat o) { return QMul(f.recenter, FlipQ(o)); }
// Inverses (DESIGN section 2).
inline Vec3 OvrFromEngPos(const TrackFrame& f, Vec3 e) { return Flip(QRot(QConj(f.recenter), e) - f.offset); }
// Inverse of the getters' ORIENTATION mapping (an OVR device orientation, whose local
// forward is -Z, from the engine one, whose local forward is +Z).
inline Quat OvrFromEngQ(const TrackFrame& f, Quat e) { return FlipQ(QMul(QConj(f.recenter), e)); }

// RIGID pose conversion, for geometry: a mesh vertex v (object-local) must land at
// OvrFromEngPos(PApply(eng, v)) == PApply(ovr, v). The point map x -> flip(R^-1 x - off)
// is the rotation Y180*R^-1 plus a translation, so q_ovr = Y180 * conj(R) * q_eng.
// NOTE: this differs from OvrFromEngQ by a 180 deg turn about the object's local Y
// (flipq(q) = Y180 q Y180^-1). Using OvrFromEngQ for a drawn object would turn its
// lens (local +Z) to face the wrong way. RenderState poses use this rigid form.
inline Quat kY180() { return Quat{0, 1, 0, 0}; }
inline Pose OvrFromEng(const TrackFrame& f, const Pose& e) {
    return MakePose(QNormalize(QMul(kY180(), QMul(QConj(f.recenter), e.q))), OvrFromEngPos(f, e.p));
}
inline Pose EngFromOvr(const TrackFrame& f, const Pose& o) {   // rigid inverse of OvrFromEng
    return MakePose(QNormalize(QMul(f.recenter, QMul(QConj(kY180()), o.q))), EngFromOvrPos(f, o.p));
}

// ---- world <-> engine tracking (rig base read from cam0) ---------------------
inline Pose WorldFromTrack(const Pose& rig, const Pose& t) { return PMul(rig, t); }
inline Pose TrackFromWorld(const Pose& rig, const Pose& w) { return PMul(PInv(rig), w); }

// Distance from p to segment [a,b]; *tOut = clamped parameter in [0,1].
inline float SegmentDistance(Vec3 p, Vec3 a, Vec3 b, float* tOut = nullptr) {
    Vec3 ab = b - a;
    float l2 = Dot(ab, ab);
    float t = l2 > 1e-12f ? Dot(p - a, ab) / l2 : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (tOut) *tOut = t;
    return Len(p - (a + ab * t));
}

} // namespace selfie
