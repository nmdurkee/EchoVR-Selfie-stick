"""
SelfieCam v1 - a selfie-stick camera for the desktop/recording view.

The headset view is NOT touched. Echo VR ships a recording mode, launch flag
`-capturevp2` ("Run the game in recording mode, using a second viewport"),
which renders a second mono viewport to the desktop window. Stock, that
viewport's camera copies the HMD pose every frame. This script re-points it
at your hand, looking back at your face.

How (static analysis, echovr.exe image base 0x140000000; see README.md):
  FUN_14011f9c0 (per frame, only when game+0x7ae0 bit 29 = -capturevp2):
      cam2 = *(renderer + 0x2ce88)                    renderer = *(base+0x20a0470)
      FUN_140629c30(*(renderer + 0x2ce80), &pose)     <- returns to 0x14011fa08
      cam2->vfn[0x68](&pose.pos); cam2->vfn[0x70](&pose.rot)
  FUN_140629c30(cam0, out): out = base(rig) * HMD  -> out[0..3]=quat xyzw, out[4..6]=pos
We hook FUN_140629c30, and ONLY for calls returning to 0x14011fa08 rewrite
`out` with: position = hand (+ optional stick), rotation = look at the head.

Hand pose comes from the engine's own getters (same frame as the HMD getters):
  right: pos FUN_14072df40(out,1)  rot FUN_14072de90(out,1)
  left:  pos FUN_14072c780(out,1)  rot FUN_14072c6d0(out,1)
and is composed with the rig base exactly like FUN_140629c30 does, using the
engine's QuaternionRotateVector (0x1400f93b0) and quat multiply (0x1400f94e0).

Client-side only: it moves a local render camera. Nothing replicates.

Run:
    python .\\SelfieCam\\selfie_cam.py            attach to a running echovr.exe
    python .\\SelfieCam\\selfie_cam.py --launch   start echovr.exe -capturevp2, then attach

Keys (in this console):
    s  selfie on/off (off = stock -capturevp2 head cam)
    h  switch hand (right/left)
    m  mode: look-at-face / rigid (camera bolted to the controller)
    f  flip the camera 180 deg (if it shows the back of your hand)
    [ ]  stick length -/+ 0.1 m
    , .  smoothing -/+
    q  quit (unhooks; the view goes back to the stock head cam)
"""

import msvcrt
import os
import subprocess
import sys
import time
from datetime import datetime

import frida

HERE = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(os.path.dirname(HERE), "artifacts", "logs", "log.txt")
GAME_DIR = r"C:\Program Files\Meta Horizon\Software\Software\ready-at-dawn-echo-arena"
GAME_EXE = os.path.join(GAME_DIR, "bin", "win10", "echovr.exe")

JS = r"""
'use strict';
var base = Process.getModuleByName('echovr.exe').base;
function R(va) { return base.add(va - 0x140000000); }

var GAME_PTR      = R(0x1420a0478);   // CR15Game*
var RENDERER_PTR  = R(0x1420a0470);
var HANDS_OK_A    = R(0x1420c77a0);   // both must be nonzero for live hand poses
var HANDS_OK_B    = R(0x1420223cc);
var VP2_RET       = R(0x14011fa08);   // return address of the -capturevp2 call site

var getPose = R(0x140629c30);
var qrot = new NativeFunction(R(0x1400f93b0), 'pointer', ['pointer', 'pointer', 'pointer']); // out = q*v
var qmul = new NativeFunction(R(0x1400f94e0), 'pointer', ['pointer', 'pointer', 'pointer']); // out = a*b
var handPos = [new NativeFunction(R(0x14072df40), 'pointer', ['pointer', 'int']),   // 0 = right
               new NativeFunction(R(0x14072c780), 'pointer', ['pointer', 'int'])];  // 1 = left
var handRot = [new NativeFunction(R(0x14072de90), 'pointer', ['pointer', 'int']),
               new NativeFunction(R(0x14072c6d0), 'pointer', ['pointer', 'int'])];

// 16-byte aligned scratch slots (the engine's math uses SSE loads)
var scratch = Memory.alloc(0x200);
var S = [];
for (var i = 0; i < 16; i++) S.push(scratch.add(0x20 * i));

var cfg = { enabled: true, hand: 0, mode: 0, flip: false, stick: 0.0, smooth: 0.5 };
var st = { frames: 0, hits: 0, lastP: null, lastQ: null, lastLog: 0, err: null };

function rv3(p) { return [p.readFloat(), p.add(4).readFloat(), p.add(8).readFloat()]; }
function wv3(p, v) { p.writeFloat(v[0]); p.add(4).writeFloat(v[1]); p.add(8).writeFloat(v[2]); p.add(12).writeFloat(0); }
function rq(p) { return [p.readFloat(), p.add(4).readFloat(), p.add(8).readFloat(), p.add(12).readFloat()]; }
function wq(p, q) { p.writeFloat(q[0]); p.add(4).writeFloat(q[1]); p.add(8).writeFloat(q[2]); p.add(12).writeFloat(q[3]); }
function sub(a, b) { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
function add(a, b) { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
function cross(a, b) { return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]; }
function len(a) { return Math.sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
function norm(a) { var l = len(a); return l < 1e-6 ? null : [a[0]/l, a[1]/l, a[2]/l]; }

function eRot(q, v) { wq(S[0], q); wv3(S[1], v); qrot(S[2], S[1], S[0]); return rv3(S[2]); }
function eMul(a, b) { wq(S[0], a); wq(S[1], b); qmul(S[2], S[0], S[1]); return rq(S[2]); }

// Rotation whose local +Z is `fwd` and local +Y is as close to `up` as possible
// (engine camera convention: forward +Z, up +Y, right = -X).
var conjugateConvention = false;
function quatFromBasis(fwd, up) {
    var x = norm(cross(up, fwd)); if (!x) return null;
    var y = cross(fwd, x), z = fwd;
    var m00 = x[0], m11 = y[1], m22 = z[2], t = m00 + m11 + m22, q;
    if (t > 0) {
        var s = Math.sqrt(t + 1) * 2;
        q = [(y[2]-z[1])/s, (z[0]-x[2])/s, (x[1]-y[0])/s, 0.25*s];
    } else if (m00 > m11 && m00 > m22) {
        var s = Math.sqrt(1 + m00 - m11 - m22) * 2;
        q = [0.25*s, (y[0]+x[1])/s, (z[0]+x[2])/s, (y[2]-z[1])/s];
    } else if (m11 > m22) {
        var s = Math.sqrt(1 + m11 - m00 - m22) * 2;
        q = [(y[0]+x[1])/s, 0.25*s, (z[1]+y[2])/s, (z[0]-x[2])/s];
    } else {
        var s = Math.sqrt(1 + m22 - m00 - m11) * 2;
        q = [(z[0]+x[2])/s, (z[1]+y[2])/s, 0.25*s, (x[1]-y[0])/s];
    }
    if (conjugateConvention) { q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; }
    return q;
}

// Self-check: does the engine rotate +Z onto `fwd` with our quaternion?
(function calibrate() {
    var fwd = norm([1, 0.3, 0.2]);
    var r = eRot(quatFromBasis(fwd, [0, 1, 0]), [0, 0, 1]);
    var d = r[0]*fwd[0] + r[1]*fwd[1] + r[2]*fwd[2];
    if (d < 0.99) {
        conjugateConvention = true;
        r = eRot(quatFromBasis(fwd, [0, 1, 0]), [0, 0, 1]);
        d = r[0]*fwd[0] + r[1]*fwd[1] + r[2]*fwd[2];
    }
    send({ type: 'info', msg: 'quat convention: ' + (conjugateConvention ? 'conjugate' : 'standard') +
           ' (check dot=' + d.toFixed(4) + ', want 1.0000)' });
})();

function smoothTo(p, q) {
    var a = 1 - Math.min(Math.max(cfg.smooth, 0), 0.95);   // weight of the new sample
    if (!st.lastP) { st.lastP = p; st.lastQ = q; return; }
    st.lastP = [st.lastP[0] + (p[0]-st.lastP[0])*a, st.lastP[1] + (p[1]-st.lastP[1])*a, st.lastP[2] + (p[2]-st.lastP[2])*a];
    var lq = st.lastQ, dot = lq[0]*q[0] + lq[1]*q[1] + lq[2]*q[2] + lq[3]*q[3], sg = dot < 0 ? -1 : 1;
    var n = [lq[0] + (sg*q[0]-lq[0])*a, lq[1] + (sg*q[1]-lq[1])*a, lq[2] + (sg*q[2]-lq[2])*a, lq[3] + (sg*q[3]-lq[3])*a];
    var l = Math.sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2] + n[3]*n[3]);
    st.lastQ = [n[0]/l, n[1]/l, n[2]/l, n[3]/l];
}

var FLIP_Y = [0, 1, 0, 0];   // 180 deg about local up

Interceptor.attach(getPose, {
    onEnter: function (args) {
        this.vp2 = this.returnAddress.equals(VP2_RET);
        if (this.vp2) { this.cam = args[0]; this.out = args[1]; }
    },
    onLeave: function () {
        if (!this.vp2) return;
        st.frames++;
        if (!cfg.enabled) { st.lastP = null; return; }
        try {
            var cam = this.cam, out = this.out;
            if ((cam.add(0xb0).readU8() & 1) === 0) return;                  // rig not head-tracked
            if (HANDS_OK_A.readU32() === 0 || HANDS_OK_B.readU32() === 0) return;

            // rig base transform, read exactly as FUN_140629c30 does
            var idx = cam.add(0x78).readU16();
            var entry = cam.add(0x70).readPointer().add(0x28).readPointer().add(idx * 0x30);
            var baseQ = rq(entry), baseP = rv3(entry.add(0x10));

            var headP = rv3(out.add(0x10));                                   // engine's own head world pos

            handPos[cfg.hand](S[4], 1); handRot[cfg.hand](S[5], 1);
            var handP = add(baseP, eRot(baseQ, rv3(S[4])));
            var handQ = eMul(baseQ, rq(S[5]));

            var camP = handP;
            if (cfg.stick !== 0) camP = add(handP, eRot(handQ, [0, 0, cfg.stick]));

            var camQ;
            if (cfg.mode === 0) {                                             // look at the face
                var fwd = norm(sub(headP, camP));
                if (!fwd) return;
                camQ = quatFromBasis(fwd, [0, 1, 0]) || quatFromBasis(fwd, eRot(handQ, [0, 1, 0]));
                if (!camQ) return;
            } else {                                                          // rigid on the controller
                camQ = handQ;
            }
            if (cfg.flip) camQ = eMul(camQ, FLIP_Y);

            smoothTo(camP, camQ);
            wq(out, st.lastQ);
            out.add(0x10).writeFloat(st.lastP[0]);
            out.add(0x14).writeFloat(st.lastP[1]);
            out.add(0x18).writeFloat(st.lastP[2]);
            st.hits++;

            var now = Date.now();
            if (now - st.lastLog > 3000) {
                st.lastLog = now;
                send({ type: 'status', msg: 'frames=' + st.frames + ' rewritten=' + st.hits +
                       ' head=' + headP.map(function (v) { return v.toFixed(2); }) +
                       ' cam=' + st.lastP.map(function (v) { return v.toFixed(2); }) +
                       ' dist=' + len(sub(headP, st.lastP)).toFixed(2) + 'm' });
            }
        } catch (e) {
            if (!st.err) { st.err = String(e); send({ type: 'error', msg: 'hook: ' + e.stack }); }
        }
    }
});

rpc.exports = {
    probe: function () {
        var game = GAME_PTR.readPointer(), rend = RENDERER_PTR.readPointer();
        var flags = game.isNull() ? 0 : game.add(0x7ae0).readU32();
        return {
            capturevp2: ((flags >>> 29) & 1) === 1,
            vp2cam: rend.isNull() ? '0x0' : rend.add(0x2ce88).readPointer().toString(),
            handsTracked: HANDS_OK_A.readU32() !== 0 && HANDS_OK_B.readU32() !== 0
        };
    },
    setCfg: function (c) { for (var k in c) cfg[k] = c[k]; st.lastP = null; return cfg; },
    stats: function () { return { frames: st.frames, hits: st.hits, err: st.err }; }
};
send({ type: 'info', msg: 'hook installed on FUN_140629c30 (vp2 call site only)' });
"""


class Tee:
    def __init__(self, filename):
        self.terminal = sys.stdout
        os.makedirs(os.path.dirname(filename), exist_ok=True)
        self.log = open(filename, "a", encoding="utf-8", buffering=1)

    def write(self, message):
        self.terminal.write(message)
        self.log.write(message)

    def flush(self):
        self.terminal.flush()
        self.log.flush()

    def close(self):
        self.log.close()


def on_message(message, data):
    if message["type"] == "send":
        p = message["payload"]
        if isinstance(p, dict):
            print(f">>> [{p.get('type', '')}] {p.get('msg', '')}")
    elif message["type"] == "error":
        print("FRIDA ERROR: " + message.get("stack", ""))


def describe(cfg):
    return (f"selfie={'ON' if cfg['enabled'] else 'off'}  hand={'right' if cfg['hand'] == 0 else 'left'}  "
            f"mode={'look-at-face' if cfg['mode'] == 0 else 'rigid'}  flip={cfg['flip']}  "
            f"stick={cfg['stick']:.1f}m  smooth={cfg['smooth']:.2f}")


def launch():
    if any(p.name.lower() == "echovr.exe" for p in frida.get_local_device().enumerate_processes()):
        print("echovr.exe is already running - close it first, or run without --launch.")
        sys.exit(1)
    print(f"Launching: {GAME_EXE} -capturevp2")
    subprocess.Popen([GAME_EXE, "-capturevp2"], cwd=os.path.dirname(GAME_EXE))


def attach():
    for _ in range(120):
        try:
            return frida.attach("echovr.exe")
        except frida.ProcessNotFoundError:
            time.sleep(1)
    print("ERROR: echovr.exe not found")
    sys.exit(1)


def _main_body():
    if "--launch" in sys.argv:
        launch()
    session = attach()
    script = session.create_script(JS)
    script.on("message", on_message)
    script.load()
    api = script.exports_sync

    # the renderer and the flag word come up a few seconds after process start
    for _ in range(60):
        probe = api.probe()
        if probe["vp2cam"] != "0x0" or probe["capturevp2"]:
            break
        time.sleep(1)
    print(f"[*] probe: {probe}")
    if not probe["capturevp2"]:
        print("[!] This echovr.exe was NOT started with -capturevp2, so the second viewport")
        print("    does not exist and nothing will change. Close the game and run:")
        print("        python .\\SelfieCam\\selfie_cam.py --launch")
    if not probe["handsTracked"]:
        print("[!] Hands are not tracked yet (headset asleep or not in VR); the hook waits for them.")

    cfg = api.set_cfg({})
    print(f"[*] {describe(cfg)}")
    print("[*] keys: s toggle | h hand | m mode | f flip | [ ] stick | , . smooth | q quit")
    session_alive = [True]
    session.on("detached", lambda reason, crash: session_alive.__setitem__(0, False))

    while session_alive[0]:
        if msvcrt.kbhit():
            k = msvcrt.getwch().lower()
            change = None
            if k == "q":
                break
            elif k == "s":
                change = {"enabled": not cfg["enabled"]}
            elif k == "h":
                change = {"hand": 1 - cfg["hand"]}
            elif k == "m":
                change = {"mode": 1 - cfg["mode"]}
            elif k == "f":
                change = {"flip": not cfg["flip"]}
            elif k == "[":
                change = {"stick": round(max(-1.0, cfg["stick"] - 0.1), 2)}
            elif k == "]":
                change = {"stick": round(min(1.5, cfg["stick"] + 0.1), 2)}
            elif k == ",":
                change = {"smooth": round(max(0.0, cfg["smooth"] - 0.1), 2)}
            elif k == ".":
                change = {"smooth": round(min(0.9, cfg["smooth"] + 0.1), 2)}
            if change:
                cfg = api.set_cfg(change)
                print(f"[*] {describe(cfg)}")
        time.sleep(0.03)

    if session_alive[0]:
        print(f"[*] stats: {api.stats()}")
        script.unload()
        session.detach()
        print("[*] unhooked - desktop view is back to the stock -capturevp2 head cam")
    else:
        print("[*] game exited")


def main():
    tee = Tee(LOG_PATH)
    sys.stdout = tee
    print()
    print("#" * 70)
    print(f"# RUN START: {datetime.now().isoformat(timespec='seconds')}  SelfieCam v1")
    print("#" * 70)
    try:
        _main_body()
    finally:
        print(f"# RUN END: {datetime.now().isoformat(timespec='seconds')}")
        print("#" * 70)
        sys.stdout = tee.terminal
        tee.close()


if __name__ == "__main__":
    main()
