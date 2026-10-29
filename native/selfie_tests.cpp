// selfie_tests.cpp - unit tests for SelfieStick core math and state machine.
// Pure: no engine, no hooks. build.cmd builds and runs this before the DLL.
#include <stdio.h>
#include <math.h>
#include <initializer_list>
#include "selfie_math.h"
#include "selfie_sim.h"

using namespace selfie;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return fabsf(a - b) <= eps; }
static bool Near(Vec3 a, Vec3 b, float eps = 1e-4f) { return Len(a - b) <= eps; }
static bool NearQ(Quat a, Quat b, float eps = 1e-4f) {   // same rotation (q ~ -q)
    float d = fabsf(QDot(QNormalize(a), QNormalize(b)));
    return d >= 1.0f - eps;
}
static const char* S(Vec3 v) { static char b[8][64]; static int i; char* r = b[i++ & 7]; snprintf(r, 64, "(%.4f %.4f %.4f)", v.x, v.y, v.z); return r; }

// deterministic RNG
static unsigned g_rng = 12345;
static float Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return ((g_rng >> 8) & 0xFFFFFF) / float(0xFFFFFF) * 2.0f - 1.0f; }
static Vec3 RndV(float s = 1) { return V3(Rnd() * s, Rnd() * s, Rnd() * s); }
static Quat RndQ() { return QNormalize(Quat{Rnd(), Rnd(), Rnd(), Rnd()}); }
static Vec3 X() { return V3(1, 0, 0); }
static Vec3 Y() { return V3(0, 1, 0); }
static Vec3 Z() { return V3(0, 0, 1); }

// ---- independent references -------------------------------------------------
static Quat RefHamilton(Quat a, Quat b) {
    Vec3 av = V3(a.x, a.y, a.z), bv = V3(b.x, b.y, b.z);
    Vec3 v = bv * a.w + av * b.w + Cross(av, bv);
    return Quat{v.x, v.y, v.z, a.w * b.w - Dot(av, bv)};
}
static Vec3 RefRotate(Quat q, Vec3 v) {
    Quat r = RefHamilton(RefHamilton(q, Quat{v.x, v.y, v.z, 0}), QConj(q));
    return V3(r.x, r.y, r.z);
}
// Literal transcriptions of the engine getters (decompile), in terms of the raw OVR copy.
//   FUN_14072cd50 (head pos): local = (-raw.x, raw.y, -raw.z) + DAT_1420c7790; FUN_1400f93b0(out, local, DAT_1420223f8)
static Vec3 GetterPos(Quat R, Vec3 off, Vec3 raw) {
    Vec3 local = V3(-raw.x, raw.y, -raw.z);
    local = V3(off.x + local.x, off.y + local.y, off.z + local.z);
    return QRot(R, local);
}
//   FUN_14072cc10 (head rot): local = (-raw.x, raw.y, raw.z ^ sign, raw.w); FUN_1400f94e0(out, DAT_1420223f8, local)
static Quat GetterRot(Quat R, Quat raw) { return QMul(R, Quat{-raw.x, raw.y, -raw.z, raw.w}); }

// ---- tests -----------------------------------------------------------------
static void TestQuatConventions() {
    for (int i = 0; i < 200; i++) {
        Quat a = RndQ(), b = RndQ();
        Vec3 v = RndV(3);
        Quat m = QMul(a, b), r = RefHamilton(a, b);
        CHECK(Near(m.x, r.x) && Near(m.y, r.y) && Near(m.z, r.z) && Near(m.w, r.w), "QMul != Hamilton a*b");
        CHECK(Near(QRot(a, v), RefRotate(a, v), 1e-4f), "QRot != q v q*");
        // composition: rotating by a*b == rotate by b then a (what FUN_140629c30 relies on)
        CHECK(Near(QRot(QMul(a, b), v), QRot(a, QRot(b, v)), 1e-4f), "QRot(a*b) != QRot(a,QRot(b))");
    }
    // +90 deg about Z takes X to Y (active, right-handed) - the hand check from the decompile
    Quat z90 = QAxisAngle(Z(), 3.14159265f / 2);
    CHECK(Near(QRot(z90, X()), Y()), "z90*X = %s", S(QRot(z90, X())));
    Quat y90 = QAxisAngle(Y(), 3.14159265f / 2);
    CHECK(Near(QRot(y90, Z()), X()), "y90*Z = %s", S(QRot(y90, Z())));
}

static void TestLookAt() {
    Quat q;
    CHECK(LookAt(Z(), Y(), &q) && NearQ(q, QIdentity()), "LookAt(+Z,+Y) must be identity");
    // engine convention self-check: the engine's QRot takes local +Z onto fwd (not the conjugate)
    Vec3 f;
    Normalize(V3(1, 0.3f, 0.2f), &f);
    CHECK(LookAt(f, Y(), &q) && Dot(QRot(q, Z()), f) > 0.9999f, "engine-convention self-check (prototype calibrate): dot=%f", Dot(QRot(q, Z()), f));
    for (int i = 0; i < 500; i++) {
        Vec3 fwd = RndV(), up = RndV();
        Vec3 fn, un;
        if (!Normalize(fwd, &fn) || !Normalize(up, &un) || fabsf(Dot(fn, un)) > 0.98f) continue;
        bool ok = LookAt(fwd, up, &q);
        CHECK(ok, "LookAt failed");
        if (!ok) continue;
        CHECK(Near(QDot(q, q), 1.0f, 1e-4f), "not unit");
        Vec3 lz = QRot(q, Z()), ly = QRot(q, Y()), lx = QRot(q, X());
        CHECK(Near(lz, fn, 1e-3f), "fwd %s vs %s", S(lz), S(fn));
        CHECK(fabsf(Dot(ly, fn)) < 1e-3f && Dot(ly, un) > 0, "up not orthogonal/positive");
        Vec3 right;
        Normalize(Cross(fn, un), &right);   // looking along fwd with up: right = fwd x up
        CHECK(Near(QRot(q, V3(-1, 0, 0)), right, 1e-3f), "right must be local -X: %s vs %s", S(QRot(q, V3(-1, 0, 0))), S(right));
        CHECK(Near(Cross(lx, ly), lz, 1e-3f), "basis not right-handed");
    }
    CHECK(!LookAt(Y(), Y(), &q), "LookAt parallel must fail");
    CHECK(!LookAt(V3(0, 0, 0), Y(), &q), "LookAt zero must fail");
}

static void TestOvrEngine() {
    for (int i = 0; i < 300; i++) {
        TrackFrame f;
        f.recenter = (i == 0) ? QIdentity() : RndQ();
        f.offset = (i == 0) ? V3(0, 0, 0) : RndV(2);
        Vec3 raw = RndV(2);
        Quat rawQ = RndQ();
        // our forward conversion == getter transcription
        Vec3 e = GetterPos(f.recenter, f.offset, raw);
        CHECK(Near(EngFromOvrPos(f, raw), e, 1e-4f), "EngFromOvrPos != getter");
        Quat eq = GetterRot(f.recenter, rawQ);
        CHECK(NearQ(EngFromOvrQ(f, rawQ), eq), "EngFromOvrQ != getter");
        // DESIGN section 2 inverses round-trip against the getters
        CHECK(Near(OvrFromEngPos(f, e), raw, 1e-4f), "OvrFromEngPos round trip %s vs %s", S(OvrFromEngPos(f, e)), S(raw));
        CHECK(NearQ(OvrFromEngQ(f, eq), rawQ), "OvrFromEngQ round trip");
        // rigid pose conversion: mesh vertices land where the point map puts them
        Pose pe = MakePose(RndQ(), RndV(5));
        Pose po = OvrFromEng(f, pe);
        for (int k = 0; k < 4; k++) {
            Vec3 v = RndV(1);
            CHECK(Near(PApply(po, v), OvrFromEngPos(f, PApply(pe, v)), 1e-3f), "rigid OvrFromEng vertex mismatch");
        }
        Pose back = EngFromOvr(f, po);
        CHECK(Near(back.p, pe.p, 1e-3f) && NearQ(back.q, pe.q), "EngFromOvr(OvrFromEng) round trip");
        // a direction seen by an OVR device (local -Z) equals the engine's local +Z for the same device
        Quat devO = RndQ();
        Quat devE = EngFromOvrQ(f, devO);
        Vec3 fwdO = QRot(devO, V3(0, 0, -1));
        Vec3 fwdE = QRot(devE, Z());
        Vec3 fwdOinE = EngFromOvrPos(f, fwdO) - EngFromOvrPos(f, V3(0, 0, 0));
        CHECK(Near(fwdE, fwdOinE, 1e-3f), "getter orientation convention: engine +Z must be OVR -Z");
    }
}

static void TestWorldTrack() {
    for (int i = 0; i < 100; i++) {
        Pose rig = MakePose(RndQ(), RndV(50));
        Pose t = MakePose(RndQ(), RndV(2));
        Pose w = WorldFromTrack(rig, t);
        // exactly FUN_140629c30: pos = basePos + QRot(baseQ, hmdPos), q = QMul(baseQ, hmdQ)
        CHECK(Near(w.p, rig.p + QRot(rig.q, t.p), 1e-3f) && NearQ(w.q, QMul(rig.q, t.q)), "WorldFromTrack != FUN_140629c30");
        Pose t2 = TrackFromWorld(rig, w);
        CHECK(Near(t2.p, t.p, 1e-3f) && NearQ(t2.q, t.q), "TrackFromWorld round trip");
    }
}

// ---- state machine helpers --------------------------------------------------
static SimInput MakeInput(const Pose& head) {
    SimInput in;
    in.headOk = true;
    in.head = head;
    in.handsOk = true;
    in.hand[0] = MakePose(QIdentity(), head.p + V3(0.3f, -0.5f, 0));    // far away
    in.hand[1] = MakePose(QIdentity(), head.p + V3(-0.3f, -0.5f, 0));
    return in;
}
static Vec3 StickPoint(const StickSim& s, float y) { return PApply(s.Stick(), V3(0, y, 0)); }

static void TestSpawn() {
    for (int i = 0; i < 20; i++) {
        StickSim s;
        s.cfg.smooth = 0;
        Pose head = MakePose(i == 0 ? QIdentity() : QNormalize(QMul(QAxisAngle(Y(), Rnd() * 3), QAxisAngle(X(), Rnd() * 0.4f))), RndV(10));
        s.Enable(true);
        SimInput none = MakeInput(head);
        none.headOk = false;
        s.Step(none);
        CHECK(!s.Visible(), "no head -> must wait to spawn");
        s.Step(MakeInput(head));
        CHECK(s.Visible() && s.Mode() == StickMode::Free, "spawned FREE");
        Vec3 handle = StickPoint(s, kHandleLength * 0.5f);
        CHECK(Near(handle, PApply(head, V3(0, -0.25f, 0.5f)), 1e-3f), "handle centre 0.5 m in front at chest height: %s", S(handle));
        CHECK(Near(QRot(s.Stick().q, Y()), QRot(head.q, Y()), 1e-3f), "shaft along head up");
        CHECK(Near(QRot(s.Stick().q, Z()), QRot(head.q, V3(0, 0, -1)), 1e-3f), "lens side faces the user");
        CHECK(Dot(StickPoint(s, kCamHeadY) - StickPoint(s, 0), QRot(head.q, Y())) > 1.0f, "camera on top");
    }
    if (true) {   // straight-ahead head: exact numbers
        StickSim s;
        s.Enable(true);
        s.Step(MakeInput(MakePose(QIdentity(), V3(0, 1.6f, 0))));
        CHECK(Near(s.Stick().p, V3(0, 1.6f - 0.25f - kHandleLength / 2, 0.5f), 1e-4f), "spawn butt %s", S(s.Stick().p));
    }
}

static void TestGrabHysteresis() {
    StickSim s;
    s.cfg.smooth = 0;
    Pose head = MakePose(QIdentity(), V3(0, 1.6f, 0));
    s.Enable(true);
    SimInput in = MakeInput(head);
    s.Step(in);
    Pose start = s.Stick();
    // right hand on the handle
    in.hand[1] = MakePose(QAxisAngle(X(), 0.3f), StickPoint(s, 0.1f) + V3(0.03f, 0, 0));
    in.grip[1] = 0.5f; s.Step(in);
    CHECK(s.Mode() == StickMode::Free, "0.5 < grabOn: no grab");
    CHECK(s.Highlight(), "hand within radius -> highlight");
    in.grip[1] = 0.65f; s.Step(in);
    CHECK(s.Mode() == StickMode::Held1 && s.Held(1), "0.65 > 0.6: grab");
    in.grip[1] = 0.5f; s.Step(in);
    CHECK(s.Mode() == StickMode::Held1, "0.5 in hysteresis band: still held");
    // move + rotate the hand: stick follows rigidly
    Pose off = PMul(PInv(in.hand[1]), s.Stick());
    in.hand[1] = MakePose(QNormalize(QMul(QAxisAngle(Y(), 1.0f), in.hand[1].q)), in.hand[1].p + V3(0.2f, 0.1f, -0.3f));
    s.Step(in);
    Pose rel = PMul(PInv(in.hand[1]), s.Stick());
    CHECK(Near(rel.p, off.p, 1e-4f) && NearQ(rel.q, off.q), "HELD1 keeps stick-relative-to-hand");
    CHECK(!Near(s.Stick().p, start.p, 0.05f), "stick moved with the hand");
    in.grip[1] = 0.39f; s.Step(in);
    CHECK(s.Mode() == StickMode::Free, "0.39 < grabOff: release");
    Pose dropped = s.Stick();
    in.hand[1].p = in.hand[1].p + V3(0.5f, 0, 0); s.Step(in);
    CHECK(Near(s.Stick().p, dropped.p, 1e-6f) && NearQ(s.Stick().q, dropped.q, 1e-6f), "FREE stays put in world");

    // far hand: no grab, and squeezing then moving in does not grab (edge-triggered)
    in.hand[1] = MakePose(QIdentity(), StickPoint(s, 0.1f) + V3(0.3f, 0, 0));
    in.grip[1] = 0.9f; s.Step(in);
    CHECK(s.Mode() == StickMode::Free, "far hand does not grab");
    in.hand[1].p = StickPoint(s, 0.1f); s.Step(in);
    CHECK(s.Mode() == StickMode::Free, "held grip moving in does not grab");
    in.grip[1] = 0.1f; s.Step(in);
    in.grip[1] = 0.9f; s.Step(in);
    CHECK(s.Mode() == StickMode::Held1, "fresh squeeze grabs");
    // tracking loss releases
    in.handsOk = false; s.Step(in);
    CHECK(s.Mode() == StickMode::Free, "hands lost -> FREE");
    // grab anywhere along the shaft, within kGrabRadius only
    in.handsOk = true; in.grip[1] = 0; s.Step(in);
    in.hand[1].p = StickPoint(s, 0.8f) + V3(kGrabRadius * 0.9f, 0, 0); in.grip[1] = 1; s.Step(in);
    CHECK(s.Mode() == StickMode::Held1, "grab on the shaft at 0.8 m");
    in.grip[1] = 0; s.Step(in);
    in.hand[1].p = StickPoint(s, 0.8f) + V3(kGrabRadius * 1.1f, 0, 0); in.grip[1] = 1; s.Step(in);
    CHECK(s.Mode() == StickMode::Free, "outside kGrabRadius: no grab");
}

static void TestTwoHands() {
    StickSim s;
    s.cfg.smooth = 0;
    s.Enable(true);
    Pose head = MakePose(QIdentity(), V3(0, 1.6f, 0));
    SimInput in = MakeInput(head);
    s.Step(in);
    // right low on the handle, left 0.4 m higher on the shaft
    in.hand[1] = MakePose(QAxisAngle(X(), 0.2f), StickPoint(s, 0.10f) + V3(0.02f, 0, 0));
    in.hand[0] = MakePose(QAxisAngle(Z(), 0.5f), StickPoint(s, 0.50f) + V3(-0.02f, 0, 0.01f));
    in.grip[1] = 1; s.Step(in);
    CHECK(s.Mode() == StickMode::Held1, "first hand");
    in.grip[0] = 1; s.Step(in);
    CHECK(s.Mode() == StickMode::Held2 && s.TwoHandAxis(), "HELD2 with axis (0.4 m apart)");
    Vec3 gl[2] = {PApply(PInv(s.Stick()), in.hand[0].p), PApply(PInv(s.Stick()), in.hand[1].p)};
    // swing both hands: rotate the pair rigidly by 60 deg about Z and translate
    Quat rot = QAxisAngle(V3(0.2f, 0.3f, 1), 1.0f);
    Vec3 pivot = in.hand[1].p, shift = V3(0.1f, 0.2f, -0.1f);
    for (int h = 0; h < 2; h++) in.hand[h].p = pivot + QRot(rot, in.hand[h].p - pivot) + shift;
    in.hand[1].q = QNormalize(QMul(rot, in.hand[1].q));
    s.Step(in);
    Vec3 axis = QRot(s.Stick().q, Y()), hh;
    Normalize(in.hand[0].p - in.hand[1].p, &hh);
    Vec3 dl;
    Normalize(gl[0] - gl[1], &dl);
    CHECK(Near(QRot(s.Stick().q, dl), hh, 1e-3f), "grab-point line follows hand-to-hand line");
    CHECK(Dot(axis, hh) > 0.99f, "shaft along hand-to-hand line");
    for (int h = 0; h < 2; h++) {
        Vec3 now = PApply(PInv(s.Stick()), in.hand[h].p);
        CHECK(Near(now, gl[h], 2e-3f), "hand %d keeps its grab offset: %s vs %s", h, S(now), S(gl[h]));
    }
    // twist comes from the first hand: rotating the first hand about the shaft rolls the stick
    // pull hands apart: midpoint mapping
    Vec3 mid = (in.hand[0].p + in.hand[1].p) * 0.5f;
    in.hand[0].p = in.hand[0].p + hh * 0.05f; in.hand[1].p = in.hand[1].p - hh * 0.05f;
    s.Step(in);
    CHECK(Near(PApply(s.Stick(), (gl[0] + gl[1]) * 0.5f), mid, 1e-3f), "midpoint of grab points stays at hand midpoint");
    // release the first hand: the other takes over without a jump
    Pose before = s.Stick();
    in.grip[1] = 0; s.Step(in);
    CHECK(s.Mode() == StickMode::Held1 && s.Held(0), "left remains");
    CHECK(Near(s.Stick().p, before.p, 1e-4f) && NearQ(s.Stick().q, before.q), "no jump on release");
    Pose off = PMul(PInv(in.hand[0]), s.Stick());
    in.hand[0].p = in.hand[0].p + V3(0, 0.1f, 0); s.Step(in);
    CHECK(Near(PMul(PInv(in.hand[0]), s.Stick()).p, off.p, 1e-4f), "remaining hand drives HELD1");

    // hands close together along the shaft -> behaves like HELD1 with the first hand
    StickSim c;
    c.cfg.smooth = 0;
    c.Enable(true);
    SimInput ci = MakeInput(head);
    c.Step(ci);
    ci.hand[1] = MakePose(QIdentity(), StickPoint(c, 0.10f) + V3(0.03f, 0, 0));
    ci.hand[0] = MakePose(QIdentity(), StickPoint(c, 0.15f) + V3(-0.03f, 0, 0));
    ci.grip[1] = 1; c.Step(ci);
    ci.grip[0] = 1; c.Step(ci);
    CHECK(c.Mode() == StickMode::Held2 && !c.TwoHandAxis(), "HELD2 without axis (5 cm)");
    Pose off1 = PMul(PInv(ci.hand[1]), c.Stick());
    ci.hand[1] = MakePose(QAxisAngle(X(), 0.7f), ci.hand[1].p + V3(0, 0.1f, 0));
    ci.hand[0].p = ci.hand[0].p + V3(0.3f, 0, 0);   // second hand ignored
    c.Step(ci);
    Pose rel = PMul(PInv(ci.hand[1]), c.Stick());
    CHECK(Near(rel.p, off1.p, 1e-4f) && NearQ(rel.q, off1.q), "close hands: follows the first hand");
}

static void TestChestAim() {
    for (int i = 0; i < 50; i++) {
        StickSim s;
        s.cfg.smooth = 0;
        s.Enable(true);
        Pose head = MakePose(i == 0 ? QIdentity() : QNormalize(QMul(QAxisAngle(Y(), Rnd() * 3), QAxisAngle(X(), Rnd() * 0.5f))), RndV(5));
        SimInput in = MakeInput(head);
        s.Step(in);
        if (i > 0) s.SetStickForTest(MakePose(RndQ(), head.p + QRot(head.q, V3(Rnd() * 0.6f, Rnd() * 0.5f, 0.5f + Rnd() * 0.3f))));
        s.Step(in);
        CHECK(s.CameraValid(), "camera valid");
        Pose cam = s.Camera();
        Vec3 chest = head.p + QRot(head.q, V3(0, -0.25f, 0.05f));
        Vec3 want;
        Normalize(chest - cam.p, &want);
        CHECK(Near(cam.p, PApply(s.Stick(), V3(0, kStickLength + 0.025f, 0)), 1e-4f), "camera sits at the camera head");
        CHECK(Dot(QRot(cam.q, Z()), want) > 0.9999f, "camera +Z looks at the chest (dot %f)", Dot(QRot(cam.q, Z()), want));
        CHECK(Dot(QRot(cam.q, Y()), QRot(head.q, Y())) > 0, "camera up follows head up (no wrist roll)");
        CHECK(NearQ(s.CamHead().q, cam.q) && Near(s.CamHead().p, cam.p, 1e-4f), "drawn camera head == desktop camera");
    }
    // smoothing converges and never lags beyond the clamp
    StickSim s;
    s.cfg.smooth = 0.3f;
    s.Enable(true);
    Pose head = MakePose(QIdentity(), V3(0, 1.6f, 0));
    SimInput in = MakeInput(head);
    s.Step(in);
    in.hand[1] = MakePose(QIdentity(), StickPoint(s, 0.1f));
    in.grip[1] = 1; s.Step(in);
    in.hand[1].p = in.hand[1].p + V3(0.3f, 0.1f, 0);
    s.Step(in);
    Vec3 head3 = PApply(s.Stick(), CamHeadLocal());
    CHECK(Len(s.Camera().p - head3) <= s.cfg.maxLagPos + 1e-5f, "lag clamp: %f", Len(s.Camera().p - head3));
    Quat aim;
    s.AimTarget(in.head, &aim);
    CHECK(QAngle(s.Camera().q, aim) <= s.cfg.maxLagRad + 0.01f, "angular lag clamp");
    for (int k = 0; k < 60; k++) s.Step(in);
    CHECK(Near(s.Camera().p, head3, 1e-4f) && NearQ(s.Camera().q, aim, 1e-5f), "smoothing converges");
}

static void TestDisable() {
    StickSim s;
    s.Enable(true);
    SimInput in = MakeInput(MakePose(QIdentity(), V3(0, 1.6f, 0)));
    s.Step(in);
    CHECK(s.Visible(), "visible");
    s.Enable(false);
    s.Step(in);
    CHECK(!s.Visible() && !s.CameraValid() && s.Mode() == StickMode::Free, "disable hides and stops the camera");
    s.Enable(true);
    s.Step(in);
    CHECK(s.Visible(), "re-enable respawns");
}

// ---- v2: length ----------------------------------------------------------------
static void TestLength() {
    CHECK(Near(ClampLength(0.1f), kStickMinLen) && Near(ClampLength(9.f), kStickMaxLen) && Near(ClampLength(1.3f), 1.3f), "ClampLength");
    CHECK(Near(ClampLength(NAN), kStickLength), "ClampLength(NaN) -> default");
    CHECK(Near(StepLengthValue(1.0f, 1), 1.25f) && Near(StepLengthValue(1.0f, -1), 0.75f), "step +/- 0.25");
    CHECK(Near(StepLengthValue(1.0f, 5), 1.25f) && Near(StepLengthValue(1.0f, -7), 0.75f), "only the sign of dir counts");
    CHECK(Near(StepLengthValue(1.0f, 0), 1.0f), "dir 0 keeps the value");
    CHECK(Near(StepLengthValue(kStickMaxLen, 1), kStickMaxLen) && Near(StepLengthValue(kStickMinLen, -1), kStickMinLen), "clamped at the ends");
    CHECK(Near(StepLengthValue(1.13f, 1), 1.25f) && Near(StepLengthValue(1.13f, -1), 1.0f), "off-grid values step to the next grid point");
    float v = kStickMinLen;
    int n = 0;
    while (v < kStickMaxLen && n < 100) { v = StepLengthValue(v, 1); n++; }
    CHECK(n == 6 && v == kStickMaxLen, "0.5 -> 2.0 in exactly 6 steps without drift (%d, %.7f)", n, v);
    for (int k = 0; k < 6; k++) v = StepLengthValue(v, -1);
    CHECK(v == kStickMinLen, "and back exactly");

    // geometry follows the length
    for (float len : {0.5f, 1.0f, 1.5f, 2.0f}) {
        StickSim s;
        s.cfg.smooth = 0;
        s.SetLength(len);
        Pose head = MakePose(QAxisAngle(Y(), 0.7f), V3(1, 1.6f, 2));
        s.Enable(true);
        SimInput in = MakeInput(head);
        s.Step(in);
        s.Step(in);
        CHECK(Near(s.Length(), len), "length set");
        CHECK(Near(s.CamHead().p, StickPoint(s, len + 0.025f), 1e-4f), "camera head at length + 0.025 (len %.2f)", len);
        CHECK(Near(s.Camera().p, StickPoint(s, len + 0.025f), 1e-4f), "desktop camera at the camera head (len %.2f)", len);
        // grab segment: near the top grabs, just beyond the top does not
        SimInput g = in;
        g.hand[1] = MakePose(QIdentity(), StickPoint(s, len - 0.05f) + QRot(s.Stick().q, V3(0.05f, 0, 0)));
        g.grip[1] = 1; s.Step(g);
        CHECK(s.Mode() == StickMode::Held1, "grab near the top of a %.2f m stick", len);
        g.grip[1] = 0; s.Step(g);
        g.hand[1].p = StickPoint(s, len + kGrabRadius + 0.03f);
        g.grip[1] = 1; s.Step(g);
        CHECK(s.Mode() == StickMode::Free, "no grab above the top of a %.2f m stick", len);
    }
    // a 1.0 m stick cannot be grabbed at 1.4 m; a 1.5 m one can
    {
        StickSim s; s.cfg.smooth = 0; s.Enable(true);
        SimInput in = MakeInput(MakePose(QIdentity(), V3(0, 1.6f, 0)));
        s.Step(in);
        in.hand[1].p = StickPoint(s, 1.4f); in.grip[1] = 1; s.Step(in);
        CHECK(s.Mode() == StickMode::Free, "1.4 m is beyond a 1.0 m shaft");
        in.grip[1] = 0; s.Step(in);
        s.SetLength(1.5f);
        in.grip[1] = 1; s.Step(in);
        CHECK(s.Mode() == StickMode::Held1, "1.4 m is on a 1.5 m shaft");
    }
    // two hands far apart on a 2 m stick
    {
        StickSim s; s.cfg.smooth = 0; s.SetLength(2.0f); s.Enable(true);
        SimInput in = MakeInput(MakePose(QIdentity(), V3(0, 1.6f, 0)));
        s.Step(in);
        in.hand[1] = MakePose(QIdentity(), StickPoint(s, 0.1f) + V3(0.02f, 0, 0));
        in.hand[0] = MakePose(QIdentity(), StickPoint(s, 1.8f) + V3(-0.02f, 0, 0));
        in.grip[1] = 1; s.Step(in);
        in.grip[0] = 1; s.Step(in);
        CHECK(s.Mode() == StickMode::Held2 && s.TwoHandAxis(), "HELD2 with hands 1.7 m apart on a 2 m stick");
        Vec3 gl0 = PApply(PInv(s.Stick()), in.hand[0].p);
        Quat rot = QAxisAngle(V3(1, 0, 0.3f), 0.8f);
        Vec3 pivot = in.hand[1].p;
        in.hand[0].p = pivot + QRot(rot, in.hand[0].p - pivot);
        in.hand[1].q = QNormalize(QMul(rot, in.hand[1].q));
        s.Step(in);
        CHECK(Near(PApply(PInv(s.Stick()), in.hand[0].p), gl0, 2e-3f), "upper hand keeps its grab point on the long stick");
        CHECK(Near(s.CamHead().p, StickPoint(s, 2.025f), 1e-4f), "camera head follows the 2 m length");
    }
    // changing the length keeps the butt where it is
    {
        StickSim s; s.cfg.smooth = 0; s.Enable(true);
        SimInput in = MakeInput(MakePose(QIdentity(), V3(0, 1.6f, 0)));
        s.Step(in);
        Pose before = s.Stick();
        s.SetLength(1.75f); s.Step(in);
        CHECK(Near(s.Stick().p, before.p, 1e-6f) && NearQ(s.Stick().q, before.q), "length change keeps the butt");
        CHECK(Near(s.CamHead().p, StickPoint(s, 1.775f), 1e-4f), "camera head moved up");
    }
}

// ---- v2: FOV ---------------------------------------------------------------------
static void TestFov() {
    CHECK(Near(ClampFov(10), kFovMin) && Near(ClampFov(500), kFovMax) && Near(ClampFov(95), 95), "ClampFov");
    CHECK(Near(StepFovValue(112, 1), 120) && Near(StepFovValue(112, -1), 110), "112 steps onto the 10 deg grid");
    CHECK(Near(StepFovValue(110, -1), 100) && Near(StepFovValue(120, 1), 130), "on-grid FOV moves one step");
    CHECK(Near(StepFovValue(kFovMax, 1), kFovMax) && Near(StepFovValue(kFovMin, -1), kFovMin), "FOV clamped");
    CHECK(Near(StepFovValue(NAN, 1), 120), "NaN treated as 112");
    FovKeeper k;
    float w = -1;
    CHECK(!k.Update(true, 112, NAN, &w) && !k.saved, "enabled, no target: never touch the game value");
    CHECK(k.Update(true, 112, 90, &w) && w == 90 && k.saved && k.original == 112, "first override saves the original");
    CHECK(!k.Update(true, 90, 90, &w), "already at the target: no write");
    CHECK(k.Update(true, 90, 300, &w) && w == kFovMax, "target clamped");
    CHECK(k.Update(true, 130, 70, &w) && w == 70 && k.original == 112, "original kept across changes");
    CHECK(k.Update(false, 70, 70, &w) && w == 112 && !k.saved, "disable restores the original");
    CHECK(!k.Update(false, 112, 70, &w), "disabled and restored: hands off");
    CHECK(!k.Update(true, NAN, 90, &w) && !k.Update(true, 0.0f, 90, &w) && !k.Update(true, 1e6f, 90, &w) && !k.saved,
          "implausible game value: never written");
    CHECK(k.Update(true, 100, 80, &w) && k.original == 100, "re-enable saves the game's current value");
    CHECK(k.Update(false, 80, 80, &w) && w == 100, "and restores that one");
    FovKeeper k2;
    CHECK(k2.Update(true, 112, 112, &w) == false && k2.saved && !k2.Update(false, 112, 112, &w) && !k2.saved,
          "target equal to the game value: saved, nothing to write either way");
}

// ---- v2 / v2.1: shutter, self-timer, flash --------------------------------------
struct ShutterRig {
    StickSim s;
    SimInput in;
    double t = 100.0;
    ShutterRig() {
        s.cfg.smooth = 0;
        s.Enable(true);
        in = MakeInput(MakePose(QIdentity(), V3(0, 1.6f, 0)));
        in.time = t;
        s.Step(in);
    }
    // place hand h so its fingertip is `d` metres from the button, approaching along the button normal
    void Tip(int h, float d, Quat q = QIdentity()) {
        Vec3 normal = QRot(s.Stick().q, Z());
        Vec3 tip = s.ShutterWorld() + normal * d;
        in.hand[h] = MakePose(q, tip - QRot(q, s.cfg.tipOffset));
    }
    void Step(double dt = 1.0 / 90) { t += dt; in.time = t; s.Step(in); }
    void StepTo(double when) { t = when; in.time = t; s.Step(in); }
    void Poke(int h) { Tip(h, 0.2f); Step(); Tip(h, 0.0f); Step(); }   // approach from outside, enter
};

static void TestShutter() {
    {   // geometry
        ShutterRig r;
        CHECK(Near(r.s.ShutterWorld(), PApply(r.s.Stick(), V3(0, kShutterY, kHandleRadius))), "button at (0, kShutterY, kHandleRadius)");
        CHECK(Dot(QRot(r.s.Stick().q, Z()), V3(0, 0, -1)) > 0.999f, "button faces the user at spawn");
        Pose hand = MakePose(QAxisAngle(Y(), 1.5707963f), V3(1, 1, 1));
        CHECK(Near(r.s.Fingertip(hand), V3(1, 1, 1) + QRot(hand.q, r.s.cfg.tipOffset)), "fingertip = hand * tipOffset");
        CHECK(Near(r.s.cfg.tipOffset, V3(0, -0.02f, 0.07f)), "default tip offset");
    }
    {   // hot radius
        ShutterRig r;
        r.Tip(0, kShutterHot + 0.01f); r.Step();
        CHECK(!r.s.ShutterHot(), "not hot outside kShutterHot");
        r.Tip(0, kShutterHot - 0.005f); r.Step();
        CHECK(r.s.ShutterHot() && !r.s.CountdownActive(), "hot inside kShutterHot, not pressed");
        r.Tip(0, kShutterPress + 0.005f); r.Step();
        CHECK(r.s.ShutterHot() && !r.s.CountdownActive(), "just outside kShutterPress: still not pressed");
        r.Tip(0, kShutterPress - 0.002f); r.Step();
        CHECK(r.s.CountdownActive() && r.s.Presses() == 1, "inside kShutterPress: pressed");
    }
    {   // holding hand excluded
        ShutterRig r;
        r.Tip(1, 0.0f); r.in.grip[1] = 1; r.Step();   // grabs (the fingertip sits on the button, hand on the handle)
        CHECK(r.s.Held(1), "right hand holds");
        r.Tip(1, 0.2f); r.Step(); r.Tip(1, 0.0f); r.Step();
        CHECK(!r.s.CountdownActive() && r.s.Presses() == 0 && !r.s.ShutterHot(), "the holding hand cannot fire or light it");
        r.in.grip[1] = 0; r.Step();
        CHECK(!r.s.Held(1) && !r.s.CountdownActive(), "releasing the grip with the finger on the button does not fire");
        r.Poke(0);
        CHECK(r.s.CountdownActive(), "the other hand fires");
    }
    {   // pointing requirement
        ShutterRig r;
        r.in.touchesKnown[0] = true; r.in.pointing[0] = false;
        r.Poke(0);
        CHECK(!r.s.CountdownActive() && r.s.Presses() == 0, "Touches known and not pointing: no fire");
        r.in.pointing[0] = true; r.Step();
        CHECK(!r.s.CountdownActive(), "starting to point while inside does not fire (entry edge only)");
        r.Poke(0);
        CHECK(r.s.CountdownActive(), "pointing poke fires");
        ShutterRig u;
        u.in.touchesKnown[1] = false; u.in.pointing[1] = false;
        u.Poke(1);
        CHECK(u.s.CountdownActive(), "no Touches data: the pointing bit is not required");
    }
    {   // rising edge + hysteresis
        ShutterRig r;
        r.Poke(0);
        for (int i = 0; i < 20; i++) r.Step();
        CHECK(r.s.Presses() == 1, "staying inside is one press");
        r.Tip(0, kShutterPress + 0.005f); r.Step(); r.Tip(0, 0.0f); r.Step();
        CHECK(r.s.Presses() == 1, "jitter inside the re-arm band is not a new press");
        r.Tip(0, kShutterPress + 0.03f); r.Step(); r.Tip(0, 0.0f); r.Step();
        CHECK(r.s.Presses() == 2, "leaving and re-entering is a new press");
    }
    {   // the self-timer timeline
        ShutterRig r;
        r.Tip(0, 0.2f); r.StepTo(10.0 - 0.01);
        r.Tip(0, 0.0f); r.StepTo(10.0);
        CHECK(r.s.CountdownActive() && r.s.CountdownDigit() == 3 && Near(r.s.CountdownAlpha(), 1.0f), "t=0: digit 3, alpha 1");
        CHECK(r.s.TakeDigitTick() == 3 && r.s.TakeDigitTick() == 0, "tick for 3, once");
        r.Tip(0, 0.3f);
        r.StepTo(10.5);
        CHECK(r.s.CountdownDigit() == 3 && Near(r.s.CountdownAlpha(), 0.25f), "t=0.5: digit 3, alpha 0.25 (ease-out)");
        r.StepTo(10.9);
        CHECK(r.s.CountdownDigit() == 3 && r.s.CountdownAlpha() < 0.02f, "t=0.9: nearly faded");
        r.StepTo(11.0);
        CHECK(r.s.CountdownDigit() == 2 && Near(r.s.CountdownAlpha(), 1.0f) && r.s.TakeDigitTick() == 2, "t=1: digit 2 pops in");
        // a press during the countdown is ignored
        r.Tip(0, 0.0f); r.StepTo(11.5);
        CHECK(r.s.Presses() == 2 && r.s.CountdownDigit() == 2 && Near(r.s.CountdownAlpha(), 0.25f), "press during the countdown ignored");
        r.Tip(0, 0.3f);
        r.StepTo(12.0);
        CHECK(r.s.CountdownDigit() == 1 && r.s.TakeDigitTick() == 1, "t=2: digit 1");
        r.StepTo(12.999);
        CHECK(r.s.CountdownDigit() == 1 && !r.s.TakeShot() && Near(r.s.Flash(), 0.0f), "t=2.999: no shot yet");
        r.StepTo(13.0);
        CHECK(!r.s.CountdownActive() && r.s.CountdownDigit() == 0 && r.s.CountdownAlpha() == 0, "t=3: countdown over");
        CHECK(r.s.TakeShot() && !r.s.TakeShot() && r.s.Shots() == 1, "t=3: exactly one shot");
        CHECK(Near(r.s.Flash(), 1.0f), "flash starts at 1");
        r.StepTo(13.125);
        CHECK(Near(r.s.Flash(), 0.5f, 1e-3f), "flash 0.5 at 125 ms");
        r.StepTo(13.25);
        CHECK(r.s.Flash() == 0.0f, "flash gone at 250 ms");
        for (int i = 1; i <= 20; i++) r.StepTo(13.25 + 0.01 * i);
        CHECK(!r.s.TakeShot() && r.s.Shots() == 1, "never a second shot");
        // cooldown: 1 s after the shot
        r.Tip(0, 0.2f); r.StepTo(13.5); r.Tip(0, 0.0f); r.StepTo(13.6);
        CHECK(!r.s.CountdownActive(), "press 0.6 s after the shot ignored (cooldown)");
        r.Tip(0, 0.2f); r.StepTo(14.05); r.Tip(0, 0.0f); r.StepTo(14.1);
        CHECK(r.s.CountdownActive(), "press after the cooldown starts a new countdown");
    }
    {   // the shot fires once even with coarse frames, at the first step >= 3 s
        ShutterRig r;
        r.Tip(0, 0.2f); r.StepTo(50.0); r.Tip(0, 0.0f); r.StepTo(50.01);
        int shots = 0;
        double firedAt = 0;
        for (double tt = 50.01; tt < 56; tt += 0.07) {
            r.StepTo(tt);
            if (r.s.TakeShot()) { shots++; firedAt = tt; }
        }
        CHECK(shots == 1 && firedAt >= 53.01 && firedAt < 53.01 + 0.0701, "one shot at ~3 s with 70 ms frames (%d at %.3f)", shots, firedAt);
    }
    {   // cancel on disable
        ShutterRig r;
        r.Poke(0);
        r.StepTo(r.t + 1.5);
        CHECK(r.s.CountdownActive(), "running");
        r.s.Enable(false);
        r.StepTo(r.t + 0.1);
        CHECK(!r.s.CountdownActive() && r.s.CountdownDigit() == 0, "disable cancels the countdown");
        r.s.Enable(true);
        for (int i = 0; i < 400; i++) r.Step(0.01);
        CHECK(!r.s.TakeShot() && r.s.Shots() == 0 && !r.s.CountdownActive(), "no shot after a cancelled countdown, none after re-enable");
    }
    {   // billboard: faces the head, head up, above the camera head, follows the stick
        ShutterRig r;
        r.in.head = MakePose(QNormalize(QMul(QAxisAngle(Y(), 0.6f), QAxisAngle(X(), 0.25f))), V3(0.3f, 1.65f, 0.1f));
        r.Step();
        r.Poke(0);
        r.Step();
        CHECK(r.s.CountdownPoseValid(), "digit pose valid");
        Pose d = r.s.CountdownPose();
        Vec3 up = QRot(r.in.head.q, Y()), toHead;
        Normalize(r.in.head.p - d.p, &toHead);
        CHECK(Dot(QRot(d.q, Z()), toHead) > 0.9999f, "digit +Z faces the head");
        Vec3 upProj;
        Normalize(up - toHead * Dot(up, toHead), &upProj);
        CHECK(Near(QRot(d.q, Y()), upProj, 1e-3f), "digit +Y is head up (projected onto the digit plane)");
        CHECK(Dot(Cross(QRot(d.q, X()), QRot(d.q, Y())), QRot(d.q, Z())) > 0.999f, "digit basis right-handed (viewer's right = +X)");
        CHECK(Near(d.p, r.s.CamHead().p + up * kCountdownLift, 1e-4f), "digit 0.12 m above the camera head");
        // hold the stick with the right hand and move it during the countdown
        r.in.hand[1] = MakePose(QIdentity(), StickPoint(r.s, 0.1f));
        r.in.grip[1] = 1; r.Step();
        CHECK(r.s.Held(1) && r.s.CountdownActive(), "grabbed during the countdown, still counting");
        r.in.hand[1] = MakePose(QAxisAngle(Z(), 0.4f), r.in.hand[1].p + V3(0.3f, 0.2f, -0.1f));
        r.Step();
        d = r.s.CountdownPose();
        Normalize(r.in.head.p - d.p, &toHead);
        CHECK(Near(d.p, r.s.CamHead().p + up * kCountdownLift, 1e-4f), "digit follows the moved stick");
        CHECK(Dot(QRot(d.q, Z()), toHead) > 0.9999f, "and still faces the head");
        // head lost: keep facing, keep following
        Quat keep = d.q;
        r.in.headOk = false;
        r.in.hand[1].p = r.in.hand[1].p + V3(0, 0.1f, 0);
        r.Step();
        CHECK(NearQ(r.s.CountdownPose().q, keep) && Near(r.s.CountdownPose().p, r.s.CamHead().p + QRot(keep, Y()) * kCountdownLift, 1e-4f),
              "no head: digit keeps its facing and follows the stick");
    }
    // pure timeline helpers
    CHECK(CountdownDigitAt(-0.1) == 0 && CountdownDigitAt(0) == 3 && CountdownDigitAt(0.999) == 3 && CountdownDigitAt(1) == 2 &&
          CountdownDigitAt(2.5) == 1 && CountdownDigitAt(3) == 0, "CountdownDigitAt");
    float prev = 2;
    bool mono = true;
    for (int i = 0; i < 100; i++) { float a = CountdownAlphaAt(1 + i / 100.0); if (a > prev + 1e-6f) mono = false; prev = a; }
    CHECK(mono && Near(CountdownAlphaAt(1.0), 1.0f) && CountdownAlphaAt(3.0) == 0, "alpha fades monotonically within a digit");
    CHECK(FlashAt(-1) == 0 && Near(FlashAt(0), 1) && FlashAt(0.3) == 0, "FlashAt");
}

int main() {
    TestQuatConventions();
    TestLookAt();
    TestOvrEngine();
    TestWorldTrack();
    TestSpawn();
    TestGrabHysteresis();
    TestTwoHands();
    TestChestAim();
    TestDisable();
    TestLength();
    TestFov();
    TestShutter();
    printf("selfie_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
