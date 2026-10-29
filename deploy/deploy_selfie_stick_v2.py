"""Upgrade the installed Selfie Stick v1 (trainer 0.6.12) to v2 (trainer 0.6.13 Selfie page +
SelfieStick.dll v2: shutter button, 3-2-1 timer, screenshots, FOV/length), with exact backups.

    python tools\\deploy_selfie_stick_v2.py                    preflight only (merge into a temp dir)
    python tools\\deploy_selfie_stick_v2.py --apply            install (Echo closed)
    python tools\\deploy_selfie_stick_v2.py --restore          verify rollback to v1
    python tools\\deploy_selfie_stick_v2.py --restore --apply  roll back to the exact v1 bytes

Full uninstall afterwards: restore v2 first, then `deploy_selfie_stick.py --restore --apply`.
Changes: replace bin\\win10\\EchoTabletTrainer.dll, bin\\win10\\SelfieStick.dll and
manifests\\48037dc70b0ecab2; add packages\\48037dc70b0ecab2_<n>. Nothing else is touched.
"""
import argparse, json, os, shutil, subprocess, tempfile
from pathlib import Path

from deploy_selfie_stick import (GAME, BIN, MANIFEST, PACKAGES, DIST, SELFIE_SRC, EXE_SHA,
                                 sha, digest, running, atomic)

HERE = Path(__file__).resolve().parents[1]
BACKUP = HERE / "backups" / "backup_selfie_stick_v2"
V1_RECEIPT = HERE / "backups" / "backup_selfie_stick_v1" / "receipt.json"
VERSION = "0.6.13-selfie-page + SelfieStick v2"


def rel(p): return str(Path(p).relative_to(GAME))


def restore(apply):
    receipt = json.loads((BACKUP / "receipt.json").read_text())
    for r, h in receipt["replaced_after"].items():
        assert digest(GAME / r) == h, f"changed since v2 install, refusing: {r}"
    for r, h in receipt["created"].items():
        p = GAME / r
        assert not p.exists() or digest(p) == h, f"created file changed since install, refusing: {r}"
    if not apply:
        print("Exact rollback to v1 verified. Run with --restore --apply (Echo closed)."); return
    if running(): raise RuntimeError("Close Echo VR before restoring")
    for r, h in receipt["replaced_before"].items():
        data = (BACKUP / (r.replace("\\", "__") + ".original")).read_bytes()
        assert sha(data) == h
        atomic(GAME / r, data)
    for r in receipt["created"]:
        if (GAME / r).exists(): (GAME / r).unlink()
    done = BACKUP.with_name(BACKUP.name + "_restored")
    if done.exists(): shutil.rmtree(done)
    BACKUP.rename(done)
    print(f"Restored v1 exactly. Backup kept at {done}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()
    if a.restore: return restore(a.apply)

    # ---------------- preflight: the installed state must be exactly v1
    assert not BACKUP.exists(), f"{BACKUP} exists: v2 already installed? use --restore first"
    v1 = json.loads(V1_RECEIPT.read_text())
    assert digest(BIN / "echovr.exe") == EXE_SHA, "unexpected echovr.exe build"
    for r, h in {**v1["replaced_after"], **v1["created"]}.items():
        assert digest(GAME / r) == h, f"installed v1 file changed, refusing: {r}"

    trainer = (DIST / "EchoTabletTrainer.dll").read_bytes()
    assert trainer[:2] == b"MZ" and b"Native trainer 0.6.13" in trainer, "dist trainer is not 0.6.13"
    selfie = SELFIE_SRC.read_bytes()
    for export in (b"SelfieStick_StepFov", b"SelfieStick_StepLength", b"SelfieStick_ShotsTaken"):
        assert export in selfie, f"SelfieStick.dll is not v2 (missing {export.decode()})"

    staging = Path(tempfile.mkdtemp(prefix="selfie2-merge-"))
    r = subprocess.run([str(DIST / "manifest_merge.exe"), str(MANIFEST), str(DIST / "tablet.patch"), str(staging)],
                       capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr.strip())
    assert r.returncode == 0, "manifest merge refused; no game files changed"
    produced = sorted(p.name for p in staging.iterdir())
    new_pkgs = [n for n in produced if n != "manifest"]
    assert "manifest" in produced and new_pkgs and all(n.startswith("48037dc70b0ecab2_") for n in new_pkgs), produced
    for n in new_pkgs: assert not (PACKAGES / n).exists(), f"package {n} already exists"

    replace = {rel(BIN / "EchoTabletTrainer.dll"): trainer,
               rel(BIN / "SelfieStick.dll"): selfie,
               rel(MANIFEST): (staging / "manifest").read_bytes()}
    create = {rel(PACKAGES / n): (staging / n).read_bytes() for n in new_pkgs}
    print(f"Preflight OK ({VERSION}). Would replace {list(replace)} and add {list(create)}.")
    if not a.apply:
        print("Preflight only; game files unchanged."); return
    if running(): raise RuntimeError("Close Echo VR before installing")

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
               "headset_validation": "pending"}
    (BACKUP / "receipt.before.json").write_text(json.dumps(receipt, indent=2))
    try:
        for r_, d in create.items(): atomic(GAME / r_, d)
        for r_, d in replace.items(): atomic(GAME / r_, d)
        (BACKUP / "receipt.json").write_text(json.dumps(receipt, indent=2))
    except BaseException:
        for r_, d in originals.items(): atomic(GAME / r_, d)
        for r_ in create:
            if (GAME / r_).exists(): (GAME / r_).unlink()
        print("FAILED - rolled back to v1."); raise
    shutil.rmtree(staging, ignore_errors=True)
    print("Installed v2 and read-back verified. Launch with the 'Echo VR Selfie' shortcut.")
    print("Undo: python tools\\deploy_selfie_stick_v2.py --restore --apply")


if __name__ == "__main__":
    main()
