"""
load_selfie.py - load SelfieStick.dll into a running echovr.exe (no tablet needed).

Step 2 of the DESIGN.md test order. Frida LoadLibraryW's native\\dist\\SelfieStick.dll
and calls its exports. That is allowed from Frida's thread because:
  * DllMain does nothing but DisableThreadLibraryCalls;
  * SelfieStick_SetEnabled only flips atomics and installs MinHook hooks (thread-agnostic).
    It never calls engine functions; the stick spawns on the next game/render frame.

The desktop camera needs the game started with -capturevp2 (use --launch).
The DLL pins itself once enabled: rebuild only after closing the game.

Run:
    python .\\SelfieCam\\load_selfie.py            attach to a running echovr.exe
    python .\\SelfieCam\\load_selfie.py --launch   start echovr.exe -capturevp2, then attach

Keys (in this console):
    e  enable  (SelfieStick_SetEnabled(1): (re)spawn the stick 0.5 m in front of you)
    d  disable (SelfieStick_SetEnabled(0): hide it, desktop view back to the stock head cam)
    s  print status now
    q  quit (disables first; the DLL itself stays loaded until the game exits)
"""

import msvcrt
import os
import subprocess
import sys
import time
from datetime import datetime

import frida

HERE = os.path.dirname(os.path.abspath(__file__))
DLL_PATH = os.path.join(HERE, "native", "dist", "SelfieStick.dll")
LOG_PATH = os.path.join(os.path.dirname(HERE), "artifacts", "logs", "log.txt")
GAME_DIR = r"C:\Program Files\Meta Horizon\Software\Software\ready-at-dawn-echo-arena"
GAME_EXE = os.path.join(GAME_DIR, "bin", "win10", "echovr.exe")
STATUS_PERIOD = 3.0

# Frida 17: Process.getModuleByName(...).getExportByName(...), not Module.getExportByName.
JS = r"""
'use strict';
var api = null;

function bind() {
    var m = Process.findModuleByName('SelfieStick.dll');
    if (m === null) return false;
    api = {
        setEnabled: new NativeFunction(m.getExportByName('SelfieStick_SetEnabled'), 'void', ['int']),
        isEnabled:  new NativeFunction(m.getExportByName('SelfieStick_IsEnabled'), 'int', []),
        status:     new NativeFunction(m.getExportByName('SelfieStick_Status'), 'pointer', [])
    };
    return true;
}

rpc.exports = {
    load: function (path) {
        if (bind()) return { ok: true, already: true, base: Process.getModuleByName('SelfieStick.dll').base.toString() };
        var loadLibraryW = new NativeFunction(
            Process.getModuleByName('kernel32.dll').getExportByName('LoadLibraryW'), 'pointer', ['pointer']);
        var getLastError = new NativeFunction(
            Process.getModuleByName('kernel32.dll').getExportByName('GetLastError'), 'uint32', []);
        var h = loadLibraryW(Memory.allocUtf16String(path));
        if (h.isNull()) return { ok: false, error: 'LoadLibraryW failed, GetLastError=' + getLastError() };
        if (!bind()) return { ok: false, error: 'loaded but SelfieStick.dll module not found' };
        return { ok: true, already: false, base: h.toString() };
    },
    probe: function () {
        var base = Process.getModuleByName('echovr.exe').base;
        var game = base.add(0x20a0478).readPointer();
        var flags = game.isNull() ? 0 : game.add(0x7ae0).readU32();
        return { capturevp2: ((flags >>> 29) & 1) === 1, ovr: Process.findModuleByName('LibOVRRT64_1.dll') !== null };
    },
    setEnabled: function (on) { if (!api) return -1; api.setEnabled(on ? 1 : 0); return api.isEnabled(); },
    status: function () { if (!api) return 'not loaded'; var p = api.status(); return p.isNull() ? '(null)' : p.readUtf8String(); }
};
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
        print(f">>> {message['payload']}")
    elif message["type"] == "error":
        print("FRIDA ERROR: " + message.get("stack", ""))


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


def stamp():
    return datetime.now().strftime("%H:%M:%S")


def _main_body():
    if not os.path.isfile(DLL_PATH):
        print(f"ERROR: {DLL_PATH} not found - run SelfieCam\\native\\build.cmd first")
        sys.exit(1)
    if "--launch" in sys.argv:
        launch()
        time.sleep(5)
    session = attach()
    script = session.create_script(JS)
    script.on("message", on_message)
    script.load()
    api = script.exports_sync

    probe = api.probe()
    print(f"[*] probe: {probe}")
    if not probe["capturevp2"]:
        print("[!] echovr.exe was NOT started with -capturevp2: the stick still works in the headset, but the")
        print("    desktop view will not move. Restart with: python .\\SelfieCam\\load_selfie.py --launch")
    if not probe["ovr"]:
        print("[!] LibOVRRT64_1.dll not loaded yet: grip input hook will be retried on the next enable (e).")

    res = api.load(DLL_PATH)
    print(f"[*] load {DLL_PATH}: {res}")
    if not res.get("ok"):
        return
    print(f"[{stamp()}] {api.status()}")
    print("[*] keys: e enable | d disable | s status | q quit")

    alive = [True]
    session.on("detached", lambda reason, crash: alive.__setitem__(0, False))
    last = time.time()
    while alive[0]:
        if msvcrt.kbhit():
            k = msvcrt.getwch().lower()
            if k == "q":
                break
            if k == "e":
                print(f"[{stamp()}] SetEnabled(1) -> IsEnabled={api.set_enabled(True)}")
            elif k == "d":
                print(f"[{stamp()}] SetEnabled(0) -> IsEnabled={api.set_enabled(False)}")
            if k in "eds":
                print(f"[{stamp()}] {api.status()}")
                last = time.time()
        if time.time() - last >= STATUS_PERIOD:
            last = time.time()
            try:
                print(f"[{stamp()}] {api.status()}")
            except Exception as e:   # session gone mid-call
                print(f"[!] status failed: {e}")
                break
        time.sleep(0.03)

    if alive[0]:
        api.set_enabled(False)
        print(f"[{stamp()}] disabled; final: {api.status()}")
        script.unload()
        session.detach()
        print("[*] detached (SelfieStick.dll stays loaded, disabled, until the game exits)")
    else:
        print("[*] game exited")


def main():
    tee = Tee(LOG_PATH)
    sys.stdout = tee
    print()
    print("#" * 70)
    print(f"# RUN START: {datetime.now().isoformat(timespec='seconds')}  SelfieStick loader")
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
