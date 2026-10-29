"""Install/restore the Selfie Stick (tablet trainer 0.6.12 + SelfieStick.dll) with exact backups.

    python tools\\deploy_selfie_stick.py                 preflight only (merges into a staging dir)
    python tools\\deploy_selfie_stick.py --apply         install
    python tools\\deploy_selfie_stick.py --restore       verify the rollback
    python tools\\deploy_selfie_stick.py --restore --apply   roll back to the exact pre-install bytes

Changes (nothing else is touched; the script wrappers stay as installed because they only
import BindTrainer, which 0.6.12 still exports):
  replace  bin\\win10\\EchoTabletTrainer.dll                    0.6.11 -> 0.6.12 (Tools: SELFIE STICK switch)
  replace  _data\\...\\manifests\\48037dc70b0ecab2              tablet.patch merged into the LIVE manifest
  add      _data\\...\\packages\\48037dc70b0ecab2_<n>           new frames from that merge
  add      bin\\win10\\SelfieStick.dll
  add      Desktop\\Echo VR Selfie.lnk                          echovr.exe -capturevp2 (PC camera on the stick)
EchoTabletTrainer.install.json is deliberately left alone: its manifest hash was already stale
(Cloud engraving deploy), so do NOT use Install.ps1 -Action Restore; use this script's --restore.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
GAME = Path(r"C:\Program Files\Meta Horizon\Software\Software\ready-at-dawn-echo-arena")
BIN = GAME / "bin" / "win10"
WIN10 = GAME / "_data" / "5932408047" / "rad15" / "win10"
MANIFEST = WIN10 / "manifests" / "48037dc70b0ecab2"
PACKAGES = WIN10 / "packages"
DIST = HERE / "PersonalDiscTrainer" / "native" / "dist"
SELFIE_SRC = HERE / "SelfieCam" / "native" / "dist" / "SelfieStick.dll"
BACKUP = HERE / "backups" / "backup_selfie_stick_v1"
SHORTCUT = Path(os.path.expandvars(r"%USERPROFILE%\OneDrive\Desktop\Echo VR Selfie.lnk"))

EXE_SHA = "3dae0cdab2eb298f9b04fc6baac83f8dd304a8f1d9fea057ab30438fe271df9a"
TRAINER_0611_SHA = "184154890f51867a1a9fe32cc78783cebba5c871cd1a0cf7870f39b12a951bfb"
VERSION = "0.6.12-selfie-stick"


def sha(b): return hashlib.sha256(b).hexdigest()
def digest(p): return sha(Path(p).read_bytes())


def running():
    out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq echovr.exe", "/FO", "CSV", "/NH"], text=True)
    return '"echovr.exe"' in out.lower()


def atomic(path, data):
    path = Path(path)
    tmp = path.with_name(path.name + ".selfie-tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)
    assert digest(path) == sha(data), f"read-back mismatch: {path}"


def make_shortcut():
    ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%s');"
          "$s.TargetPath='%s';$s.Arguments='-capturevp2';$s.WorkingDirectory='%s';"
          "$s.Description='Echo VR with the -capturevp2 second viewport (Selfie Stick PC camera)';$s.Save()"
          % (SHORTCUT, BIN / "echovr.exe", BIN))
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True, capture_output=True)
    assert SHORTCUT.exists(), "shortcut not created"


def restore(apply):
    receipt = json.loads((BACKUP / "receipt.json").read_text())
    for rel, h in receipt["replaced_after"].items():
        assert digest(GAME / rel) == h, f"changed since install, refusing: {rel}"
    for rel, h in receipt["created"].items():
        p = GAME / rel
        assert not p.exists() or digest(p) == h, f"created file changed since install, refusing: {rel}"
    if not apply:
        print("Exact rollback verified. Run with --restore --apply (Echo closed) to restore."); return
    if running(): raise RuntimeError("Close Echo VR before restoring")
    for rel, h in receipt["replaced_before"].items():
        data = (BACKUP / (rel.replace("\\", "__") + ".original")).read_bytes()
        assert sha(data) == h
        atomic(GAME / rel, data)
    for rel in receipt["created"]:
        p = GAME / rel
        if p.exists(): p.unlink()
    if SHORTCUT.exists(): SHORTCUT.unlink()
    done = BACKUP.with_name(BACKUP.name + "_restored")
    if done.exists(): shutil.rmtree(done)
    BACKUP.rename(done)
    print("Restored the exact pre-install files; SelfieStick.dll, the new package and the shortcut removed.")
    print(f"Backup kept at {done}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()
    if a.restore: return restore(a.apply)

    # ---------------- preflight
    assert not BACKUP.exists(), f"{BACKUP} exists: already installed? use --restore first"
    assert digest(BIN / "echovr.exe") == EXE_SHA, "unexpected echovr.exe build"
    assert digest(BIN / "EchoTabletTrainer.dll") == TRAINER_0611_SHA, "installed trainer is not the known 0.6.11"
    trainer = (DIST / "EchoTabletTrainer.dll").read_bytes()
    assert trainer[:2] == b"MZ" and b"Native trainer 0.6.12" in trainer, "dist trainer is not 0.6.12"
    selfie = SELFIE_SRC.read_bytes()
    assert selfie[:2] == b"MZ" and b"SelfieStick_SetEnabled" in selfie, "SelfieStick.dll missing/invalid"
    assert not (BIN / "SelfieStick.dll").exists(), "bin\\win10\\SelfieStick.dll already present"

    staging = Path(tempfile.mkdtemp(prefix="selfie-merge-"))
    r = subprocess.run([str(DIST / "manifest_merge.exe"), str(MANIFEST), str(DIST / "tablet.patch"), str(staging)],
                       capture_output=True, text=True)
    print(r.stdout.strip()); print(r.stderr.strip())
    assert r.returncode == 0, "manifest merge refused; no game files changed"
    produced = sorted(p.name for p in staging.iterdir())
    assert "manifest" in produced, f"merge produced no manifest: {produced}"
    new_pkgs = [n for n in produced if n != "manifest"]
    assert new_pkgs and all(n.startswith("48037dc70b0ecab2_") for n in new_pkgs), f"unexpected merge output {produced}"
    for n in new_pkgs: assert not (PACKAGES / n).exists(), f"package {n} already exists"
    new_manifest = (staging / "manifest").read_bytes()

    rel = lambda p: str(Path(p).relative_to(GAME))
    replace = {rel(BIN / "EchoTabletTrainer.dll"): trainer, rel(MANIFEST): new_manifest}
    create = {rel(BIN / "SelfieStick.dll"): selfie}
    for n in new_pkgs: create[rel(PACKAGES / n)] = (staging / n).read_bytes()
    print(f"Preflight OK ({VERSION}). Would replace {list(replace)} and add {list(create)} + desktop shortcut.")
    if not a.apply:
        print("Preflight only; game files unchanged."); return
    if running(): raise RuntimeError("Close Echo VR before installing")

    # ---------------- backup, then install with rollback on any failure
    BACKUP.mkdir(parents=True)
    originals = {}
    for r_ in replace:
        d = (GAME / r_).read_bytes(); originals[r_] = d
        saved = BACKUP / (r_.replace("\\", "__") + ".original")
        saved.write_bytes(d); assert saved.read_bytes() == d
    receipt = {"version": VERSION,
               "replaced_before": {r_: sha(d) for r_, d in originals.items()},
               "replaced_after": {r_: sha(d) for r_, d in replace.items()},
               "created": {r_: sha(d) for r_, d in create.items()},
               "shortcut": str(SHORTCUT), "headset_validation": "pending"}
    (BACKUP / "receipt.before.json").write_text(json.dumps(receipt, indent=2))
    try:
        for r_, d in create.items(): atomic(GAME / r_, d)
        for r_, d in replace.items(): atomic(GAME / r_, d)
        make_shortcut()
        (BACKUP / "receipt.json").write_text(json.dumps(receipt, indent=2))
    except BaseException:
        for r_, d in originals.items(): atomic(GAME / r_, d)
        for r_ in create:
            if (GAME / r_).exists(): (GAME / r_).unlink()
        if SHORTCUT.exists(): SHORTCUT.unlink()
        print("FAILED - rolled back to the original files."); raise
    shutil.rmtree(staging, ignore_errors=True)
    print("Installed and read-back verified.")
    print(f"Launch with the desktop shortcut '{SHORTCUT.name}' (adds -capturevp2), then Tools tab -> SELFIE STICK.")
    print("Undo: python tools\\deploy_selfie_stick.py --restore --apply")


if __name__ == "__main__":
    main()
