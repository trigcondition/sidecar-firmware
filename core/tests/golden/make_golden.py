#!/usr/bin/env python3
"""core/tests/golden/make_golden.py - the format registry's golden files (core/fmt, core/tests/test_fmt.cpp): one folder
per registered format, v<N>/<name> a file as that version of the Sidecar writes it, expected/<name>.current what
fmt::load gives for it (only where it differs from the input: an upgrade, or the writer's own canonical form). Made from
today's writers' formats (the module that writes each is named below); the .sqp files with sim/protocol.py's sqp_pack.
Run from the repository: python3 core/tests/golden/make_golden.py (idempotent). No firmware data: our own formats only."""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "sim"))
import protocol as P  # noqa: E402


def put(fmt, ver, name, data):
    d = os.path.join(HERE, fmt, "v%d" % ver)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def expect(fmt, name, data):
    d = os.path.join(HERE, fmt, "expected")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".current"), "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


# ---- class P: per-project content
put("pool", 1, "live.txt", "Live/s0.wav\nLive/s1.wav\n\nDrums/Kicks/kick 01.wav\n")   # pool.cpp pool_save
put("pool", 1, "empty.txt", "")
put("pool", 1, "crlf.txt", "Live/s0.wav\r\nLive/s1.wav\r\n")
put("bankpool", 1, "granular.pool.txt", "/Samples/Live/s0.wav\n\n/GRANULAR/pad.wav\n" + "\n" * 125 + "/Samples/Drums/b1.wav\n")
put("picks", 1, "live.txt", "0 2 0 /Samples/Live/s0.wav\n3 7 1 /GRANULAR/pad.wav\n")   # granular.cpp picks_save
put("slots", 1, "live.txt", "U1 juno60 1.2.0\nU3 dexed 0.9.12\n")                     # rmp_slots_format
put("slotnotes", 1, "live.inf", "U1 saved 0 1234 juno60 1.2.0 JUNO\n")                 # rmp_notes_format
put("fxslots", 1, "live.txt", "1a2b SHIMMER\n\n0c0d TAPE\n")                          # send_fx_card.cpp
put("juno", 1, "live.txt", "00112233445566778899aabbccddeeff0011\tBRASS 1\n\n00112233445566778899aabbccddeeff0012\n")
put("fm", 1, "live.txt", "00112233445566778899aabbccddeeff\t3\tE.PIANO\n\n")          # dx7_lib.h save
put("lfo", 1, "k001.txt", "# the kit's LFOs\nlfo1 track=1 dest=3 rate=40 depth=64\n")  # (lines as the module reads them)
put("ins", 1, "k001.txt", "# INSERT FX\nins 0 type=2 p=10,20,30\n")
# .sqp: v2 (no conditions) and v3 (a condition) as the core writes them; v1 = a v2 file without locks, version byte 1
tr = [dict(track=0, steps={0: dict(notes=[(60, 100, 24, 0)]), 4: dict(notes=[(62, 90, 48, 3), (65, 80, 48, 0)])})]
v2 = P.sqp_pack(tr)
put("sqp", 2, "p000.sqp", v2)
v1 = v2[:4] + bytes([1]) + v2[5:]
put("sqp", 1, "p000.sqp", v1)
tr3 = [dict(track=2, steps={1: dict(flags=1 | (5 << 2), notes=[(36, 127, 12, 0)], locks=[(3, 0x1234)])})]
v3 = P.sqp_pack(tr3)
assert v3[4] == 3
put("sqp", 3, "p001.sqp", v3)
# seqx (E2): v4 = v3 + the block trailer. The registry reads v4 natively and upgrades v1..v3 in memory (1 -> 2 -> 3 ->
# 4: the body as the v2 writer gives it, the version byte 4); a v4 file with a fixed seed, a track's seed, a poly config
# and a block this build does not know (kept verbatim)
expect("sqp", "p000.sqp", v2[:4] + bytes([4]) + v2[5:])
expect("sqp", "p001.sqp", v3[:4] + bytes([4]) + v3[5:])
tr4 = [dict(track=3, poly=(4, 1, 2, 30, 64, 0), tseed=(1, 77, 1),
            steps={0: dict(notes=[(60, 100, 24, 0), (64, 100, 24, 0)]), 2: dict(prob=50, notes=[(67, 90, 24, 0)])})]
v4 = P.sqp_pack(tr4, seed=(1, 41), blocks=[(0x40, P.SQP_BLK_TRACK, bytes([3, 1, 2, 3]))])
assert v4[4] == 4
put("sqp", 4, "p002.sqp", v4)
# seqtop: a v4 file with the top-8 features' blocks (required: an older v4 Sidecar locks it) - P2 a track's speed, P1 its
# direction, R1 random notes (a flagged step), R1b SEED MOD, V1 a lock's condition, P4 / M4 a ratchet and an arp
tr5 = [dict(track=1, length=12, speed=P.SEQ_SPEEDS.index("2/3"), dir=P.SEQ_DIRS.index("PEND"), rnd=(1, 50, 3, 2),
            smod=(9, 8, 2, 1),
            steps={0: dict(notes=[(60, 100, 24, 0), (63, 90, 24, 0), (67, 80, 24, 0)], locks=[(2, 90 << 7, 9 + 2), (5, 7 << 7)],
                           slide=[5]),
                   3: dict(notes=[(62, 100, 48, 6)], rnd=1, rt=(3, 6, 1, -2, 0, 1, 50)),
                   5: dict(notes=[(48, 100, 96, 0), (55, 90, 96, 0)], rt=(0, 7, 0, 0, P.SEQ_RT_MODES.index("UPDN"), 2, 0),
                           fx=((P.SEQ_FX_CMDS.index("OCT"), -1), (P.SEQ_FX_CMDS.index("CHN"), 50))),
                   9: dict(notes=[], fx=((P.SEQ_FX_CMDS.index("JMP"), 2), (0, 0)))})]
# (V1: step 1's P3 lock on 1:3, the COND code 11; X1 its P6 lock slides; P4 step 4 three repeats; M4 step 6 an UPDN arp
# over 2 octaves; H1 step 6 OCT -1 on a 50 % chance, the empty step 10 jumps to step 2)
v5 = P.sqp_pack(tr5, scale=(2, P.SEQ_SCALES.index("DOR"), 2))   # M1: D dorian, QNT PLAY
assert v5[4] == 4
put("sqp", 4, "p003.sqp", v5)
put("tracks", 1, "tracks.txt", "mask=0x005\nlat=12\nquant=1\npin=0x000\npinseq=1\nlatset=0\n")   # seq.cpp save_tracks
put("song", 1, "take-1.json", '{"format":"sidecar-song","version":1,"project":"LIVE","name":"take-1","length":1920,'
    '"events":[]}\n')
# ---- class S: settings
put("display", 1, "display.txt", "screensaver=10\nbrightness=80\ntouchtest=0\nuistyle=oled\nres=auto\n")
put("keys", 1, "keys.txt", "mode=selected\nauto=10\nmac=1\n")
put("fxin", 1, "fxin.txt", "src=fx\n")
put("sendfx", 1, "sendfx.txt", "out=fx\n")
put("wifi", 1, "wifi.txt", "# Sidecar Wi-Fi\non=1\nssid=studio\nband=2g\n")
put("project", 1, "project", "RYTMLINJE")
put("projects", 1, "projects.txt", "LIVE\tLIVE\t3\t2026-10-22\t-\nAR_PERC\tAR PERC\t1\t-\ti\n")
put("sdtest", 1, "sdtest.txt", "ahead=120\nstreams=24\n")
# ---- class D
put("cardindex", 4, "env", "hash=0123456789abcdef\nversion=4\nfolders=3\nfiles=12\n")
put("dx7idx", 1, "dx7.bin", b"DX7IDX1\x00" + struct.pack("<IIII", 1, 2, 0, 0))
# ---- class X (sniffed only): an RMPA header (format 3), a bundle's first local header + manifest
put("rmpa", 3, "demo.rmp", b"RMPA" + struct.pack("<HH", 3, 9) + b"\x00" * 88)
man = b'{"format":"sidecar-bundle","version":1,"min_reader":1,"entries":[]}'
lh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH", 20, 0x0800, 0, 0, 0, zlib.crc32(man), len(man), len(man), 13, 0)
put("bundle", 1, "demo.sidecar.zip", lh + b"manifest.json" + man)
# ---- the library's own (core/lib): an empty index (header + no records), a log, a state file
body = struct.pack("<IIII", 0, 0, 0, 0)   # records, path bytes, flags, reserved
hdr = b"SLIB" + struct.pack("<HHII", 1, 1, 16, zlib.crc32(body))
put("library", 1, "index.bin", hdr + body)
put("library-log", 1, "log.txt", "#sidecar library-log 1\n- /Samples/old.wav\n> /Samples/a.wav /Samples/b.wav\n")
put("library-state", 1, "state.txt", "format=library-state/1\ntree=0123456789abcdef\nfiles=0\n")
# ---- the pool lists (core/lib/pools_fmt.h), as pools.cpp writes them: sparse rows, refs with and without a hash,
# the cached extras, a long name (39 characters), inputs / built-ins, an instrument with members; the used set
SHA1 = "9f2c" + "0" * 56 + "e1a0"
SHA2 = "4be0" + "1" * 56 + "0711"
SHA3 = "77aa" + "2" * 58 + "c3"
put("samplepool", 1, "template.txt", "#sidecar samplepool 1\n" + "".join(
    "H%03d input %s\n" % (i + 1, n) for i, n in enumerate(["IN", "MAIN", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB"]))
    + "H011 builtin PAD\nH012 builtin BELLS\n")
put("samplepool", 1, "live.txt", "#sidecar samplepool 1\nA001 sample %s 412044 /Samples/Drums/kick.wav\tkick\t103000 2 48000\n"
    "A012 sample - 96044 /Samples/LIVE/take 3.wav\ttake 3\n"
    "C127 sample %s 10 /GRANULAR/a very long folder name/choir.wav\t%s\n"
    "H001 input IN\nH011 builtin PAD\n" % (SHA1, SHA2, "N" * 39))
put("wtpool", 1, "template.txt", "#sidecar wtpool 1\nA001 builtin BASIC\nA002 builtin PWM\nA003 builtin VOCAL\n"
    "A004 builtin ORGAN\nA005 builtin FM\nA006 builtin SYNC\n")
put("wtpool", 1, "live.txt", "#sidecar wtpool 1\nA001 builtin BASIC\nB009 sample %s 131116 /WAVETABLES/saw_wt64.wav\tsaw_wt64\t"
    "131072 1 48000\n" % SHA1)
put("instpool", 1, "empty.txt", "#sidecar instpool 1\n")
put("instpool", 1, "live.txt", "#sidecar instpool 1\nB003 instrument %s 900 /MULTISAMPLE/Piano/Piano.sfz\tPiano\n"
    " m %s 1920044 /MULTISAMPLE/Piano/Piano_C3.wav\n m - 1920044 /MULTISAMPLE/Piano/Piano_C4.wav\n"
    "B004 instrument - 120 /Samples/LIVE/auto/auto.sfz\tauto\n m %s 40044 /Samples/LIVE/auto/auto.wav\n" % (SHA2, SHA3, SHA1))
put("poolused", 1, "live.txt", "#sidecar poolused 1\nsmp A001 12\nsmp H001 3\nwt B009 1\ninst B003 40\n")
# ---- CHOP's slice maps (core/chop/slicemap.cpp write / upgrade_v1): v2 as CHOP writes it (every per-slice setting, a
# RESAMPLE'd slice playing its own file), v1 the sampler's slice map (core/samp/sampler.cpp slices_sfz) and its upgrade
SM_HEAD = ("// sidecar slicemap 2\n// CHOP's slices of one sample (docs/CHOP.md): a region a slice; sc_* opcodes are "
           "CHOP's, other players skip them\n<control> default_path=\n<global>")
put("slicemap", 2, "amen.chop.sfz", SM_HEAD + " sc_sha=%s sc_size=768044 sc_frames=192000 sc_rate=48000 sc_ch=2 "
    "sc_name=AMEN BREAK sample=/Samples/LIVE/amen.wav\n"
    "<region> key=36 offset=0 end=23999 loop_mode=one_shot\n"
    "<region> key=37 offset=24000 end=47999 transpose=3 tune=-12 volume=-2 direction=reverse ampeg_attack=0.005 "
    "ampeg_decay=0.300 ampeg_sustain=0 loop_mode=loop_continuous loop_type=alternate loop_start=24000 loop_end=47999 "
    "group=2 off_by=2 sc_nudge=-120 sc_stut=4 sc_stutlen=25 sc_stutdec=20 sc_tape=stop sc_tapems=400 sc_nbr=30\n"
    "<region> key=38 offset=0 sc_from=48000 sc_to=96000 loop_mode=one_shot sample=/Samples/LIVE/AMEN_BREAK-S3.wav\n"
    "<region> key=39 offset=96000 end=191999 sc_tape=spin sc_tapems=200 loop_mode=one_shot\n" % SHA1)
V1 = ("// Sidecar sample TAKE_001: 4 slices (transients), 48 kHz; key 36 (C1) is the first\n"
      "<global> loop_mode=one_shot ampeg_attack=0.001 ampeg_release=0.02\n")
V1S = [(0, 12000), (12000, 24500), (24500, 36000), (36000, 48000)]
put("slicemap", 1, "TAKE_001-slices.sfz", V1 + "".join(
    "<region> sample=TAKE_001.wav key=%d pitch_keycenter=%d offset=%d end=%d\n" % (36 + i, 36 + i, a, b - 1)
    for i, (a, b) in enumerate(V1S)))
expect("slicemap", "TAKE_001-slices.sfz", SM_HEAD + " sc_sha=- sc_size=0 sc_frames=0 sc_rate=0 sc_ch=1 sample=TAKE_001.wav\n"
       + "".join("<region> key=%d offset=%d end=%d loop_mode=one_shot\n" % (36 + i, a, b - 1) for i, (a, b) in enumerate(V1S)))
print("golden files in", HERE)
