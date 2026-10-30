"""User machines on a card store (core/user/um.cpp, the Pi's and the Mac core lib's) with real RMPA packages run
in place: the scan and the installed list, 1E assign / clear and their 1D answers, 1A with the fingerprint, the slot
map file, playing (the sine at 440 Hz), the sound tag, enum names, a package removed (MISSING: the Rytm's !NAME),
a stale list refused, the version range, duplicates, a Teensy package and junk listed as not fitting, the
console, the JSON (docs/user-machines-pi.md).

    make -C core engines lib && for p in sine_test v2_test; do make -C plugins/$p a64; done
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_um
One engine per process (the core keeps module state): each test runs its scenario in a subprocess.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


def rd(p):
    with open(p, "rb") as f:
        return f.read()


def wr(p, b):
    with open(p, "wb" if isinstance(b, (bytes, bytearray)) else "w") as f:
        f.write(b)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402

PKG = {n: os.path.join(REPO, "plugins", n, f"{n}.a64.rmp") for n in ("sine_test", "v2_test")}
ARM = os.path.join(REPO, "plugins", "sine_test", "sine_test.rmp")
HAVE = all(os.path.exists(p) for p in PKG.values())


def scenario(card, script):
    """Run `script` (python, with e = CoreEngine(card, um=True) and helpers) in a fresh process; its stdout's last
    line is JSON."""
    pre = f"""
import json, sys, numpy as np
sys.path.insert(0, {os.path.join(HERE, '..')!r})
import protocol as P
from coreengine import CoreEngine
e = CoreEngine({card!r}, um=True)
def tx(m):
    e.report(m); e.idle()
    return [x for x in e.take_sysex()]
def of(msgs, cmd): return [x for x in msgs if x[4] == cmd]
def inst():
    m = of(tx(P.ask(0, 0, P.ASK_INSTALLED)), P.LIST)
    names = [n for x in m for (_, n) in P.decode_list(x[5:-1])["entries"]]
    return m[0][8], names
def meta(s):
    return [P.decode_meta(x[5:-1]) for x in of(tx(P.ask(s, 0, P.ASK_META)), P.META)]
def play(vb, note=69, tag=None, n=4800):
    e.report(P.trig_params(vb, note, 100, [64 << 7] * 16, tag)); e.idle()
    a = e.frames(n)[:, 0].astype(float)
    f = np.abs(np.fft.rfft(a))
    return float(np.abs(a).max()), float(np.argmax(f) * 48000 / len(a))
"""
    r = subprocess.run([sys.executable, "-c", pre + script], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stderr[-2000:])
    return json.loads(r.stdout.strip().splitlines()[-1])


@unittest.skipUnless(HAVE, "build the RMPA packages: make -C plugins/sine_test a64; make -C plugins/v2_test a64")
class TestCoreUm(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="um-")
        os.makedirs(os.path.join(self.card, "Samples"))
        os.makedirs(os.path.join(self.card, "Plugins"))
        for n, p in PKG.items():
            shutil.copy(p, os.path.join(self.card, "Plugins", f"{n}.rmp"))

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def test_assign_play_tag_missing(self):
        r = scenario(self.card, """
out = {}
gen, names = inst()
out["names"] = names
tx(P.HDR + bytes([0x10]) + b"UMTEST" + b"\\xf7")
k = names.index("SINE TEST")
m = tx(P.assign(1, P.OP_ASSIGN, k, gen))
out["answer"] = [P.decode_slots(x[5:-1]) for x in of(m, P.SLOTS)]
mm = meta(1)
out["meta0"] = mm[0]
out["parts"] = len(mm)
out["play"] = play(0x50)
out["tag_ok"] = play(0x50, tag=P.plugin_fp("rytm.sine_test"))
out["tag_bad"] = play(0x50, tag=P.plugin_fp("rytm.sine_test") ^ 1)
out["tag_back"] = play(0x50, tag=0)
out["json_played"] = e.um_json()
out["stale"] = [P.decode_slots(x[5:-1]) for x in of(tx(P.assign(2, P.OP_ASSIGN, 0, (gen + 5) & 127)), P.SLOTS)]
import os
os.remove({pk!r})
print(e.um_command("um=rescan"))
e.idle()
out["missing"] = meta(1)[0]
out["missing_play"] = play(0x50)
out["json"] = e.um_json()
print(json.dumps(out))
""".replace("{pk!r}", repr(os.path.join(self.card, "Plugins", "sine_test.rmp"))))
        self.assertEqual(sorted(r["names"]), ["SINE TEST", "V2 TEST"])
        self.assertEqual(r["answer"][-1]["answer"], [1, P.RES_READY])
        m0 = r["meta0"]
        self.assertEqual((m0["state"], m0["name"], m0["fp"]), (P.ST_READY, "SINE TEST", P.plugin_fp("rytm.sine_test")))
        self.assertEqual(r["parts"], 2)   # head + one part of 3 knobs
        peak, f = r["play"]
        self.assertGreater(peak, 1000)
        self.assertAlmostEqual(f, 440.0, delta=12)
        self.assertGreater(r["tag_ok"][0], 1000)
        self.assertEqual(r["tag_bad"][0], 0.0)    # a sound made for another machine: silent
        self.assertGreater(r["tag_back"][0], 1000)  # an untagged trig plays again
        # the measured cost: U1 played, so its slot and its package carry a render time per voice block
        jp = r["json_played"]
        s1 = [x for x in jp if x["t"] == "slot"][0]
        self.assertGreater(s1["blocks"], 0); self.assertGreater(s1["ns"], 0); self.assertLess(s1["ns"], 250000)
        self.assertGreater(max(x["ns"] for x in jp if x["t"] == "plugin"), 0)
        self.assertEqual([x for x in jp if x["t"] == "plugins"][0]["block_ns"], 250000)
        self.assertEqual(r["stale"][-1]["answer"], [2, P.RES_NOT_INSTALLED])
        mi = r["missing"]
        self.assertEqual((mi["state"], mi["name"], mi["fp"]), (P.ST_MISSING, "MISS SINE", 0))   # the Rytm: !MISS SINE (the reason + the short name)
        self.assertEqual(r["missing_play"][0], 0.0)
        slots = [x for x in r["json"] if x["t"] == "slot"]
        self.assertEqual(slots[0]["state"], "MISSING")
        self.assertEqual(rd(os.path.join(self.card, ".rytm", "slots", "UMTEST.txt")), b"U1 rytm.sine_test 1.0.0\n")

    def test_instances(self):
        """seqx (E0): a package whose header opts in (RMP_HF_INSTANCES, max_voices 4) plays the sequencer's notes on
        instances of its own - two sines at once on one voice; the same package without the flag (every older one)
        stays mono: the second note retriggers the voice's one state."""
        pk = os.path.join(self.card, "Plugins", "sine_test.rmp")
        d = bytearray(rd(pk))
        self.assertEqual(struct.unpack_from("<I", d, 68)[0], 0)   # (the header's flags: an older package's 0)
        r0 = scenario(self.card, self.INST)
        struct.pack_into("<I", d, 68, 0x1 | 3 << 4)   # max_voices 4 (the header's flags are outside its CRC)
        wr(pk, bytes(d))
        r1 = scenario(self.card, self.INST)
        self.assertIn("mono 1/1", r0["one"])
        self.assertIn("mono", r0["two"])
        self.assertEqual(len(r0["peaks"]), 1)                 # one sine (the second note took the voice)
        self.assertIn("inst 1/4", r1["one"])
        self.assertIn("inst 2/4", r1["two"])
        self.assertEqual(len(r1["peaks"]), 2)                 # two sines at once: 440 and 660 Hz
        self.assertIn("v1 U1 inst 2/4", r1["status"])

    INST = """
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"UMTEST" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("SINE TEST"), gen))
e.report(P.params_report(0x50, [64 << 7] * 16)); e.idle()   # voice 0 on U1 (a 0x03: no trig)
out = {}
out["one"] = e.poly_command("inst=on:1:69:100:4")
out["two"] = e.poly_command("inst=on:1:76:100:4")   # a fifth up: 659 Hz
a = e.frames(9600)[:, 0].astype(float)
f = np.abs(np.fft.rfft(a))
fr = np.fft.rfftfreq(len(a), 1 / 48000)
pk = [int(round(fr[i])) for i in range(1, len(f) - 1) if f[i] > f[i - 1] and f[i] >= f[i + 1] and f[i] > 0.2 * f.max()]
out["peaks"] = pk
out["status"] = e.poly_command("inst")
print(json.dumps(out))
"""

    def test_damaged_map_kept(self):
        """1042 (data safety): a slot map or notes file this build cannot parse (an unknown line - a newer Sidecar's, a
        damaged one) is never written over: assigning a slot plays it but the files keep their bytes."""
        d = os.path.join(self.card, ".rytm", "slots")
        os.makedirs(d)
        bad_map = b"U1 rytm.v2_test 1.0.0\nU2 rytm.sine_test 1.0.0 extra-field-from-a-newer-sidecar\n"
        bad_inf = b"this is not a notes file\n\x00\x01"
        wr(os.path.join(d, "UMTEST.txt"), bad_map)
        wr(os.path.join(d, "UMBAD2.txt"), b"U1 rytm.v2_test 1.0.0\n")
        wr(os.path.join(d, "UMBAD2.inf"), bad_inf)
        r = scenario(self.card, """
out = {}
gen, names = inst()
k = names.index("SINE TEST")
for p in ("UMTEST", "UMBAD2"):
    tx(P.HDR + bytes([0x10]) + p.encode() + b"\\xf7")
    for _ in range(20): e.idle()
    m = tx(P.assign(3, P.OP_ASSIGN, k, gen))
    for _ in range(20): e.idle()
    out[p] = [P.decode_slots(x[5:-1]) for x in of(m, P.SLOTS)][-1]["answer"]
print(json.dumps(out))
""")
        self.assertEqual(r["UMTEST"], [3, P.RES_READY])   # the slot plays (in memory)
        self.assertEqual(rd(os.path.join(d, "UMTEST.txt")), bad_map)   # the unparsable map: its bytes kept
        self.assertEqual(rd(os.path.join(d, "UMBAD2.inf")), bad_inf)   # the unparsable notes: kept

    def test_enum_clear_console_json(self):
        r = scenario(self.card, """
out = {}
print(e.um_command("um=assign:2:rytm.v2_test"))
e.idle()
mm = meta(2)
out["meta0"] = mm[0]
m = of(tx(P.ask(2, 0, P.ASK_ENUM, 5)), P.LIST)
out["enum"] = [n for x in m for (_, n) in P.decode_list(x[5:-1])["entries"]]
out["unchanged"] = [x.hex() for x in of(tx(P.ask(2, mm[0]["gen"], P.ASK_META)), P.META)]
m = tx(P.assign(2, P.OP_CLEAR, 0, 0))
out["clear"] = [P.decode_slots(x[5:-1]) for x in of(m, P.SLOTS)]
out["after"] = meta(2)[0]
out["json"] = e.um_json()
print(json.dumps(out))
""")
        self.assertEqual((r["meta0"]["state"], r["meta0"]["name"]), (P.ST_READY, "V2 TEST"))
        self.assertEqual(r["enum"], ["SINE", "POOL"])
        self.assertEqual(len(r["unchanged"]), 1)
        self.assertEqual(r["clear"][-1]["answer"], [2, P.RES_READY])
        self.assertEqual(r["after"]["state"], P.ST_EMPTY)
        kinds = {x["t"] for x in r["json"]}
        self.assertTrue({"plugin", "slot", "plugins", "mem", "mem_end"} <= kinds)
        pl = [x for x in r["json"] if x["t"] == "plugin"]
        self.assertTrue(all(x["fit"] and x["target"] == "a64" for x in pl))
        mem = {x["k"]: x for x in r["json"] if x["t"] == "mem"}
        self.assertTrue({"code", "data", "state"} <= set(mem))

    def test_enum_names_from_the_scan_not_the_card(self):
        # the ENUM ASK is answered in the SysEx handler on the control core: from the names the scan cached, never a
        # card read (3 ms a time on the Pi: an underrun on every ask). With the package gone from the card after the
        # scan, the names still come.
        r = scenario(self.card, f"""
import os
out = {{}}
e.um_command("um=assign:2:rytm.v2_test"); e.idle()
meta(2)
os.remove({os.path.join(self.card, "Plugins", "v2_test.rmp")!r})
m = of(tx(P.ask(2, 0, P.ASK_ENUM, 5)), P.LIST)
out["enum"] = [n for x in m for (_, n) in P.decode_list(x[5:-1])["entries"]]
print(json.dumps(out))
""")
        self.assertEqual(r["enum"], ["SINE", "POOL"])

    def test_versions_duplicates_foreign(self):
        # a copy of sine_test claiming 1.1.0 (header rewritten, CRC unchanged: the body is the same), a Teensy
        # package, junk: the list shows all, the Rytm's list only what loads here
        import struct
        d = bytearray(rd(PKG["sine_test"]))
        struct.pack_into("<I", d, 40, 0x010100)   # version 1.1.0 (compatible from 1.0.0)
        wr(os.path.join(self.card, "Plugins", "sine_new.rmp"), d)
        if os.path.exists(ARM):
            shutil.copy(ARM, os.path.join(self.card, "Plugins", "sine_arm.rmp"))
        wr(os.path.join(self.card, "Plugins", "junk.rmp"), b"not a package" * 10)
        bad = bytearray(rd(PKG["v2_test"]))
        bad[len(bad) // 2] ^= 1
        wr(os.path.join(self.card, "Plugins", "v2_bad.rmp"), bad)
        os.makedirs(os.path.join(self.card, ".rytm", "slots"))
        wr(os.path.join(self.card, ".rytm", "slots", "_default.txt"), "U1 rytm.sine_test 1.0.0\nU3 rytm.sine_test 2.0.0\n")
        r = scenario(self.card, """
out = {}
gen, names = inst()
out["names"] = names
out["u1"] = meta(1)[0]
out["u3"] = meta(3)[0]
out["json"] = [x for x in e.um_json() if x["t"] == "plugin"]
print(json.dumps(out))
""")
        self.assertEqual(sorted(r["names"]), ["SINE TEST", "V2 TEST"])   # one SINE (the newer), no junk
        self.assertEqual(r["u1"]["state"], P.ST_READY)     # 1.0.0 is in 1.1.0's range
        self.assertEqual(r["u3"]["state"], P.ST_MISSING)   # 2.0.0 is not installed
        why = {os.path.basename(x["file"]): (x["fit"], x["why"]) for x in r["json"]}
        self.assertFalse(why["junk.rmp"][0])
        self.assertIn("not a machine package", why["junk.rmp"][1])
        self.assertIn("CRC", why["v2_bad.rmp"][1])
        self.assertTrue(why["sine_test.rmp"][0])   # phase 4: the older copy is kept (a project may want exactly it)
        self.assertTrue(why["sine_new.rmp"][0])
        if os.path.exists(ARM):
            self.assertIn("Teensy", why["sine_arm.rmp"][1])

    def test_versions_side_by_side(self):
        # bundles phase 4 (docs/BUNDLES.md "Exactness"): 1.0.0 and 1.1.0 of one id installed side by side; a slot plays
        # exactly the version its map names, the newest compatible one when that one is not installed (and only the
        # newest is in the Rytm's list); a version nothing serves stays MISSING; the older copy is kept, not refused
        import struct
        d = bytearray(rd(PKG["sine_test"]))
        struct.pack_into("<I", d, 40, 0x010100)   # 1.1.0 (compatible from 1.0.0)
        wr(os.path.join(self.card, "Plugins", "rytm.sine_test-1.1.0.rmp"), d)
        os.makedirs(os.path.join(self.card, ".rytm", "slots"))
        wr(os.path.join(self.card, ".rytm", "slots", "_default.txt"),
           "U1 rytm.sine_test 1.0.0\nU2 rytm.sine_test 1.1.0\nU3 rytm.sine_test 1.0.5\nU4 rytm.sine_test 1.2.0\n")
        r = scenario(self.card, """
out = {}
gen, names = inst()
out["names"] = names
out["slots"] = [x for x in e.um_json() if x["t"] == "slot"]
out["plugins"] = [x for x in e.um_json() if x["t"] == "plugin"]
out["play1"] = play(0x50)[1]
print(json.dumps(out))
""")
        self.assertEqual(sorted(r["names"]), ["SINE TEST", "V2 TEST"])   # one SINE TEST in the Rytm's list: the newest
        sl = {x["slot"]: x for x in r["slots"]}
        self.assertEqual(sl[1]["state"], "READY")
        self.assertEqual(os.path.basename(sl[1]["file"]), "sine_test.rmp")      # exactly 1.0.0
        self.assertEqual(sl[1]["play_ver"], "1.0.0")
        self.assertEqual(os.path.basename(sl[2]["file"]), "rytm.sine_test-1.1.0.rmp")
        self.assertEqual(sl[2]["play_ver"], "1.1.0")
        self.assertEqual(sl[3]["state"], "READY")                                # 1.0.5: the newest serves it
        self.assertEqual(sl[3]["play_ver"], "1.1.0")
        self.assertEqual(sl[4]["state"], "MISSING")                              # 1.2.0: nothing serves it
        self.assertIn("1.2.0", sl[4]["why"])
        pl = {os.path.basename(x["file"]): x for x in r["plugins"]}
        self.assertTrue(pl["sine_test.rmp"]["fit"])                              # kept beside the newer one
        self.assertTrue(pl["sine_test.rmp"]["older"])
        self.assertFalse(pl["rytm.sine_test-1.1.0.rmp"]["older"])
        self.assertGreater(r["play1"], 400)

    def test_resolve_prints_inherit_lazy_import(self):
        # which machine a slot plays when a project loads (rmp_resolve.h): a shared project with no map resolves from
        # the Rytm's 0x25 (a repeat ignored), a fresh project inherits the previous one's slots, an older Rytm's
        # trig tag fills an empty slot lazily, explicit empty stays, an import names a machine not installed
        os.makedirs(os.path.join(self.card, ".rytm", "slots"))
        wr(os.path.join(self.card, ".rytm", "slots", "OWN.txt"), "U2 rytm.sine_test 1.0.0\n")
        r = scenario(self.card, """
import time
out = {}
def proj(n): tx(P.HDR + bytes([0x10]) + n.encode() + b"\\xf7")
def slots(): return {x["slot"]: x for x in e.um_json() if x["t"] == "slot"}
def settle():
    for _ in range(40): e.idle()
SINE, V2 = P.plugin_fp("rytm.sine_test"), P.plugin_fp("rytm.v2_test")
inst(); settle()
# an older Rytm (no 0x25 seen yet): the project OWN keeps its map, U1 fills lazily from a trig's tag
proj("OWN"); settle()
out["own"] = {k: (v["id"], v["src"]) for k, v in slots().items()}
out["lazy_play"] = play(0x50, tag=SINE)
settle()
out["lazy"] = slots()[1]
out["lazy_play2"] = play(0x50, tag=SINE)
# a fresh project (no map) inherits OWN's slots after 1.5 s (no 0x25 for it came meanwhile)
proj("FRESH1"); settle()
out["inherit_early"] = slots()[1]["id"]
time.sleep(1.6); settle()
out["inherit"] = {k: (v["id"], v["src"]) for k, v in slots().items()}
# a shared project, this Rytm now sends 0x25: U3 from the sounds, U1 conflicts with nothing (no info): inherited
proj("SHARED"); tx(P.slot_prints([(3, V2, 4), (4, 999, 2)], seq=1)); settle()
s = slots()
out["shared"] = {k: (v["id"], v["src"], v["unknown"], v["want_fp"]) for k, v in s.items()}
out["inf"] = open({card!r} + "/.rytm/slots/SHARED.inf").read()
out["txt"] = open({card!r} + "/.rytm/slots/SHARED.txt").read()
# the user clears U3 (explicit empty); the same report again is ignored, a new project load keeps it empty
print(e.um_command("um=clear:3")); settle()
tx(P.slot_prints([(3, V2, 4), (4, 999, 2)], seq=1)); settle()
out["cleared"] = slots()[3]["src"], slots()[3]["id"]
proj("OWN"); settle(); proj("SHARED"); tx(P.slot_prints([(3, V2, 4)], seq=1)); settle()
out["cleared2"] = slots()[3]["src"], slots()[3]["id"]
# saved vs sounds: OWN's U2 is sine_test, its sounds now say v2_test -> updated from the project's sounds
proj("OWN"); tx(P.slot_prints([(2, V2, 3)], seq=2)); settle()
out["updated"] = slots()[2]["id"], slots()[2]["src"], slots()[2]["updated"]
# an import: a machine not installed is MISSING by name; "start this project empty"
print(e.um_command("um=put:5:ext.wobble:0.3.0:WOBBLE")); settle()
out["import"] = slots()[5]["id"], slots()[5]["state"], slots()[5]["want_name"], slots()[5]["src"]
print(e.um_command("um=empty")); settle()
out["empty"] = [v["src"] for v in slots().values()]
pl = [x for x in e.um_json() if x["t"] == "plugins"][0]
out["prints"] = pl["prints"]
print(json.dumps(out))
""".replace("{card!r}", repr(self.card)))
        self.assertEqual(r["own"]["2"], ["rytm.sine_test", "saved"])
        self.assertEqual(r["own"]["1"], ["", "none"])
        self.assertEqual(r["lazy"]["id"], "rytm.sine_test")    # filled from the trig's tag
        self.assertEqual(r["lazy"]["src"], "sounds")
        self.assertGreater(r["lazy_play2"][0], 1000)           # and plays from the next trig on
        self.assertEqual(r["inherit_early"], "")              # waits for a 0x25 first
        self.assertEqual(r["inherit"]["1"], ["rytm.sine_test", "inherited"])
        self.assertEqual(r["inherit"]["2"], ["rytm.sine_test", "inherited"])
        sh = r["shared"]
        self.assertEqual(sh["3"][:2], ["rytm.v2_test", "sounds"])
        self.assertEqual(sh["1"][:2], ["rytm.sine_test", "inherited"])   # no fingerprint information: inherited
        self.assertTrue(sh["4"][2]); self.assertEqual(sh["4"][3], 999)      # needs a machine with fingerprint 999
        self.assertIn("U3 rytm.v2_test 1.0.0", r["txt"])
        self.assertIn("U3 sounds", r["inf"])
        self.assertEqual(r["cleared"], ["empty", ""])
        self.assertEqual(r["cleared2"], ["empty", ""])
        self.assertEqual(r["updated"], ["rytm.v2_test", "sounds", True])
        self.assertEqual(r["import"], ["ext.wobble", "MISSING", "WOBBLE", "imported"])
        self.assertEqual(r["empty"], ["empty"] * 5)
        self.assertEqual(r["prints"], "rytm")


STARTER = ("sc_acid", "sc_wavepad", "sc_metal", "sc_snare", "sc_pluck", "sc_grain")
STARTER_PKG = {n: os.path.join(REPO, "plugins", n, f"{n}.a64.rmp") for n in STARTER}


@unittest.skipUnless(all(os.path.exists(p) for p in STARTER_PKG.values()),
                     "build the starter set: for p in sc_acid sc_wavepad sc_metal sc_snare sc_pluck sc_grain; do make -C plugins/$p a64; done")
class TestStarterSet(unittest.TestCase):
    """Each starter machine (plugins/sc_*) as an RMPA package on the card: listed, assigned to U1, a trig with its
    manifest's defaults (what the Rytm's slot menu gives a new sound) sounds, finite, below full scale."""

    def test_starter_set(self):
        card = tempfile.mkdtemp(prefix="um-")
        try:
            os.makedirs(os.path.join(card, "Samples"))
            os.makedirs(os.path.join(card, "Plugins"))
            defaults = {}
            for n, p in STARTER_PKG.items():
                shutil.copy(p, os.path.join(card, "Plugins", f"{n}.rmp"))
                with open(os.path.join(REPO, "plugins", n, "manifest.json")) as f:
                    m = json.load(f)
                d = [64] * 16
                for k in m["knobs"]:
                    d[k["p"] - 1] = k.get("default", 64)
                defaults[m["id"]] = d
            r = scenario(card, f"""
out = {{}}
for id, d in {defaults!r}.items():
    e.um_command("um=assign:1:" + id); e.idle()
    e.report(P.trig_params(0x50, 48, 100, [v << 7 for v in d])); e.idle()
    a = e.frames(24000)[:, 0].astype(float) / 32767
    out[id] = [float(np.abs(a).max()), float(np.sqrt(np.mean(a * a))), bool(np.isfinite(a).all())]
out["json"] = [x for x in e.um_json() if x["t"] == "plugin"]
print(json.dumps(out))
""")
            for id in defaults:
                peak, rms, fin = r[id]
                self.assertTrue(fin, id)
                self.assertGreater(peak, 0.05, id)
                self.assertLess(peak, 1.0, id)
                self.assertGreater(rms, 0.002, id)
            self.assertTrue(all(x["fit"] for x in r["json"]))
            self.assertEqual(len(r["json"]), 6)
        finally:
            shutil.rmtree(card, True)


if __name__ == "__main__":
    unittest.main()
