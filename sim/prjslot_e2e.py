#!/usr/bin/env python3
"""Projects keyed by the Rytm's slot, end to end in rytm-emu: the emulated Rytm (rytm-mods rui1052: 0026's 4B project
reports) over the Overbridge link with the real engine core as the host (sim/poly_live.py), headless (dtboot); its card
a folder kept across the runs (a Sidecar rebooted between them). docs/superpowers/specs/2026-10-27-project-slot-keys-design.md.

    make -C core lib && uv run python sim/prjslot_e2e.py --emu ~/PycharmProjects/worktrees/rytm-emu-seqp2 [--out DIR]

The emulator's base: rytm-mods tools/emu/us_base.sh rui1052 (its card: PRESETS in slots 1 and 2, slot 1 active and
write protected). The project manager driven with the research's panel sequences (its proj_ops.sh: first key at the
snapshot + 100 M, keys 50 M apart, FUNC + key with FUNC held 80 M; 200 M instructions = 1 emulated second). Each run
starts from the previous run's snapshot and card (chained), with a fresh host process on the same Sidecar card folder:
  hello     the Sidecar card as name mode left it (PRESETS's files under its name, /.rytm/project PRESETS): the first
            ACTIVE (1, PRESETS) migrates them to @001 without a question
  saveas    SAVE PROJECT AS slot 4: @004 a copy of @001, the Sidecar on @004
  paste     MANAGE: slot 1 COPY, slot 3 PASTE: @003 a copy of @001 (the Sidecar stays on @004)
  clear     MANAGE > slot 3 > CLEAR: @003's files in the trash (why clear), @003 empty
  load2     LOAD slot 2 (the second PRESETS): @002 has no files - the two PRESETS never merged
  pastereload  (rui1052b) slot 1 pasted onto the active slot 2, slot 2 loaded again: its LOAD - @002 a copy of @001 at
            once (not only when the slot is left)
  delete    MANAGE > slot 4 > DELETE (not the active one now): @004's files in the trash (why delete)
  kitsave   (rui1052d) YES + KIT on @002: kit 1's record /.rytm/kits/@002/k001.sks holds its LFOs as they are
  kitreload its LFO file changed (an edit), NO + KIT: the saved LFOs back, the edit kept in k001.prv
  pagereload  (rui1052d) the Sidecar's page open (TRK + YES), NO held + PLAYMODE: the Rytm's kit reload under the page,
            the Sidecar's LFOs back as saved, the edit kept in k001.prv
Exit non-zero when a check fails; <run>.log (the host's log and the emulator's tail) in --out.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = os.environ.get("PJ_TAG", "rui1052")
FAILS = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""), flush=True)
    if not ok:
        FAILS.append(name)


def dn(n):
    return [f"DOWN*{n - 1}"] if n > 1 else []


HOME = ["NO*6", "GLOBAL", "UP*9", "YES"]
MANAGE = HOME + ["DOWN*2", "YES", "F+UP*16"]
RTOP = ["RIGHT", "UP*7"]
SEQS = {
    "hello": ["w:600"],
    "saveas": HOME + ["UP*2", "DOWN", "YES", "F+UP*16"] + dn(4) + ["YES", "w:100", "YES", "w:800"],
    "paste": MANAGE + dn(1) + ["F+RECORD", "F+UP*16"] + dn(3) + ["F+STOP", "w:100", "YES", "w:800"],
    "clear": MANAGE + dn(3) + RTOP + ["YES", "w:100", "YES", "w:800"],
    "load2": HOME + ["UP*2", "YES", "F+UP*16"] + dn(2) + ["YES", "w:100", "NO", "w:100", "YES", "w:1400"],
    "delete": MANAGE + dn(4) + RTOP + ["DOWN", "YES", "w:100", "YES", "w:800"],
    # rui1052b: slot 1 pasted onto the active slot 2, then slot 2 loaded again (its LOAD: the paste followed at once)
    "pastereload": MANAGE + dn(1) + ["F+RECORD", "F+UP*16"] + dn(2) + ["F+STOP", "w:100", "YES", "w:600"] +
                   HOME + ["UP*2", "YES", "F+UP*16"] + dn(2) + ["YES", "w:100", "YES", "w:1400"],
    # rui1052d (4C): YES + KIT saves kit 1, NO + KIT reloads it (core/bundle/kits.h)
    "kitsave": ["NO*4", "w:200", "YES+PLAYMODE", "w:800"],
    "kitreload": ["NO*4", "w:200", "NO+PLAYMODE", "w:800"],
    # rui1052d: the Sidecar's page open (TRK + YES, the real host's mask: NO caught), NO held + PLAYMODE: stock's kit
    # reload under the page, which the Sidecar follows (4C RELOAD)
    "pagereload": ["NO*4", "w:200", "TRK+YES", "w:600", "NO+PLAYMODE", "w:800"],
}


def presses(tokens, t):
    p = []
    for tok in tokens:
        if tok.startswith("w:"):
            t += int(tok[2:])
            continue
        n = 1
        if "*" in tok:
            tok, n = tok.split("*")
            n = int(n)
        for _ in range(n):
            if "+" in tok:   # HOLD+KEY: HOLD held 80 M, KEY 20 M in (F = FUNC)
                hold, key = tok.split("+", 1)
                p += ["--press", f"{'FUNC' if hold == 'F' else hold}@{t}M:80M", "--press", f"{key}@{t + 20}M"]
                t += 130
            else:
                p += ["--press", f"{tok}@{t}M"]
                t += 50
    return p, t


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def put(card, path, text):
    p = os.path.join(card, path.lstrip("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(text)


def get(card, path):
    p = os.path.join(card, path.lstrip("/"))
    return open(p).read() if os.path.isfile(p) else None


FILES = ["/.rytm/lfo/{k}.txt", "/.rytm/seq/{k}/tracks.txt", "/.rytm/seq/{k}/A01.sqp", "/.rytm/songs/{k}/take-001.json",
         "/.rytm/projects/{k}/samples.pool.txt"]


def fixture(card, key, c):
    put(card, f"/.rytm/lfo/{key}.txt", "# Sidecar LFOs\n")
    put(card, f"/.rytm/seq/{key}/tracks.txt", f"# tracks {c}\n")
    put(card, f"/.rytm/seq/{key}/A01.sqp", f"pattern {c}\n" * 2000)
    put(card, f"/.rytm/songs/{key}/take-001.json", '{"song":"%s"}' % c)
    put(card, f"/.rytm/projects/{key}/samples.pool.txt", f"# pool {c}\n")


def same(card, a, b):
    """every fixture file of key a has the same bytes under key b"""
    return all(get(card, f.format(k=a)) is not None and get(card, f.format(k=a)) == get(card, f.format(k=b)) for f in FILES[2:])


def none(card, k):
    return all(get(card, f.format(k=k)) is None for f in FILES)


def trash(card):
    """[(id, {key, why, ...})] of /.rytm/trash"""
    d = os.path.join(card, ".rytm/trash")
    out = []
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if n.isdigit():
            info = dict(l.split("=", 1) for l in (get(card, f"/.rytm/trash/{n}/info.txt") or "").splitlines() if "=" in l)
            out.append((int(n), info))
    return out


def project(card):
    return get(card, "/.rytm/project")


def run(name, a, state, card):
    """One run from state (snap, img, clock); returns the next state."""
    snap, img, clock = state
    port = free_port()
    log = open(os.path.join(a.out, f"{name}.log"), "w")
    host = subprocess.Popen([sys.executable, os.path.join(HERE, "poly_live.py"), "--link", f"127.0.0.1:{port}", "--card",
                             card], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.5)   # the core's boot
    p, t = presses(SEQS[name], clock + 100)
    end = t + 400
    nimg = os.path.join(a.tmp, f"{name}.img")
    subprocess.run(["cp", "-c", img, nimg], check=True)
    nsnap = os.path.join(a.tmp, f"{name}.snap")
    cmd = [os.path.join(a.emu, "target/release/dtboot"), f"fw/AR173_us_{TAG}.syx", "--load", snap, "--card", nimg,
           "--card-save", "--instr", f"{end - clock}M", "--every", "100000M", "--usb-host", "--usb-midi-real",
           "--ob-link", f"127.0.0.1:{port}", "--save", nsnap] + p
    r = subprocess.run(cmd, cwd=a.emu, capture_output=True, text=True, timeout=1800)
    time.sleep(1.5)
    host.terminate()
    try:
        host.wait(10)
    except subprocess.TimeoutExpired:
        host.kill()
    log.write("\n---- dtboot\n" + r.stdout[-4000:] + r.stderr[-2000:])
    log.close()
    print(f"{name}: dtboot rc {r.returncode}", flush=True)
    check(f"{name}: dtboot ran clean", r.returncode == 0 and "FAULT" not in r.stdout + r.stderr)
    return nsnap, nimg, end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", default=os.path.expanduser("~/PycharmProjects/worktrees/rytm-emu-seqp2"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "prjslot_e2e"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    a.tmp = tempfile.mkdtemp(prefix="prjslot_e2e_")
    card = os.path.join(a.tmp, "sidecar")
    os.makedirs(os.path.join(card, "Samples"))
    clock = json.load(open(os.path.join(a.emu, f"sections/us_{TAG}.json")))["clock"]
    state = (os.path.join(a.emu, f"sections/us_{TAG}.snap"), os.path.join(a.emu, f"sections/us_{TAG}.img"), clock)

    # the Sidecar's card as name mode left it: PRESETS's files under its name, the Sidecar on it
    fixture(card, "PRESETS", "presets")
    put(card, "/.rytm/project", "PRESETS")

    state = run("hello", a, state, card)
    check("hello: migrated by slot + name (PRESETS -> @001)", get(card, "/.rytm/seq/@001/tracks.txt") == "# tracks presets\n"
          and none(card, "PRESETS"), project(card))
    check("hello: the Sidecar on @001, the card keyed by slot", project(card) == "@001" and
          "mode=slot" in (get(card, "/.rytm/prjslot.txt") or ""), project(card))

    state = run("saveas", a, state, card)
    check("saveas: @004 a copy of @001 (both kept)", same(card, "@001", "@004"), project(card))
    check("saveas: the Sidecar on @004", project(card) == "@004", project(card))

    state = run("paste", a, state, card)
    check("paste: @003 a copy of @001", same(card, "@001", "@003"))
    check("paste: the Sidecar stays on @004", project(card) == "@004", project(card))

    state = run("clear", a, state, card)
    t = trash(card)
    check("clear: @003's files in the trash (why clear), @003 empty",
          any(i.get("key") == "@003" and i.get("why") == "clear" for _, i in t) and none(card, "@003"), t)

    state = run("load2", a, state, card)
    check("load2: the Sidecar on @002", project(card) == "@002", project(card))
    check("load2: the second PRESETS has no files (never merged with @001)", none(card, "@002") and same(card, "@001", "@004"))

    state = run("pastereload", a, state, card)
    check("pastereload: @002 a copy of @001 (the LOAD of the active slot)", same(card, "@001", "@002"), project(card))
    check("pastereload: the Sidecar on @002", project(card) == "@002", project(card))

    state = run("delete", a, state, card)
    t = trash(card)
    check("delete: @004's files in the trash (why delete), @004 empty",
          any(i.get("key") == "@004" and i.get("why") == "delete" for _, i in t) and none(card, "@004"), t)
    check("delete: @001's files untouched", get(card, "/.rytm/seq/@001/tracks.txt") == "# tracks presets\n")

    lfo = "# Sidecar LFOs\n1 V3 2 1 64 0 0 %d 0\n"
    put(card, "/.rytm/lfo/@002/k001.txt", lfo % 25)
    state = run("kitsave", a, state, card)
    sks = get(card, "/.rytm/kits/@002/k001.sks") or ""
    check("kitsave: kit 1's record on @002 holds its LFOs (depth 25)", "=lfo\n" in sks and " 25 " in sks, sks[:200])
    put(card, "/.rytm/lfo/@002/k001.txt", lfo % 33)   # (an edit, the Sidecar off)
    state = run("kitreload", a, state, card)
    w, prv = get(card, "/.rytm/lfo/@002/k001.txt") or "", get(card, "/.rytm/kits/@002/k001.prv") or ""
    check("kitreload: the saved LFOs back (depth 25), the edit kept in k001.prv (depth 33)", " 25 " in w and " 33 " not in w
          and " 33 " in prv, (w[:120], prv[:200]))
    put(card, "/.rytm/lfo/@002/k001.txt", lfo % 44)   # (another edit, the Sidecar off)
    state = run("pagereload", a, state, card)
    w, prv = get(card, "/.rytm/lfo/@002/k001.txt") or "", get(card, "/.rytm/kits/@002/k001.prv") or ""
    hl = open(os.path.join(a.out, "pagereload.log")).read()
    check("pagereload: the page opened (the host's 30 / mask), NO + PLAYMODE reloaded the kit under it: LFOs back (25), "
          "the edit kept (44)", " 25 " in w and " 44 " not in w and " 44 " in prv, (w[:120], prv[:200]))
    check("pagereload: the page was open (the Rytm's 2B: page-open) and the Sidecar followed the reload (kits)",
          "page-open" in hl and "kits: kit 001 reloaded" in hl, hl[-300:])

    shutil.copytree(os.path.join(card, ".rytm"), os.path.join(a.out, "card-rytm"), dirs_exist_ok=True)
    shutil.rmtree(a.tmp, ignore_errors=True)
    print(f"\n{'PASS' if not FAILS else 'FAIL'}: {len(FAILS)} failed" + (": " + ", ".join(FAILS) if FAILS else ""))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
