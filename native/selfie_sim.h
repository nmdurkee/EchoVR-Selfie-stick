// selfie_sim.h - the selfie stick state machine (FREE / HELD1 / HELD2), pure math.
// No engine access and no threading here: selfie_core.cpp feeds it world-space poses
// and grip values once per frame (under its lock) and publishes the result. The unit
// tests drive it directly. See ../DESIGN.md sections 3 and 7.
//
// v2: adjustable length, shutter button (poke with the other hand's index fingertip),
// v2.1: a 3-second self-timer started by the shutter press, lens flash after the shot.
// Also the pure FOV helpers (clamp / step / save-and-restore of the engine's capturefov).
#pragma once
#include "selfie_math.h"

namespace selfie {

enum class StickMode { Free = 0, Held1 = 1, Held2 = 2 };
inline const char* ModeName(StickMode m) {
    return m == StickMode::Free ? "FREE" : m == StickMode::Held1 ? "HELD1" : "HELD2";
}

// Grid index after one step from position g (in steps): up = next grid point above, down = next below,
// 0 = nearest. On-grid values move by exactly one step.
inline float GridStep(float g, int dir) {
    if (dir > 0) return floorf(g + 1e-3f) + 1.0f;
    if (dir < 0) return ceilf(g - 1e-3f) - 1.0f;
    return roundf(g);
}

// ---- length ---------------------------------------------------------------------
inline float ClampLength(float v) {
    if (!Finite(v)) return kStickLength;
    return v < kStickMinLen ? kStickMinLen : v > kStickMaxLen ? kStickMaxLen : v;
}
// One step of kStickLenStep in direction dir (sign only), snapped to the step grid, clamped.
inline float StepLengthValue(float cur, int dir) {
    cur = ClampLength(cur);
    return ClampLength(kStickMinLen + GridStep((cur - kStickMinLen) / kStickLenStep, dir) * kStickLenStep);
}

// ---- FOV (degrees, the engine's capturefov) ----------------------------------------
inline float ClampFov(float v) {
    if (!Finite(v)) return 112.0f;
    return v < kFovMin ? kFovMin : v > kFovMax ? kFovMax : v;
}
inline float StepFovValue(float cur, int dir) {
    cur = ClampFov(cur);
    return ClampFov(kFovMin + GridStep((cur - kFovMin) / kFovStep, dir) * kFovStep);
}
// A plausible capturefov value read from the game (anything else: do not touch the field).
inline bool PlausibleFov(float v) { return Finite(v) && v >= 1.0f && v <= 179.0f; }

// Remembers the game's own capturefov the first time we override it and gives it back on disable.
// Update() is called every frame with the value currently in the game; it returns true and sets
// *write when the field must be written.
struct FovKeeper {
    bool saved = false;
    float original = 112.0f;
    bool Update(bool enabled, float current, float target, float* write) {
        if (!PlausibleFov(current)) return false;                 // not the field we expect: hands off
        if (enabled) {
            if (!Finite(target)) return false;                    // nothing requested: leave the game's value
            float want = ClampFov(target);
            if (!saved) { original = current; saved = true; }
            if (current == want) return false;
            *write = want;
            return true;
        }
        if (!saved) return false;
        saved = false;
        if (current == original) return false;
        *write = original;
        return true;
    }
};

// ---- stick-local points -------------------------------------------------------------
constexpr float kCamHeadY = kStickLength + 0.025f;              // camera box centre at the default length
inline Vec3 CamHeadLocal(float length = kStickLength) { return V3(0, ClampLength(length) + 0.025f, 0); }
inline Vec3 ShutterLocal() { return V3(0, kShutterY, kHandleRadius); }
// Spawn: handle centre this far from the head, in head-local space (+Z forward, +Y up).
inline Vec3 SpawnHandleLocal() { return V3(0, -0.25f, 0.5f); }
// Aim point: top of the chest, in head-local space (DESIGN section 3).
inline Vec3 ChestLocal() { return V3(0, -0.25f, 0.05f); }
constexpr float kCountdownLift = 0.12f;   // digit centre above the camera head, along head up
constexpr float kFlashSeconds = 0.25f;
constexpr float kShutterCooldown = 1.0f;

// Self-timer timeline (pure): elapsed seconds since the press -> digit (3,2,1; 0 = done) and alpha.
// Each digit pops in at full opacity and fades out over its second with an ease-out: (1 - f)^2.
inline int CountdownDigitAt(double elapsed) {
    if (!(elapsed >= 0) || elapsed >= kCountdownSeconds) return 0;
    return 3 - static_cast<int>(floor(elapsed));
}
inline float CountdownAlphaAt(double elapsed) {
    if (!(elapsed >= 0) || elapsed >= kCountdownSeconds) return 0.0f;
    float f = static_cast<float>(elapsed - floor(elapsed));
    return (1.0f - f) * (1.0f - f);
}
inline float FlashAt(double sinceShot) {
    if (!(sinceShot >= 0) || sinceShot >= kFlashSeconds) return 0.0f;
    return 1.0f - static_cast<float>(sinceShot) / kFlashSeconds;
}

struct SimConfig {
    float grabOn = 0.6f;          // grip rises above -> grab (if near)
    float grabOff = 0.4f;         // grip falls below -> release
    float grabRadius = kGrabRadius;
    float twoHandMinSep = 0.10f;  // along-shaft hand separation needed for the two-hand axis
    float smooth = 0.3f;          // camera smoothing: 0 = none, 0.95 max (fraction of the old value kept per step)
    float maxLagPos = 0.02f;      // camera may trail the drawn camera head by at most this (m)
    float maxLagRad = 0.14f;      // ... and by at most this angle (~8 deg)
    // Index fingertip in CONTROLLER-LOCAL engine space (+Z = pointing direction, +Y = up, metres).
    // The Touch pose origin sits in the middle of the grip just below the tracking ring; with the
    // index extended the tip is ~7 cm ahead of it along the pointing axis and ~2 cm lower.
    Vec3 tipOffset = {0.0f, -0.02f, 0.07f};
    float pressRadius = kShutterPress;
    float rearmMargin = 0.015f;   // the fingertip must leave to pressRadius + this before another poke counts
    float hotRadius = kShutterHot;
};

struct SimInput {
    bool headOk = false;
    Pose head = PIdentity();      // world
    bool handsOk = false;
    Pose hand[2] = {PIdentity(), PIdentity()};   // world, 0 = left, 1 = right
    float grip[2] = {0, 0};                      // HandTrigger, 0..1
    // v2
    double time = 0;                             // seconds, monotonic
    bool touchesKnown[2] = {false, false};       // Touch capacitive data seen recently for this hand
    bool pointing[2] = {false, false};           // ovrTouch_*IndexPointing
};

class StickSim {
public:
    SimConfig cfg;

    // Enable(true) (re)spawns the stick on the next Step that has a head pose.
    void Enable(bool on) {
        enabled_ = on;
        spawnPending_ = on;
        CancelCountdown();
        flashStart_ = -1e9;
        shutterHot_ = false;
        armed_[0] = armed_[1] = false;   // a finger already on the button must leave first
        if (!on) {
            visible_ = false;
            held_[0] = held_[1] = false;
            camValid_ = false;
        }
    }
    bool Enabled() const { return enabled_; }
    void SetLength(float len) { length_ = ClampLength(len); }
    float Length() const { return length_; }

    void Step(const SimInput& in) {
        highlight_ = false;
        shutterHot_ = false;
        now_ = in.time;
        if (!enabled_) { visible_ = false; return; }
        if (spawnPending_) {
            if (!in.headOk) { visible_ = false; return; }
            Spawn(in.head);
            spawnPending_ = false;
        }

        // grip hysteresis -> edges
        bool rising[2] = {false, false}, falling[2] = {false, false};
        for (int h = 0; h < 2; h++) {
            float g = in.handsOk && Finite(in.grip[h]) ? in.grip[h] : 0.0f;
            bool was = down_[h];
            if (!down_[h] && g > cfg.grabOn) down_[h] = true;
            else if (down_[h] && g < cfg.grabOff) down_[h] = false;
            rising[h] = down_[h] && !was;
            falling[h] = !down_[h] && was;
        }
        // releases first (a lost hand releases too), then grabs
        for (int h = 0; h < 2; h++)
            if (held_[h] && (falling[h] || !in.handsOk)) Release(h, in);
        for (int h = 0; h < 2; h++)
            if (rising[h] && !held_[h] && in.handsOk && HandDistance(in.hand[h].p) <= cfg.grabRadius) Grab(h, in);

        if (in.handsOk) UpdateHeld(in);

        if (in.handsOk)
            for (int h = 0; h < 2; h++)
                if (HandDistance(in.hand[h].p) <= cfg.grabRadius) highlight_ = true;

        UpdateCamera(in);
        UpdateShutter(in);
        UpdateCountdown(in);
        visible_ = true;
    }

    // ---- outputs (world space) ----
    bool Visible() const { return visible_; }
    StickMode Mode() const {
        int n = (held_[0] ? 1 : 0) + (held_[1] ? 1 : 0);
        return n == 0 ? StickMode::Free : n == 1 ? StickMode::Held1 : StickMode::Held2;
    }
    bool Held(int h) const { return held_[h]; }
    bool TwoHandAxis() const { return Mode() == StickMode::Held2 && twoAxis_; }
    bool Highlight() const { return highlight_; }
    const Pose& Stick() const { return stick_; }
    // Desktop camera: smoothed, aimed at the chest. Valid once a head pose was seen.
    bool CameraValid() const { return visible_ && camValid_; }
    Pose Camera() const { return MakePose(camQ_, camP_); }
    // Drawn camera head: exactly on the stick, rotated like the desktop camera.
    Pose CamHead() const { return MakePose(camValid_ ? camQ_ : stick_.q, PApply(stick_, CamHeadLocal(length_))); }
    // Unsmoothed aim for the current stick and head (tests / diagnostics).
    bool AimTarget(const Pose& head, Quat* out) const { return Aim(head, PApply(stick_, CamHeadLocal(length_)), out); }
    // Shutter button centre (world) and the fingertip estimate for a hand pose (world).
    Vec3 ShutterWorld() const { return PApply(stick_, ShutterLocal()); }
    Vec3 Fingertip(const Pose& hand) const { return PApply(hand, cfg.tipOffset); }
    bool ShutterHot() const { return shutterHot_; }
    // Self-timer.
    bool CountdownActive() const { return countdownActive_; }
    int CountdownDigit() const { return countdownActive_ ? CountdownDigitAt(now_ - countdownStart_) : 0; }
    float CountdownAlpha() const { return countdownActive_ ? CountdownAlphaAt(now_ - countdownStart_) : 0.0f; }
    bool CountdownPoseValid() const { return countdownActive_ && digitValid_; }
    const Pose& CountdownPose() const { return digitPose_; }
    float Flash() const { return FlashAt(now_ - flashStart_); }
    // Events, each reported once: the shot (countdown finished) and a new digit (3, 2, 1) for the tick sound.
    bool TakeShot() { bool s = shotEvent_; shotEvent_ = false; return s; }
    int TakeDigitTick() { int d = tickEvent_; tickEvent_ = 0; return d; }
    uint32_t Presses() const { return presses_; }
    uint32_t Shots() const { return shots_; }

    // Test hook: place the stick directly.
    void SetStickForTest(const Pose& p) { stick_ = p; }

private:
    bool enabled_ = false, spawnPending_ = false, visible_ = false, highlight_ = false;
    Pose stick_ = PIdentity();
    float length_ = kStickLength;
    bool down_[2] = {false, false};
    bool held_[2] = {false, false};
    int first_ = 1;                       // the hand that grabbed first (drives HELD1 / fallback)
    Pose off_[2] = {PIdentity(), PIdentity()};   // stick relative to hand: stick = hand * off
    Vec3 local_[2] = {{0, 0, 0}, {0, 0, 0}};     // HELD2: each hand's grab point in stick-local
    bool twoAxis_ = false;
    bool camValid_ = false;
    Quat camQ_ = {0, 0, 0, 1};
    Vec3 camP_ = {0, 0, 0};
    // shutter / self-timer
    double now_ = 0;
    bool shutterHot_ = false;
    bool armed_[2] = {false, false};
    bool countdownActive_ = false;
    double countdownStart_ = 0;
    int lastDigit_ = 0;
    double cooldownUntil_ = -1e9;
    double flashStart_ = -1e9;
    bool shotEvent_ = false;
    int tickEvent_ = 0;
    bool digitValid_ = false;
    Pose digitPose_ = PIdentity();
    uint32_t presses_ = 0, shots_ = 0;

    void CancelCountdown() {
        countdownActive_ = false;
        lastDigit_ = 0;
        shotEvent_ = false;
        tickEvent_ = 0;
        digitValid_ = false;
    }

    float HandDistance(Vec3 p) const {
        Vec3 a = stick_.p, b = PApply(stick_, V3(0, length_, 0));
        return SegmentDistance(p, a, b);
    }

    void Spawn(const Pose& head) {
        // shaft along head-up, lens (local +Z) facing the user: head * Ry(180)
        Quat q = QNormalize(QMul(head.q, Quat{0, 1, 0, 0}));
        Vec3 handleCentre = PApply(head, SpawnHandleLocal());
        stick_ = MakePose(q, handleCentre - QRot(q, V3(0, kHandleLength * 0.5f, 0)));
        held_[0] = held_[1] = false;
        twoAxis_ = false;
        camValid_ = false;
    }

    void Grab(int h, const SimInput& in) {
        int o = 1 - h;
        held_[h] = true;
        off_[h] = PMul(PInv(in.hand[h]), stick_);
        if (!held_[o]) { first_ = h; return; }
        // becoming HELD2: remember both grab points on the stick
        Pose inv = PInv(stick_);
        local_[h] = PApply(inv, in.hand[h].p);
        local_[o] = PApply(inv, in.hand[o].p);
        twoAxis_ = fabsf(local_[h].y - local_[o].y) > cfg.twoHandMinSep;
    }

    void Release(int h, const SimInput& in) {
        held_[h] = false;
        int o = 1 - h;
        if (held_[o]) {   // the remaining hand takes over from where the stick is now: no jump
            first_ = o;
            if (in.handsOk) off_[o] = PMul(PInv(in.hand[o]), stick_);
        }
        twoAxis_ = false;
    }

    void UpdateHeld(const SimInput& in) {
        StickMode m = Mode();
        if (m == StickMode::Free) return;
        int f = held_[first_] ? first_ : 1 - first_;
        Pose pred = PMul(in.hand[f], off_[f]);
        if (m == StickMode::Held1 || !twoAxis_) { stick_ = NormalizedPose(pred); return; }
        // HELD2 with the two-hand axis: keep both grab points, twist from the first hand
        int lo = local_[0].y <= local_[1].y ? 0 : 1, hi = 1 - lo;
        Vec3 dl, dw;
        if (!Normalize(local_[hi] - local_[lo], &dl) || !Normalize(in.hand[hi].p - in.hand[lo].p, &dw)) {
            stick_ = NormalizedPose(pred);
            return;
        }
        Quat swing = QFromTo(QRot(pred.q, dl), dw);
        Quat q = QNormalize(QMul(swing, pred.q));
        Vec3 midL = (local_[0] + local_[1]) * 0.5f;
        Vec3 midW = (in.hand[0].p + in.hand[1].p) * 0.5f;
        stick_ = MakePose(q, midW - QRot(q, midL));
    }

    static Pose NormalizedPose(const Pose& p) { return MakePose(QNormalize(p.q), p.p); }

    static bool Aim(const Pose& head, Vec3 from, Quat* out) {
        Vec3 chest = PApply(head, ChestLocal());
        Vec3 up = QRot(head.q, V3(0, 1, 0));
        if (LookAt(chest - from, up, out)) return true;
        return LookAt(chest - from, QRot(head.q, V3(0, 0, 1)), out);   // looking straight down/up the head axis
    }

    void UpdateCamera(const SimInput& in) {
        Vec3 target = PApply(stick_, CamHeadLocal(length_));
        Quat aim = camQ_;
        bool haveAim = in.headOk && Aim(in.head, target, &aim);
        if (!haveAim) {
            if (!camValid_) return;   // nothing to aim at yet
            aim = camQ_;              // keep the last aim, follow the stick
        }
        if (!camValid_) { camQ_ = aim; camP_ = target; camValid_ = true; return; }
        float s = cfg.smooth < 0 ? 0 : cfg.smooth > 0.95f ? 0.95f : cfg.smooth;
        float a = 1.0f - s;                                   // weight of the new sample
        camP_ = Lerp(camP_, target, a);
        camQ_ = QNlerp(camQ_, aim, a);
        // never let the camera visibly disagree with the drawn stick
        Vec3 d = camP_ - target;
        float dl = Len(d);
        if (dl > cfg.maxLagPos) camP_ = target + d * (cfg.maxLagPos / dl);
        float ang = QAngle(camQ_, aim);
        if (ang > cfg.maxLagRad) camQ_ = QNlerp(aim, camQ_, cfg.maxLagRad / ang);
        if (!Finite(camP_) || !Finite(camQ_)) { camP_ = target; camQ_ = aim; }
    }

    // Poke detection. A hand fires on the frame its fingertip ENTERS pressRadius, if it is not holding
    // the stick, is pointing (when Touch capacitive data is known) and was armed (its fingertip left to
    // pressRadius + rearmMargin since the last entry). A press starts the self-timer if none runs and
    // the post-shot cooldown is over; otherwise it is ignored.
    void UpdateShutter(const SimInput& in) {
        const Vec3 button = ShutterWorld();
        for (int h = 0; h < 2; h++) {
            if (!in.handsOk || held_[h]) { armed_[h] = false; continue; }
            Vec3 tip = Fingertip(in.hand[h]);
            float d = Len(tip - button);
            if (!Finite(d)) { armed_[h] = false; continue; }
            if (d <= cfg.hotRadius) shutterHot_ = true;
            if (d > cfg.pressRadius + cfg.rearmMargin) { armed_[h] = true; continue; }
            if (d > cfg.pressRadius) continue;            // hysteresis band: keep the armed state
            bool pointingOk = !in.touchesKnown[h] || in.pointing[h];
            bool fire = armed_[h] && pointingOk;
            armed_[h] = false;
            if (!fire) continue;
            presses_++;
            if (!countdownActive_ && in.time >= cooldownUntil_) {
                countdownActive_ = true;
                countdownStart_ = in.time;
                lastDigit_ = 0;
            }
        }
    }

    void UpdateCountdown(const SimInput& in) {
        if (!countdownActive_) return;
        double el = in.time - countdownStart_;
        if (el >= kCountdownSeconds) {
            countdownActive_ = false;
            lastDigit_ = 0;
            digitValid_ = false;
            shotEvent_ = true;
            shots_++;
            flashStart_ = in.time;
            cooldownUntil_ = in.time + kShutterCooldown;
            return;
        }
        int d = CountdownDigitAt(el);
        if (d != lastDigit_) { lastDigit_ = d; tickEvent_ = d; }
        // billboard above the camera head, facing the head, +Y = head up; follows the stick every frame
        if (in.headOk) {
            Vec3 up = QRot(in.head.q, V3(0, 1, 0));
            Vec3 centre = PApply(stick_, CamHeadLocal(length_)) + up * kCountdownLift;
            Quat q;
            if (LookAt(in.head.p - centre, up, &q) || LookAt(in.head.p - centre, QRot(in.head.q, V3(0, 0, 1)), &q)) {
                digitPose_ = MakePose(q, centre);
                digitValid_ = true;
            }
        } else if (digitValid_) {   // no head this frame: keep the facing, follow the stick
            Vec3 up = QRot(digitPose_.q, V3(0, 1, 0));
            digitPose_.p = PApply(stick_, CamHeadLocal(length_)) + up * kCountdownLift;
        }
    }
};

} // namespace selfie
