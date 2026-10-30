#!/usr/bin/env python3
"""SONG RECORD end to end in rytm-emu (docs/SONG.md "Built: MVP"): the emulated Rytm (rytm-mods rui1036, OS 1.73) over the
Overbridge link with the real engine core as the host (sim/poly_live.py), headless (dtboot).

    make -C core lib && uv run python sim/song_e2e.py --emu ~/PycharmProjects/worktrees/rytm-emu-seqp2 [--out DIR] [--keep]

1. config at the start of each run the Rytm's MIDI CONFIG walked on its own screen (GLOBAL > MIDI CONFIG: PORT CONFIG
           ENCODER DEST INT+EXT, OUTPUT CH TRK CH, PARAM OUTPUT NRPN; SYNC PROG CH SEND / RECEIVE and TRANSPORT SEND on):
           the emulator's settings, never a real Rytm's.
2. record  then: the host checks the config (song=check: none of its problems), arms (song=arm: the start snapshot by
           the stock dumps), then the performance on the Rytm's panel: PLAY, BD's A knob turned (NRPN), SD muted (MUTE
           mode), A02 queued (BANK A + T2), SD's knob, SD unmuted, STOP. The take is folded into
           /.rytm/songs/<project>/take-001.json; checks: its events (NRPN on BD's channel, the mutes, the queued PC and the
           switch on a bar line, PLAY / STOP), its start snapshot, the journal gone.
3. replay  a fresh emulator from the same base snapshot (the same walk), the take loaded, song=replay + song=confirm (the backup, the restore,
           the verify), then PLAY on the Rytm: the host drives it. Checks: the restore verified, the backup on the card,
           every replayed message within a step of the tick it was recorded at (song=log:on), the Rytm switching to
           A02 on the recorded bar, the mutes, and the end state (song=state: the working kit's fingerprint) equal to
           the record run's.
Card images and snapshots are APFS clones (cp -c) in a scratch folder, removed at the end (--keep keeps them).
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = os.environ.get("SONG_TAG", "rui1036")
BASE_CLOCK = 7400
STEP_M = 200 * 60 / 127.0 / 4      # M instructions a 1x step at 127 BPM (200 M = 1 s)
FAILS = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail != "" else ""), flush=True)
    if not ok:
        FAILS.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# the MIDI CONFIG walk on the rui1036 base (its GLOBAL list opens on SYSTEM: every list topped with UP first, so the
# walk does not depend on the cursors the menus remember) [each screen checked in rytm-emu]
U = ["UP"] * 10
WALK = (["GLOBAL"] + U + ["DOWN"] * 3 + ["YES"] +                      # MIDI CONFIG (PROJECT SAMPLES GLOBAL SLOT, then it)
        U + ["DOWN", "YES"] + ["UP"] * 16 +                             # PORT CONFIG (SYNC, then it), at TURBO SPEED
        ["DOWN"] * 5 + ["RIGHT"] +                                      # OUTPUT CH: AUTO -> TRK CH
        ["DOWN", "LEFT"] +                                              # PARAM OUTPUT: CC -> NRPN
        ["DOWN", "RIGHT"] +                                             # ENCODER DEST: INT -> INT+EXT
        ["NO"] + U + ["YES"] + U +                                      # SYNC, at CLOCK RECEIVE
        ["DOWN"] * 4 + ["YES"] + ["DOWN", "YES"] +                      # PROG CH RECEIVE on, PROG CH SEND on (TRANSPORT SEND is)
        ["NO", "NO", "NO"])


def dtboot(emu, snap, card, length, events, port=None, save=None, png=None, extra=(), clock=BASE_CLOCK):
    cmd = [os.path.join(emu, "target/release/dtboot"), f"fw/AR173_us_{TAG}.syx", "--load", snap, "--card", card,
           "--instr", f"{length:g}M", "--every", "100000M", "--usb-host", "--usb-midi-real"]
    if port:
        cmd += ["--ob-link", f"127.0.0.1:{port}"]
    if save:
        cmd += ["--save", save]
    for e in events:
        t = clock + e[0]
        if e[1] == "nudge":   # (t, "nudge", "ENC:DIR", count, spacing)
            cmd += ["--nudge", f"{e[2]}@{t:g}Mx{e[3]}/{e[4]:g}M"]
        else:
            cmd += ["--press", f"{e[1]}@{t:g}M" + (f":{e[2]:g}M" if len(e) > 2 else "")]
    cmd += list(extra)
    env = dict(os.environ)
    if png:
        env["DTBOOT_END_PNG"] = png
    return subprocess.run(cmd, cwd=emu, env=env, capture_output=True, text=True, timeout=3600)


def host(port, card, logf, at):
    args = [sys.executable, os.path.join(HERE, "poly_live.py"), "--link", f"127.0.0.1:{port}", "--card", card]
    for t, c in at:
        args += ["--at", f"{t:g}:{c}"]
    return subprocess.Popen(args, stdout=logf, stderr=subprocess.STDOUT)


def run_linked(name, a, snap, card_img, card_dir, length, events, at, clock, save=None):
    port = free_port()
    logp = os.path.join(a.out, f"{name}.log")
    log = open(logp, "w")
    h = host(port, card_dir, log, at)
    time.sleep(1.5)
    r = dtboot(a.emu, snap, card_img, length, events, port=port, png=os.path.join(a.out, f"{name}_screen.png"), clock=clock,
               save=save)
    time.sleep(1.0)
    h.terminate()
    try:
        h.wait(10)
    except subprocess.TimeoutExpired:
        h.kill()
    log.write("\n---- dtboot\n" + r.stdout[-4000:] + r.stderr[-2000:])
    log.close()
    print(f"{name}: dtboot rc {r.returncode}", flush=True)
    return open(logp).read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", default=os.path.expanduser("~/PycharmProjects/worktrees/rytm-emu-seqp2"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "core", "build", "song_e2e"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--prep-only", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    scratch = tempfile.mkdtemp(prefix="song-e2e-", dir=a.out)
    base_snap = os.path.join(a.emu, f"sections/us_{TAG}.snap")
    base_img = os.path.join(a.emu, f"sections/us_{TAG}.img")

    def clone_card(name):
        p = os.path.join(scratch, f"{name}.img")
        subprocess.run(["cp", "-c", base_img, p], check=True)
        return p
    try:
        # ---- 1. the MIDI CONFIG on the emulated Rytm's screen, at the start of each run (a snapshot saved with the link
        # up does not stream again when loaded: each run starts from the base snapshot and walks the menus first)
        walk = [(100 + 80 * i, k) for i, k in enumerate(WALK)]
        W = 100 + 80 * len(WALK) + 200   # M: the walk's length; everything after it is shifted by W
        clock0 = BASE_CLOCK
        prep = base_snap
        # ---- 2. record
        card = os.path.join(scratch, "card")
        os.makedirs(os.path.join(card, "Samples"))
        S = STEP_M
        p0 = W + 600                                   # PLAY
        perf = [(p0, "PLAY"),
                (p0 + 2 * S, "nudge", "0:1", 6, 12),   # BD's A (SRC), six steps
                (p0 + 5 * S, "MUTE", 40), (p0 + 5 * S + 15, "SD"),       # SD muted
                (p0 + 9 * S, "BANK_A", 40), (p0 + 9 * S + 10, "T2"),     # A02 queued: switches at bar 2's end
                (p0 + 7 * S, "nudge", "0:-1", 6, 12),  # BD's A back down
                (p0 + 26 * S, "MUTE", 40), (p0 + 26 * S + 15, "SD"),     # SD unmuted
                (p0 + 40 * S, "STOP")]
        rec_len = p0 + 40 * S + 3000                  # (the host's clock runs some seconds behind the emulator's: room
                                                       #  after STOP for song=state)
        w = W / 200
        at = [(1.0, "song=log:on"), (w + 0.2, "song=check"), (w + 1.0, "song=arm"), (w + 1.6, "song"),
              ((p0 + 40 * S) / 200 + 6.0, "song=state"), ((p0 + 40 * S) / 200 + 9.0, "song")]
        log = run_linked("record", a, prep, clone_card("record"), card, rec_len, walk + perf, at, clock0)
        cfg_line = [ln for ln in log.splitlines() if "at: song -> song:" in ln]
        print("  " + (cfg_line[0][:240] if cfg_line else "no song status"))
        check("record: the Rytm's MIDI CONFIG as SONG RECORD needs it (read from its GLOBAL X dump)",
              bool(cfg_line) and "config ok" in cfg_line[0], cfg_line[0][:200] if cfg_line else "")
        check("record: armed with the start snapshot", "armed: PLAY records" in log)
        proj_dirs = [d for d in os.listdir(os.path.join(card, ".rytm", "songs"))] if os.path.isdir(os.path.join(card, ".rytm", "songs")) else []
        take_path = None
        for d in proj_dirs:
            p = os.path.join(card, ".rytm", "songs", d, "take-001.json")
            if os.path.exists(p):
                take_path = p
        check("record: the take folded into take-001.json, the journal gone", take_path is not None and
              not os.path.exists(take_path[:-5] + ".jsonl"), proj_dirs)
        if not take_path:
            raise SystemExit(1)
        song = json.load(open(take_path))
        evs = song["events"]
        types = [e[1] for e in evs]
        print(f"  take: {len(evs)} events, types {sorted(set(types))}, length {song['length']}")
        for e in evs:
            print("   ", e)
        st = song["start"]
        check("record: the start snapshot (kit X, pattern X, settings X, global X) in the file",
              all(k in st["rytm_sysex"] for k in ("kit_x", "pattern_x", "settings_x", "global_x")))
        ps = [e for e in evs if e[1] == "p" and e[2]["trk"] == 0]
        check("record: BD's knob as NRPN events on its channel (14-bit values, up then down)", len(ps) >= 6 and
              all(ps[i][2]["v"] != ps[i + 1][2]["v"] for i in range(len(ps) - 1)), len(ps))
        mutes = [e for e in evs if e[1] == "mute"]
        check("record: SD muted then unmuted (the mask)", [e[2]["mask"] for e in mutes] == [2, 0], [e[2] for e in mutes])
        sws = [e for e in evs if e[1] == "sw"]
        pcs = [e for e in evs if e[1] == "pc"]
        check("record: A02 queued (pc) and switched on a bar line (sw)", len(pcs) == 1 and pcs[0][2]["to"] == 1 and
              len(sws) == 1 and sws[0][2]["to"] == 1 and sws[0][0] % 1536 == 0, (pcs, sws))
        check("record: PLAY first, STOP last, time in order", types[0] == "play" and types[-1] == "stop" and
              all(evs[i][0] <= evs[i + 1][0] for i in range(len(evs) - 1)))
        # the knob events against the performance: a nudge every 12 M = 0.42 of a step
        if ps:
            first = ps[0][0] / 96
            check("record: the first knob event within a step of where the panel turned it (step 2)", 1.5 <= first <= 3.5, first)
        rec_state = re.findall(r"song: state kit ([0-9a-f]+)", log)
        if a.prep_only:
            return
        # ---- 3. replay into a fresh emulator from the same snapshot
        p1 = W + 1000
        rp = [(p1, "PLAY"), (p1 + 40 * S, "STOP")]
        rep_len = p1 + 40 * S + 3000
        at = [(1.0, "song=log:on"), (w + 0.5, "song=load:take-001"), (w + 1.2, "song=replay"), (w + 1.8, "song=confirm"),
              (w + 4.5, "song"), ((p1 + 40 * S) / 200 + 6.0, "song=state"), ((p1 + 40 * S) / 200 + 9.0, "song")]
        log2 = run_linked("replay", a, prep, clone_card("replay"), card, rep_len, walk + rp, at, clock0)
        st_line = [ln for ln in log2.splitlines() if "at: song -> song:" in ln]
        print("  " + (st_line[0][:240] if st_line else "no status"))
        check("replay: restored and verified before PLAY (backup written first)", "restored (kit and pattern verified)" in log2
              and bool(st_line) and "replay ready" in st_line[0], st_line[:1])
        backups = [f for d in proj_dirs for f in os.listdir(os.path.join(card, ".rytm", "songs", d)) if f.startswith(".backup-")]
        check("replay: the backup of the working state on the card", bool(backups), backups)
        outs = [(int(t), x) for t, x in re.findall(r"song: out t (\d+): ([0-9a-f ]+)", log2)]
        print(f"  replayed messages: {len(outs)}; first {outs[:6]}")
        # each recorded parameter / mute: its replayed message's t
        errs = []
        for e in evs:
            if e[1] == "p":
                want = f"{0xB0 + e[2]['trk']:02x} 26 {e[2]['v'] & 127:02x}"
                hits = [t for t, x in outs if x.strip() == want and abs(t - e[0]) < 400]
            elif e[1] == "mute":
                continue
            else:
                continue
            errs.append(min((t - e[0] for t in hits), key=abs) if hits else None)
        print(f"  replay - record per parameter event (ticks): {errs}")
        check("replay: every knob event sent, within a step of its recorded tick (MIDI out timing)",
              errs and all(x is not None and abs(x) <= 96 for x in errs), errs)
        mo = [(t, x) for t, x in outs if x.startswith("b1 5e")]
        check("replay: SD's mute and unmute as CC 94 near their recorded ticks", len(mo) >= 2 and
              abs(mo[0][0] - mutes[0][0]) <= 96 and abs(mo[1][0] - mutes[1][0]) <= 96, (mo, [m[0] for m in mutes]))
        pcout = [t for t, x in outs if x.startswith("cd 01")]
        check("replay: the PC for A02 ahead of the switch", bool(pcout) and pcout[0] < sws[0][0], (pcout, sws[0][0]))
        sw2 = [(int(fr), int(to), int(t)) for fr, to, t in re.findall(r"song: switch (\d+) -> (\d+) at t (\d+)", log2)]
        check("replay: the Rytm switched to A02 on the recorded bar", any(to == 1 and abs(t - sws[0][0]) <= 96 for _, to, t in sw2),
              (sw2, sws[0][0]))
        rep_state = re.findall(r"song: state kit ([0-9a-f]+)", log2)
        print(f"  end state: record kit {rec_state}, replay kit {rep_state}")
        check("replay: the Rytm's working kit at the end equal to the record run's (song=state)",
              bool(rec_state) and bool(rep_state) and rec_state[-1] == rep_state[-1], (rec_state, rep_state))
        shutil.copy(take_path, os.path.join(a.out, "take-001.json"))
    finally:
        if not a.keep:
            shutil.rmtree(scratch, ignore_errors=True)
    if FAILS:
        print(f"FAILED: {FAILS}")
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
