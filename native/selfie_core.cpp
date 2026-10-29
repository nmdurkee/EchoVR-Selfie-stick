// selfie_core.cpp - SelfieStick.dll core: pose sampling, state machine driver,
// seqlock'd RenderState / camera publish, lazy hook install and the DLL exports.
//
// Threads:
//   * SelfieStick_* exports: any thread (Frida RPC, tablet on the game input thread under its
//     lock). Only atomics and a fire-and-forget CreateThread. Never engine calls, never locks.
//   * selfie::Tick: render thread (selfie_render's commit hook), once per frame.
//   * OnVp2Frame: game thread (camera hook). Steps the state machine itself only
//     when Tick has not run for > 50 ms (renderer missing), then writes the camera.
//     It also applies / restores the desktop FOV (capturefov), which the engine reads
//     right after the hooked call in the same function.
// The state machine (StickSim) is guarded by g_simLock. Engine getters are pure
// readers of globals; they are called only from inside Tick / the camera hook.
// Side effects of a shot (capture request, click sound) run AFTER g_simLock is released;
// sounds are played by a tiny worker thread (PlaySound SND_MEMORY|SND_ASYNC), never inline.
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "MinHook.h"
#include "selfie_shared.h"
#include "selfie_math.h"
#include "selfie_sim.h"
#include "selfie_internal.h"

namespace selfie {
namespace internal {
namespace {

// ---------------------------------------------------------------- seqlock box
template <class T>
struct SeqBox {
    std::atomic<uint32_t> seq{0};
    T val{};
    void Write(const T& v) {   // single writer (callers hold g_simLock)
        uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        memcpy(&val, &v, sizeof(T));
        seq.store(s + 2, std::memory_order_release);
    }
    bool Read(T* out) const {
        for (int i = 0; i < 1000; i++) {
            uint32_t a = seq.load(std::memory_order_acquire);
            if (a & 1) { YieldProcessor(); continue; }
            memcpy(out, &val, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t b = seq.load(std::memory_order_relaxed);
            if (a == b) return a != 0;
        }
        return false;
    }
};

struct CamPub {
    int valid;
    Pose cam;   // world
};

// ---------------------------------------------------------------- state
std::atomic<int> g_enabled{0};
std::atomic<uint32_t> g_enableGen{0};
SRWLOCK g_simLock = SRWLOCK_INIT;
StickSim g_sim;                 // guarded by g_simLock
uint32_t g_simGen = 0;          // guarded by g_simLock
uint32_t g_rsCount = 0;         // guarded by g_simLock
int g_lastVisible = -1;         // guarded by g_simLock
SeqBox<RenderState> g_rsBox;
SeqBox<CamPub> g_camBox;

std::atomic<uint64_t> g_lastTickMs{0}, g_ticks{0}, g_steps{0}, g_sampleFails{0}, g_hookSteps{0};
std::atomic<int> g_mode{0}, g_visible{0}, g_highlight{0}, g_twoAxis{0}, g_headOk{0}, g_handsOk{0};
std::atomic<uint32_t> g_gripBits[2] = {{0}, {0}};
// v2
std::atomic<float> g_length{kStickLength};
std::atomic<int> g_hot{0}, g_countdown{0}, g_pointKnown{0};
std::atomic<uint32_t> g_presses{0}, g_shotsFired{0}, g_shotRequestsRefused{0};
std::atomic<bool> g_tipOverride{false};
std::atomic<float> g_tip[3] = {{0.0f}, {-0.02f}, {0.07f}};

// FOV
std::atomic<float> g_fovTarget{NAN};      // NaN = the user has not asked for a value
std::atomic<float> g_fovRead{NAN};        // last plausible value read from the game
std::atomic<bool> g_fovSigOk{false};
std::atomic<uint64_t> g_fovWrites{0}, g_fovFaults{0};
std::atomic<int> g_fovState{0};           // 0 never-read, 1 vp2-off, 2 ok, 3 implausible
SRWLOCK g_fovLock = SRWLOCK_INIT;
FovKeeper g_fovKeeper;                    // guarded by g_fovLock

// install state: touched only by the one-shot install worker (one at a time, g_workerBusy)
std::atomic<bool> g_workerBusy{false}, g_installAgain{false};
uintptr_t g_base = 0;
bool g_pinned = false, g_sigChecked = false, g_mhInit = false, g_renderOk = false, g_captureOk = false;
std::atomic<bool> g_gettersOk{false};
bool g_camSigOk = false;
std::atomic<const char*> g_installErr{""};   // always a string literal: lock-free for Status()

// engine getters
using GetVecFn = float*(__fastcall*)(float* out);
using GetHandFn = float*(__fastcall*)(float* out, int which);
GetVecFn g_headPos = nullptr, g_headRot = nullptr;
GetHandFn g_handPos[2] = {nullptr, nullptr}, g_handRot[2] = {nullptr, nullptr};   // 0 = left, 1 = right

// ---------------------------------------------------------------- signatures
struct Sig { uintptr_t rva; unsigned char bytes[16]; size_t n; };
// echovr.exe sha256 3dae0cda...df9a, read from the on-disk image.
const Sig kGetterSigs[] = {
    {kRvaHeadPos,  {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x42,0xaa,0x99,0x01,0x00}, 14},
    {kRvaHeadRot,  {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x82,0xab,0x99,0x01,0x00}, 14},
    {kRvaRightPos, {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x52,0x98,0x99,0x01,0x00}, 14},
    {kRvaRightRot, {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x02,0x99,0x99,0x01,0x00}, 14},
    {kRvaLeftPos,  {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0x12,0xb0,0x99,0x01,0x00}, 14},
    {kRvaLeftRot,  {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x83,0x3d,0xc2,0xb0,0x99,0x01,0x00}, 14},
    // call FUN_140629c30 at 0x14011fa03 (returns to 0x14011fa08)
    {kRvaVp2Call,  {0xe8,0x28,0xa2,0x50,0x00}, 5},
};
const Sig kCamSig = {kRvaGetCamPose, {0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x57,0x48,0x81,0xec,0xa0,0x00,0x00,0x00}, 15};
// FOV (FUN_14011f9c0): mov rdx,[rcx+0x7ae0] (the -capturevp2 flag word of `game`) and
// movss xmm0,[rsi+0x8e2c] (capturefov; rsi = rcx = game). A mismatch disables only the FOV feature.
const Sig kFovSigs[] = {
    {kRvaVp2FlagRead, {0x48,0x8b,0x91,0xe0,0x7a,0x00,0x00}, 7},
    {kRvaFovRead,     {0xf3,0x0f,0x10,0x86,0x2c,0x8e,0x00,0x00}, 8},
};

bool SigMatches(uintptr_t base, const Sig& s) {
    __try {
        return memcmp(reinterpret_cast<const void*>(base + s.rva), s.bytes, s.n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- time
double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

// ---------------------------------------------------------------- sounds
// Generated once by the sound worker: 16-bit mono PCM WAV images in static memory
// (PlaySound SND_MEMORY|SND_ASYNC needs the buffer alive while it plays).
constexpr int kRate = 22050;
struct Wav { unsigned char bytes[44 + kRate / 10 * 2]; };   // up to 100 ms
Wav g_wavClick, g_wavTick;
std::atomic<HANDLE> g_soundEvent{nullptr};
std::atomic<int> g_soundReq{0};                       // 0 none, 1 tick, 2 click
std::atomic<uint64_t> g_soundsPlayed{0};

void BuildWav(Wav& w, int kind) {
    const int n = kind == 2 ? kRate * 80 / 1000 : kRate * 30 / 1000;
    unsigned char* b = w.bytes;
    auto put32 = [](unsigned char* p, uint32_t v) { memcpy(p, &v, 4); };
    auto put16 = [](unsigned char* p, uint16_t v) { memcpy(p, &v, 2); };
    memcpy(b, "RIFF", 4); put32(b + 4, 36 + n * 2); memcpy(b + 8, "WAVEfmt ", 8);
    put32(b + 16, 16); put16(b + 20, 1); put16(b + 22, 1); put32(b + 24, kRate); put32(b + 28, kRate * 2);
    put16(b + 32, 2); put16(b + 34, 16); memcpy(b + 36, "data", 4); put32(b + 40, n * 2);
    uint32_t rng = 0x1234567u;
    for (int i = 0; i < n; i++) {
        const float t = float(i) / kRate;
        float s;
        if (kind == 2) {   // shutter: two short noise bursts ("ka-chik") over a low thump
            rng = rng * 1664525u + 1013904223u;
            const float noise = float(int(rng >> 9) - (1 << 22)) / float(1 << 22);
            float env = expf(-t * 180.f) + (t > 0.035f ? 0.7f * expf(-(t - 0.035f) * 220.f) : 0.f);
            s = 0.55f * noise * env + 0.35f * sinf(6.2831853f * 140.f * t) * expf(-t * 60.f);
        } else {           // tick: soft 1.6 kHz blip
            s = 0.22f * sinf(6.2831853f * 1600.f * t) * expf(-t * 140.f);
        }
        if (s > 1.f) s = 1.f;
        if (s < -1.f) s = -1.f;
        put16(b + 44 + i * 2, uint16_t(int16_t(s * 32000.f)));
    }
}

DWORD WINAPI SoundWorker(void* ev) {
    BuildWav(g_wavClick, 2);
    BuildWav(g_wavTick, 1);
    for (;;) {
        if (WaitForSingleObject(static_cast<HANDLE>(ev), INFINITE) != WAIT_OBJECT_0) return 0;
        const int id = g_soundReq.exchange(0);
        if (!id) continue;
        const Wav& w = id == 2 ? g_wavClick : g_wavTick;
        PlaySoundW(reinterpret_cast<LPCWSTR>(w.bytes), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        g_soundsPlayed.fetch_add(1, std::memory_order_relaxed);
    }
}

// Any thread, never blocks: records the request (a click wins over a pending tick) and pokes the worker.
void QueueSound(int id) {
    HANDLE ev = g_soundEvent.load(std::memory_order_acquire);
    if (!ev) return;
    int cur = g_soundReq.load();
    while (id >= cur && !g_soundReq.compare_exchange_weak(cur, id)) {}
    SetEvent(ev);
}

// ---------------------------------------------------------------- sampling
// Everything in ENGINE WORLD space: world = rig * engineTracking (FUN_140629c30).
bool SampleWorld(SimInput* in, Pose* rig, TrackFrame* tf) {
    if (!g_gettersOk.load(std::memory_order_acquire)) return false;
    const uintptr_t b = g_base;
    __try {
        uintptr_t renderer = *reinterpret_cast<const uintptr_t*>(b + kRvaRendererPtr);
        if (!renderer) return false;
        uintptr_t cam0 = *reinterpret_cast<const uintptr_t*>(renderer + kRendererCam0);
        if (!cam0) return false;
        uint16_t idx = *reinterpret_cast<const uint16_t*>(cam0 + 0x78);
        uintptr_t holder = *reinterpret_cast<const uintptr_t*>(cam0 + 0x70);
        if (!holder) return false;
        uintptr_t table = *reinterpret_cast<const uintptr_t*>(holder + 0x28);
        if (!table) return false;
        const float* e = reinterpret_cast<const float*>(table + uintptr_t(idx) * 0x30);
        rig->q = Quat{e[0], e[1], e[2], e[3]};
        rig->p = V3(e[4], e[5], e[6]);
        if (!Finite(rig->q) || !Finite(rig->p) || fabsf(QDot(rig->q, rig->q) - 1.0f) > 0.05f) return false;
        rig->q = QNormalize(rig->q);

        const float* rq = reinterpret_cast<const float*>(b + kRvaRecenterQ);
        const float* po = reinterpret_cast<const float*>(b + kRvaPlayOffset);
        tf->recenter = QNormalize(Quat{rq[0], rq[1], rq[2], rq[3]});
        tf->offset = V3(po[0], po[1], po[2]);
        if (!Finite(tf->recenter) || !Finite(tf->offset)) return false;

        const bool live = *reinterpret_cast<const uint64_t*>(b + kRvaTrackLive) != 0;
        const bool headLive = live && *reinterpret_cast<const uint32_t*>(b + kRvaHeadLive) != 0;
        const bool handsLive = live && *reinterpret_cast<const uint32_t*>(b + kRvaHandsLive) != 0;

        alignas(16) float buf[8] = {};
        Pose t;
        g_headPos(buf);  t.p = V3(buf[0], buf[1], buf[2]);
        g_headRot(buf);  t.q = Quat{buf[0], buf[1], buf[2], buf[3]};
        in->headOk = headLive && Finite(t.p) && Finite(t.q);
        in->head = WorldFromTrack(*rig, MakePose(QNormalize(t.q), t.p));

        in->handsOk = handsLive;
        for (int h = 0; h < 2; h++) {
            g_handPos[h](buf, 1);  t.p = V3(buf[0], buf[1], buf[2]);
            g_handRot[h](buf, 1);  t.q = Quat{buf[0], buf[1], buf[2], buf[3]};
            if (!Finite(t.p) || !Finite(t.q)) in->handsOk = false;
            in->hand[h] = WorldFromTrack(*rig, MakePose(QNormalize(t.q), t.p));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- FOV
// Caller holds g_fovLock. Plain data only in this frame (SEH).
void ApplyFovLocked() {
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(g_base + kRvaGamePtr);
        if (!game || (game & 3)) { g_fovState = 1; return; }
        const uint64_t flags = *reinterpret_cast<const uint64_t*>(game + kGameFlags);
        if (!((flags >> 29) & 1)) { g_fovState = 1; return; }   // not running with -capturevp2: leave it alone
        volatile float* f = reinterpret_cast<volatile float*>(game + kGameCaptureFov);
        const float cur = *f;
        if (!PlausibleFov(cur)) { g_fovState = 3; return; }
        g_fovState = 2;
        g_fovRead.store(cur, std::memory_order_relaxed);
        float w;
        if (g_fovKeeper.Update(g_enabled.load(std::memory_order_relaxed) != 0, cur,
                               g_fovTarget.load(std::memory_order_relaxed), &w)) {
            *f = w;
            g_fovWrites.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fovFaults.fetch_add(1, std::memory_order_relaxed);
    }
}

// Game / render threads. Non-blocking: if another thread is applying right now, skip this frame.
void ApplyFov() {
    if (!g_fovSigOk.load(std::memory_order_acquire)) return;
    if (!TryAcquireSRWLockExclusive(&g_fovLock)) return;
    ApplyFovLocked();
    ReleaseSRWLockExclusive(&g_fovLock);
}

// ---------------------------------------------------------------- step + publish
struct StepEvents { bool shot = false; int tick = 0; };

void PublishHidden() {
    if (g_lastVisible == 0) return;
    RenderState rs = {};
    rs.sequence = ++g_rsCount;
    rs.visible = 0;
    rs.stick = PIdentity();
    rs.camHead = PIdentity();
    rs.countdownPose = PIdentity();
    rs.length = g_sim.Length();
    g_rsBox.Write(rs);
    CamPub c = {};
    g_camBox.Write(c);
    g_lastVisible = 0;
    g_visible = 0;
    g_hot = 0;
    g_countdown = 0;
}

// Caller holds g_simLock.
StepEvents StepLocked() {
    StepEvents ev;
    const uint32_t gen = g_enableGen.load(std::memory_order_acquire);
    if (gen != g_simGen) {
        g_simGen = gen;
        g_sim.Enable(g_enabled.load() != 0);   // also cancels a running self-timer (no shot)
    }
    g_sim.SetLength(g_length.load(std::memory_order_relaxed));
    if (g_tipOverride.load(std::memory_order_acquire))
        g_sim.cfg.tipOffset = V3(g_tip[0].load(), g_tip[1].load(), g_tip[2].load());
    if (!g_sim.Enabled()) { PublishHidden(); g_mode = 0; return ev; }

    SimInput in;
    Pose rig;
    TrackFrame tf;
    if (!SampleWorld(&in, &rig, &tf)) { g_sampleFails.fetch_add(1, std::memory_order_relaxed); return ev; }
    float grips[2];
    InputGrips(grips);
    in.grip[0] = grips[0];
    in.grip[1] = grips[1];
    uint32_t touches[2];
    bool known[2];
    InputTouches(touches, known);
    for (int h = 0; h < 2; h++) {
        in.touchesKnown[h] = known[h];
        in.pointing[h] = (touches[h] & kTouchIndexPointing[h]) != 0;
    }
    in.time = NowSeconds();

    g_sim.Step(in);
    g_steps.fetch_add(1, std::memory_order_relaxed);
    ev.shot = g_sim.TakeShot();
    ev.tick = g_sim.TakeDigitTick();

    if (!g_sim.Visible()) { PublishHidden(); }
    else {
        RenderState rs = {};
        rs.sequence = ++g_rsCount;
        rs.visible = 1;
        rs.stick = OvrFromEng(tf, TrackFromWorld(rig, g_sim.Stick()));
        rs.camHead = OvrFromEng(tf, TrackFromWorld(rig, g_sim.CamHead()));
        rs.highlight = g_sim.Highlight() ? 1 : 0;
        rs.length = g_sim.Length();
        rs.shutterHot = g_sim.ShutterHot() ? 1 : 0;
        rs.flash = g_sim.Flash();
        rs.countdownPose = PIdentity();
        if (g_sim.CountdownPoseValid() && g_sim.CountdownDigit() > 0) {
            rs.countdownDigit = g_sim.CountdownDigit();
            rs.countdownAlpha = g_sim.CountdownAlpha();
            rs.countdownPose = OvrFromEng(tf, TrackFromWorld(rig, g_sim.CountdownPose()));
        }
        g_rsBox.Write(rs);
        CamPub c;
        c.valid = g_sim.CameraValid() ? 1 : 0;
        c.cam = g_sim.Camera();
        g_camBox.Write(c);
        g_lastVisible = 1;
        g_visible = 1;
        g_hot = rs.shutterHot;
        g_countdown = rs.countdownDigit;
    }
    g_mode = static_cast<int>(g_sim.Mode());
    g_highlight = g_sim.Highlight() ? 1 : 0;
    g_twoAxis = g_sim.TwoHandAxis() ? 1 : 0;
    g_headOk = in.headOk ? 1 : 0;
    g_handsOk = in.handsOk ? 1 : 0;
    g_pointKnown = (known[0] ? 1 : 0) | (known[1] ? 2 : 0);
    g_presses = g_sim.Presses();
    uint32_t bits[2];
    memcpy(bits, grips, 8);
    g_gripBits[0] = bits[0];
    g_gripBits[1] = bits[1];
    return ev;
}

// Outside g_simLock.
void HandleEvents(const StepEvents& ev) {
    if (ev.shot) {
        g_shotsFired.fetch_add(1, std::memory_order_relaxed);
        if (!selfie_capture::RequestShot()) g_shotRequestsRefused.fetch_add(1, std::memory_order_relaxed);
        QueueSound(2);
    } else if (ev.tick) {
        QueueSound(1);
    }
}

void StepAndHandle() {
    AcquireSRWLockExclusive(&g_simLock);
    StepEvents ev = StepLocked();
    ReleaseSRWLockExclusive(&g_simLock);
    HandleEvents(ev);
}

// ---------------------------------------------------------------- install
void SetErr(const char* s) { g_installErr.store(s, std::memory_order_release); }

// SELFIESTICK_TIP="x,y,z" (controller-local engine metres, +Z forward) overrides the fingertip offset.
void ReadTipOverride() {
    char buf[96];
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof(buf), "SELFIESTICK_TIP") != 0 || n == 0) return;
    float x, y, z;
    if (sscanf_s(buf, "%f,%f,%f", &x, &y, &z) != 3) return;
    if (!Finite(x) || !Finite(y) || !Finite(z) || fabsf(x) > 0.3f || fabsf(y) > 0.3f || fabsf(z) > 0.3f) return;
    g_tip[0] = x; g_tip[1] = y; g_tip[2] = z;
    g_tipOverride.store(true, std::memory_order_release);
}

void EnsureSoundWorker() {
    if (g_soundEvent.load()) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return;
    HANDLE t = CreateThread(nullptr, 0, SoundWorker, ev, 0, nullptr);
    if (!t) { CloseHandle(ev); return; }
    CloseHandle(t);
    g_soundEvent.store(ev, std::memory_order_release);
}

// Runs only on the install worker thread (never under any of our locks: MH_EnableHook
// suspends every other thread while it patches).
void EnsureInstalled() {
    if (!g_pinned) {
        HMODULE self = nullptr;
        g_pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                      reinterpret_cast<LPCWSTR>(&EnsureInstalled), &self) != 0;
    }
    EnsureSoundWorker();
    if (!g_base) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"echovr.exe"));
    if (!g_base) { SetErr("echovr.exe-not-loaded"); return; }
    if (!g_sigChecked) {
        g_sigChecked = true;
        ReadTipOverride();
        bool ok = true;
        for (const Sig& s : kGetterSigs) ok = ok && SigMatches(g_base, s);
        g_camSigOk = SigMatches(g_base, kCamSig);
        bool fovOk = true;
        for (const Sig& s : kFovSigs) fovOk = fovOk && SigMatches(g_base, s);
        if (ok) {
            g_headPos = reinterpret_cast<GetVecFn>(g_base + kRvaHeadPos);
            g_headRot = reinterpret_cast<GetVecFn>(g_base + kRvaHeadRot);
            g_handPos[0] = reinterpret_cast<GetHandFn>(g_base + kRvaLeftPos);
            g_handRot[0] = reinterpret_cast<GetHandFn>(g_base + kRvaLeftRot);
            g_handPos[1] = reinterpret_cast<GetHandFn>(g_base + kRvaRightPos);
            g_handRot[1] = reinterpret_cast<GetHandFn>(g_base + kRvaRightRot);
            g_gettersOk.store(true, std::memory_order_release);
            g_fovSigOk.store(fovOk, std::memory_order_release);
        } else {
            SetErr("unknown-echovr-build(getter-signature-mismatch)");
            return;
        }
        if (!g_camSigOk) SetErr("camera-target-signature-mismatch(already-hooked? close selfie_cam.py)");
        else if (!fovOk) SetErr("fov-signature-mismatch(fov-disabled)");
    }
    if (!g_gettersOk.load()) return;
    if (!g_mhInit) {
        MH_STATUS s = MH_Initialize();
        g_mhInit = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED);
        if (!g_mhInit) { SetErr("MH_Initialize-failed"); return; }
    }
    if (g_camSigOk && !CameraInstalled() && !CameraInstall(g_base)) SetErr("camera-hook-failed");
    if (!InputInstalled() && !InputInstall()) SetErr("input-hook-failed(LibOVRRT64_1.dll?)");
    if (!g_renderOk) g_renderOk = selfie_render::Install();
    if (!g_captureOk) g_captureOk = selfie_capture::Install();
}

DWORD WINAPI InstallWorker(void*) {
    for (;;) {
        while (g_installAgain.exchange(false)) EnsureInstalled();
        g_workerBusy.store(false);
        // a request that arrived after the last exchange but before the store: take it ourselves
        if (!g_installAgain.load()) break;
        bool expected = false;
        if (!g_workerBusy.compare_exchange_strong(expected, true)) break;   // another worker took it
    }
    return 0;
}

// Non-blocking: starts a one-shot worker (never joined) or asks the running one to go again.
void RequestInstall() {
    g_installAgain.store(true);
    bool expected = false;
    if (!g_workerBusy.compare_exchange_strong(expected, true)) return;
    HANDLE t = CreateThread(nullptr, 0, InstallWorker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else { g_workerBusy.store(false); SetErr("install-thread-failed"); }
}

} // namespace

bool Enabled() { return g_enabled.load(std::memory_order_relaxed) != 0; }

// Camera hook (game thread), called for every vp2 frame, enabled or not (the FOV restore needs it).
void OnVp2Frame(void* cam0, float* out) {
    (void)cam0;
    ApplyFov();   // the engine reads capturefov right after this call returns
    if (!Enabled()) return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastTickMs.load(std::memory_order_relaxed) > 50) {   // renderer not ticking: drive from here
        StepAndHandle();
        g_hookSteps.fetch_add(1, std::memory_order_relaxed);
    }
    CamPub c;
    if (!g_camBox.Read(&c) || !c.valid || !Finite(c.cam.q) || !Finite(c.cam.p)) return;
    out[0] = c.cam.q.x; out[1] = c.cam.q.y; out[2] = c.cam.q.z; out[3] = c.cam.q.w;
    out[4] = c.cam.p.x; out[5] = c.cam.p.y; out[6] = c.cam.p.z;
    NoteCameraOverride();
}

} // namespace internal

// ---------------------------------------------------------------- contract API
bool GetRenderState(RenderState* out) {
    if (!out) return false;
    return internal::g_rsBox.Read(out);
}

void Tick() {
    using namespace internal;
    g_lastTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_ticks.fetch_add(1, std::memory_order_relaxed);
    StepAndHandle();
    ApplyFov();
}

} // namespace selfie

// ---------------------------------------------------------------- exports
using namespace selfie;
using namespace selfie::internal;

extern "C" __declspec(dllexport) void SelfieStick_SetEnabled(int on) {
    // Called on the game's input thread while the tablet holds its own lock: only atomics
    // and a fire-and-forget CreateThread here. No locks, no waits, no engine calls.
    if (on) RequestInstall();
    g_enabled.store(on ? 1 : 0, std::memory_order_release);
    g_enableGen.fetch_add(1, std::memory_order_acq_rel);   // next Tick / camera frame (re)spawns or hides
}

extern "C" __declspec(dllexport) int SelfieStick_IsEnabled(void) { return g_enabled.load() ? 1 : 0; }

// v2 exports: atomics only.
extern "C" __declspec(dllexport) float SelfieStick_GetFov(void) {
    const float t = g_fovTarget.load();
    if (Finite(t)) return t;
    const float r = g_fovRead.load();
    return Finite(r) ? r : 112.0f;
}

extern "C" __declspec(dllexport) float SelfieStick_StepFov(int dir) {
    const float v = StepFovValue(SelfieStick_GetFov(), dir);
    g_fovTarget.store(v);
    return v;
}

extern "C" __declspec(dllexport) float SelfieStick_GetLength(void) { return ClampLength(g_length.load()); }

extern "C" __declspec(dllexport) float SelfieStick_StepLength(int dir) {
    float cur = g_length.load();
    float v;
    do { v = StepLengthValue(cur, dir); } while (!g_length.compare_exchange_weak(cur, v));
    return v;
}

extern "C" __declspec(dllexport) int SelfieStick_ShotsTaken(void) { return static_cast<int>(selfie_capture::ShotsTaken()); }

extern "C" __declspec(dllexport) const char* SelfieStick_Status(void) {
    static char bufs[4][1280];
    static std::atomic<unsigned> slot{0};
    char* b = bufs[slot.fetch_add(1) & 3];
    const uint64_t now = GetTickCount64(), lt = g_lastTickMs.load();
    const char* tick = lt == 0 ? "never" : (now - lt <= 50 ? "alive" : "stale");
    float g[2];
    uint32_t bits[2] = {g_gripBits[0].load(), g_gripBits[1].load()};
    memcpy(g, bits, 8);
    const char* render = selfie_render::Status();
    const char* capture = selfie_capture::Status();
    const char* err = g_installErr.load(std::memory_order_acquire);
    if (!err) err = "";
    static const char* const kFovStates[] = {"unread", "vp2-off", "ok", "implausible"};
    const int fs = g_fovState.load();
    _snprintf_s(b, 1280, _TRUNCATE,
                "en=%d mode=%s vis=%d hl=%d two=%d head=%d hands=%d grip L=%.2f R=%.2f | len=%.2f fov=%.0f(game %.0f %s%s w=%llu f=%llu) "
                "hot=%d point=%d presses=%u countdown=%d fired=%u refused=%u shots=%u sounds=%llu | steps=%llu hookSteps=%llu "
                "sampleFails=%llu tick=%s(%llu) | cam=%s vp2=%llu override=%llu faults=%llu | input=%s calls=%llu | "
                "eng=%s | %s | %s%s%s",
                g_enabled.load(), ModeName(static_cast<StickMode>(g_mode.load())), g_visible.load(),
                g_highlight.load(), g_twoAxis.load(), g_headOk.load(), g_handsOk.load(), g[0], g[1],
                SelfieStick_GetLength(), SelfieStick_GetFov(), g_fovRead.load(),
                g_fovSigOk.load() ? kFovStates[fs < 0 || fs > 3 ? 0 : fs] : "sig-mismatch",
                Finite(g_fovTarget.load()) ? " target" : "",
                (unsigned long long)g_fovWrites.load(), (unsigned long long)g_fovFaults.load(),
                g_hot.load(), g_pointKnown.load(), g_presses.load(), g_countdown.load(), g_shotsFired.load(),
                g_shotRequestsRefused.load(), selfie_capture::ShotsTaken(), (unsigned long long)g_soundsPlayed.load(),
                (unsigned long long)g_steps.load(), (unsigned long long)g_hookSteps.load(),
                (unsigned long long)g_sampleFails.load(), tick, (unsigned long long)g_ticks.load(),
                CameraInstalled() ? "hooked" : "off", (unsigned long long)CameraVp2Calls(),
                (unsigned long long)CameraOverrides(), (unsigned long long)CameraFaults(),
                InputInstalled() ? "hooked" : "off", (unsigned long long)InputCalls(),
                g_gettersOk.load() ? "ok" : "unverified", render ? render : "render=?",
                capture ? capture : "capture=?", err[0] ? " | err=" : "", err);
    return b;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);   // nothing heavy here
    return TRUE;
}
