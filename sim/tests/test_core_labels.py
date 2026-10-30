"""Labels that follow a list knob (rmp_format.h RMP_LABELS_*, the manifest's "type_labels"; plugins/mdkit's TYPE) on
the card hosts (core/user/um.cpp, the Pi's and the Mac core lib's): the slot's 1A names its knobs as the value its
voice's list knob has, a change moves the slot's generation (a 1D, so the Rytm asks 1A again and its user-machine page
and the touchscreen follow), the voice whose knobs moved last decides, an older package (no block) keeps its labels.

    make -C core engines lib && make -C plugins/sine_test a64
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_labels
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import rmp_pack  # noqa: E402
from test_core_um import scenario  # noqa: E402

SINE = os.path.join(REPO, "plugins", "sine_test", "sine_test.a64.rmp")


def relabel(rmp, man):
    """The package rmp as man says (the image and relocations unchanged; the id, manifest_size and the CRC new)."""
    h = rmp_pack.parse_header(rmp)
    start = 96 + h["image_size"] + 8 * h["reloc_count"]
    mb, _, _ = rmp_pack._manifest(man)
    body = rmp[96:start] + mb
    body += b"\0" * (-len(body) % 4)
    f = list(struct.unpack(rmp_pack.HEADER_FMT, rmp[:96]))
    f[3] = man["id"].encode(); f[10] = len(mb); f[12] = zlib.crc32(body) & 0xFFFFFFFF
    return struct.pack(rmp_pack.HEADER_FMT, *f) + body


MAN = {"id": "rytm.lbl_test", "version": "1.0.0", "name": "LBL TEST", "short": "LBLT", "licence": "MIT",
       "page2": "params",
       "knobs": [{"p": 1, "long": "Tune", "short": "TUN", "look": "centred", "text": "bipolar"},
                 {"p": 2, "long": "Level", "short": "LEV", "look": "fader"},
                 {"p": 3, "long": "Decay", "short": "DEC"},
                 {"p": 9, "long": "Type", "short": "TYPE", "look": "list", "text": "enum", "names": ["AA", "BB", "CC"]}],
       "type_labels": {"knob": 9, "params": [1, 3],
                       "sets": [[["Pitch", "PTCH"], ["Decay", "DEC"]],
                                [["Snappy", "SNAP"], ["Tone", "TONE"]],
                                [None, ["Grains", "GRNS"]]]}}


@unittest.skipUnless(os.path.exists(SINE), "make -C plugins/sine_test a64; make -C core engines lib")
class TestLabels(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="lbl-")
        os.makedirs(os.path.join(self.card, "Samples"))
        os.makedirs(os.path.join(self.card, "Plugins"))
        with open(SINE, "rb") as f:
            sine = f.read()
        with open(os.path.join(self.card, "Plugins", "lbl.rmp"), "wb") as f:
            f.write(relabel(sine, MAN))
        old = dict(MAN, id="rytm.old_test", name="OLD TEST")
        del old["type_labels"]
        with open(os.path.join(self.card, "Plugins", "old.rmp"), "wb") as f:
            f.write(relabel(sine, old))

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def test_labels_follow_the_list_knob(self):
        r = scenario(self.card, """
import time
out = {}
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"LBLTEST" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("LBL TEST"), gen))
tx(P.assign(2, P.OP_ASSIGN, names.index("OLD TEST"), gen))
for _ in range(20): e.idle()
def knobs(s):
    m = meta(s)
    return m[0]["gen"], [(k["p"], k["long"], k["short"]) for x in m[1:] for k in x["knobs"] if k["p"] in (1, 2, 3, 9)]   # (the rest: padding)
def report(vb, p9, trig=False):
    ps = [64 << 7] * 16
    ps[8] = p9 << 7
    m = tx(P.trig_params(vb, 60, 100, ps) if trig else P.params_report(vb, ps))
    for _ in range(3): time.sleep(0.11); e.idle(); m += e.take_sysex()   # meta_poll at most every 100 ms
    return [P.decode_slots(x[5:-1]) for x in of(m, P.SLOTS)]
out["static"] = knobs(1)
out["d_a"] = report(0x50, 0)
out["a"] = knobs(1)
out["d_c"] = report(0x50, 127)
out["c"] = knobs(1)
out["d_same"] = report(0x50, 120)
out["same"] = knobs(1)
report(0x51, 64)                  # another voice on U1, its knobs moved last: its value
out["b"] = knobs(1)
report(0x50, 0, trig=True)        # a trig on voice 0: not a knob move, voice 1 still decides
out["b_trig"] = knobs(1)
report(0x5A, 127)                 # U2's voice 2
out["old"] = knobs(2)             # a package without the block: its own labels, whatever P9 says
out["json"] = [x.get("lset") for x in e.um_json() if x["t"] == "slot"]
print(json.dumps(out))
""")
        g0, st = r["static"]
        self.assertEqual(st, [[1, "Tune", "TUN"], [2, "Level", "LEV"], [3, "Decay", "DEC"], [9, "Type", "TYPE"]])
        ga, a = r["a"]
        self.assertEqual(a, [[1, "Pitch", "PTCH"], [2, "Level", "LEV"], [3, "Decay", "DEC"], [9, "Type", "TYPE"]])
        self.assertNotEqual(ga, g0)
        self.assertTrue(any(d["gens"][0] == ga for d in r["d_a"]))   # the 1D carried the new generation
        gc, c = r["c"]
        self.assertEqual(c, [[1, "", ""], [2, "Level", "LEV"], [3, "Grains", "GRNS"], [9, "Type", "TYPE"]])   # P1 inert
        self.assertNotEqual(gc, ga)
        self.assertEqual(r["same"], r["c"])                   # 120 is CC too: nothing moves, no new generation
        self.assertEqual(r["b"][1][0], [1, "Snappy", "SNAP"])
        self.assertEqual(r["b_trig"], r["b"])
        self.assertEqual(r["old"][1], st)
        self.assertEqual(r["json"][:2], [1, None])

    def test_no_flood_lfos_rate_limit_selection(self):
        """Review I1: a Sidecar LFO (voice_params_changed every 5 ms) does not decide whose TYPE the labels show, label
        changes move the generation at most every 150 ms (no 1D flood when two voices' knobs alternate), and the
        Rytm's selected track wins."""
        r = scenario(self.card, """
import time, ctypes
out = {}
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"LBLTEST" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("LBL TEST"), gen))
for _ in range(20): e.idle()
def ps(p9):
    p = [64 << 7] * 16
    p[8] = p9 << 7
    return p
def lset():
    return [x.get("lset") for x in e.um_json() if x["t"] == "slot"][0]
def ones(m): return len(of(m, P.SLOTS))
e.report(P.params_report(0x50, ps(0))); e.idle()
e.report(P.params_report(0x51, ps(127))); e.idle()   # voice 1's knobs moved last: CC
time.sleep(0.2); e.idle(); e.take_sysex()
out["start"] = lset()
L = e.lib
L.core_c_lfo_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
L.core_c_lfo_tick.argtypes = [ctypes.c_uint32]
b = ctypes.create_string_buffer(256)
for c in ("lfo=1:tgt:V1", "lfo=1:par:1", "lfo=1:dep:63", "lfo=1:rate:120"):   # voice 0's P2 (its TYPE is AA)
    L.core_c_lfo_command(c.encode(), b, 256)
n1d, sets = 0, set()
t0 = time.time()
while time.time() - t0 < 1.0:
    L.core_c_lfo_tick(int(time.time() * 1e6) & 0xFFFFFFFF)
    e.idle(); n1d += ones(e.take_sysex()); sets.add(lset())
    time.sleep(0.002)
st = (ctypes.c_uint32 * 8)()
L.core_c_lfo_stats(st)
out["lfo"] = [n1d, sorted(x for x in sets if x is not None), st[4]]
for c in ("lfo=1:off",):
    L.core_c_lfo_command(c.encode(), b, 256)
n1d, t0, k = 0, time.time(), 0
while time.time() - t0 < 1.0:   # real knob moves on the two voices, alternating every 10 ms
    k += 1
    e.report(P.params_report(0x50 if k & 1 else 0x51, ps(0 if k & 1 else 127)))
    e.idle(); n1d += ones(e.take_sysex())
    time.sleep(0.01)
out["alternate"] = n1d
e.report(P.HDR + bytes([0x49, 1, 2, 0, 44]) + b"BD LBL TEST " + b"\\xf7")   # the Rytm selects track 1 (voice 0)
e.report(P.params_report(0x51, ps(127)))   # voice 1's knobs move: the selected track still decides
for _ in range(4): time.sleep(0.1); e.idle()
out["selected"] = lset()
print(json.dumps(out))
""")
        self.assertEqual(r["start"], 2)
        n, sets, calls = r["lfo"]
        self.assertGreater(calls, 100)       # the LFOs did call the plugin's params (voice_params_changed)
        self.assertEqual(sets, [2])          # the LFOs never moved the labels
        self.assertLessEqual(n, 2)           # (a 5 s heartbeat 1D at most)
        self.assertLessEqual(r["alternate"], 9)   # at most one label change per 150 ms (+ a heartbeat)
        self.assertEqual(r["selected"], 0)

    def test_list_follows_another(self):
        """2026-10-27 (plugins/mdkit's MD, an RLB2 block): P10 TYPE's names follow P9 ENGINE. The slot's 1A gives
        TYPE the enum count of the ENGINE the voice has (the zones the Rytm shows), its 1B names are that ENGINE's,
        P1/P3 carry the label set of (ENGINE, TYPE); an ENGINE change moves the generation. The old-style package on
        U2 (RLB1) and the static description before any report keep working."""
        man = json.loads(json.dumps(MAN))
        man["id"], man["name"] = "rytm.dep_test", "DEP TEST"
        man["knobs"][3] = {"p": 9, "long": "Engine", "short": "ENG", "look": "list", "text": "enum", "names": ["FM", "PI", "TRX"]}
        man["knobs"].append({"p": 10, "long": "Type", "short": "TYPE", "look": "list", "text": "enum", "names": ["BD", "SD"]})
        lists = [["BD", "SD"], ["BD", "MT", "HH"], ["BD", "SD", "CP", "OH"]]
        sets = [[[f"{e}{n}", n[:4]], None if n == "HH" else ["Decay", "DEC"]] for e, ln in zip(("F", "P", "T"), lists) for n in ln]
        man["type_labels"] = {"knob": 10, "follows": 9, "lists": lists, "params": [1, 3], "sets": sets}
        with open(SINE, "rb") as f:
            sine = f.read()
        with open(os.path.join(self.card, "Plugins", "dep.rmp"), "wb") as f:
            f.write(relabel(sine, man))
        r = scenario(self.card, """
import time
out = {}
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"DEPTEST" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("DEP TEST"), gen))
tx(P.assign(2, P.OP_ASSIGN, names.index("LBL TEST"), gen))
for _ in range(20): e.idle()
def knobs(s):
    m = meta(s)
    return m[0]["gen"], [(k["p"], k["long"], k["short"], k["enum_count"]) for x in m[1:] for k in x["knobs"] if k["p"] in (1, 3, 9, 10)]
def enum(s, knob):
    ls = [P.decode_list(x[5:-1]) for x in of(tx(P.ask(s, 0, P.ASK_ENUM, knob)), P.LIST)]
    return [n for l in ls for _, n in l["entries"]]
def report(vb, p9, p10):
    ps = [64 << 7] * 16
    ps[8], ps[9] = p9 << 7, p10 << 7
    tx(P.params_report(vb, ps))
    for _ in range(3): time.sleep(0.11); e.idle(); e.take_sysex()
out["static"] = knobs(1), enum(1, 9)
for name, p9, p10 in (("fm_sd", 0, 127), ("pi_bd", 64, 0), ("pi_hh", 64, 127), ("trx_cp", 127, 80), ("trx_oh", 127, 127)):
    report(0x50, p9, p10)
    out[name] = knobs(1), enum(1, 9), enum(1, 8)
report(0x59, 0, 127)            # U2's voice 1: the RLB1 package as before (its P9 CC)
out["old"] = knobs(2)
out["json"] = [x.get("lset") for x in e.um_json() if x["t"] == "slot"]
print(json.dumps(out))
""")
        (g0, st), st_names = r["static"]
        self.assertEqual(st, [[1, "Tune", "TUN", 0], [3, "Decay", "DEC", 0], [9, "Engine", "ENG", 3], [10, "Type", "TYPE", 2]])
        self.assertEqual(st_names, ["BD", "SD"])   # the static names (lists[0]) before any voice reported
        gens = {g0}
        for name, want, count, nm in (("fm_sd", [1, "FSD", "SD", 0], 2, ["BD", "SD"]),
                                      ("pi_bd", [1, "PBD", "BD", 0], 3, ["BD", "MT", "HH"]),
                                      ("pi_hh", [1, "PHH", "HH", 0], 3, ["BD", "MT", "HH"]),
                                      ("trx_cp", [1, "TCP", "CP", 0], 4, ["BD", "SD", "CP", "OH"]),
                                      ("trx_oh", [1, "TOH", "OH", 0], 4, ["BD", "SD", "CP", "OH"])):
            (g, k), names10, names9 = r[name]
            self.assertEqual(k[0], want, name)
            self.assertEqual(k[1], [3, "", "", 0] if name == "pi_hh" else [3, "Decay", "DEC", 0], name)   # HH: P3 inert
            self.assertEqual(k[2], [9, "Engine", "ENG", 3], name)
            self.assertEqual(k[3], [10, "Type", "TYPE", count], name)   # the zones the Rytm shows: the ENGINE's count
            self.assertEqual(names10, nm, name)
            self.assertEqual(names9, ["FM", "PI", "TRX"], name)         # ENGINE's names: the package's own table
            self.assertNotIn(g, gens, name)                             # every change a new generation
            gens.add(g)
        self.assertEqual(r["old"][1][0], [1, "Pitch", "PTCH", 0])      # RLB1 on U2 as before (its P9 0: AA, set 0)
        self.assertEqual(r["json"][:2], [8, 0])                         # TRX (base 5) + OH (3)

    def dep_man(self, lists, sets=None, version="1.0.0", params=(1, 3), engines=None):
        man = json.loads(json.dumps(MAN))
        man["id"], man["name"], man["version"] = "rytm.dep_test", "DEP TEST", version
        man["knobs"][3] = {"p": 9, "long": "Engine", "short": "ENG", "look": "list", "text": "enum",
                           "names": engines or ["FM", "PI", "TRX"][:len(lists)]}
        man["knobs"].append({"p": 10, "long": "Type", "short": "TYPE", "look": "list", "text": "enum", "names": lists[0]})
        if sets is None:
            sets = [[["Same", "SAME"]] * len(params) for ln in lists for _ in ln]
        man["type_labels"] = {"knob": 10, "follows": 9, "lists": lists, "params": list(params), "sets": sets}
        return man

    def put(self, name, man):
        with open(SINE, "rb") as f:
            sine = f.read()
        with open(os.path.join(self.card, "Plugins", name), "wb") as f:
            f.write(relabel(sine, man))

    def test_same_count_lists_move_the_generation(self):
        """Review finding 1: two ENGINEs whose TYPE lists have the same count and whose label sets are the same: an
        ENGINE change still moves the slot's generation (the set shown is in the print), so the Rytm asks 1A and 1B
        again and shows the new names."""
        self.put("dep.rmp", self.dep_man([["BD", "SD"], ["HH", "OH"]], engines=["FM", "PI"]))
        r = scenario(self.card, """
import time
out = {}
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"DEPTEST" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("DEP TEST"), gen))
for _ in range(20): e.idle()
def report(p9):
    ps = [64 << 7] * 16
    ps[8], ps[9] = p9 << 7, 0
    tx(P.params_report(0x50, ps))
    for _ in range(3): time.sleep(0.11); e.idle(); e.take_sysex()
def state():
    m = meta(1)
    ls = [P.decode_list(x[5:-1]) for x in of(tx(P.ask(1, 0, P.ASK_ENUM, 9)), P.LIST)]
    return m[0]["gen"], [n for l in ls for _, n in l["entries"]], [x.get("lset") for x in e.um_json() if x["t"] == "slot"][0]
report(0); out["fm"] = state()
report(127); out["pi"] = state()
report(0); out["fm2"] = state()
print(json.dumps(out))
""")
        (g1, n1, s1), (g2, n2, s2), (g3, n3, s3) = r["fm"], r["pi"], r["fm2"]
        self.assertEqual((n1, s1, n2, s2, n3, s3), (["BD", "SD"], 0, ["HH", "OH"], 2, ["BD", "SD"], 0))
        self.assertNotEqual(g2, g1)   # the old code: the same print (same count, same labels), the same generation
        self.assertNotEqual(g3, g2)

    def test_rescan_resets_the_set_shown(self):
        """Review finding 5: a rescan that replaces the slot's package drops the set shown at once (the new package's
        sets are other lists), so the slot's next 1A names the new package's set - not the old index into the new
        package's sets - even inside the 150 ms rate limit."""
        lists = [["BD", "SD"], ["BD", "MT", "HH"], ["BD", "SD", "CP", "OH"]]
        tag = lambda pre: [[[f"{pre}{e}{n}"[:12], n[:4]], ["Decay", "DEC"]] for e, ln in zip("FPT", lists2) for n in ln]
        lists2 = lists
        self.put("dep.rmp", self.dep_man(lists, tag("O")))
        lists2 = [["BD", "SD", "CP"], ["BD", "MT", "HH"], ["BD", "SD", "CP", "OH"]]
        with open(SINE, "rb") as f:
            new = relabel(f.read(), self.dep_man(lists2, tag("N"), version="1.0.1"))
        r = scenario(self.card, """
import time, os
out = {}
gen, names = inst()
tx(P.HDR + bytes([0x10]) + b"DEPTEST" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("DEP TEST"), gen))
for _ in range(20): e.idle()
ps = [64 << 7] * 16
ps[8], ps[9] = 64 << 7, 0                  # PI, BD: set 2 in the old package, 3 in the new
time.sleep(0.2); tx(P.params_report(0x50, ps))   # (the set moves now: the last move is > 150 ms old)
out["old"] = meta(1)[1]["knobs"][0]["long"]
with open(%r, "wb") as f: f.write(%r)
e.um_command("um=rescan")
for _ in range(40):
    e.idle()
    sl = [x for x in e.um_json() if x["t"] == "slot"][0]
    if sl.get("state") == "READY" and sl.get("ver", sl.get("version")) in ("1.0.1", 0x10001): break
out["slot"] = {k: v for k, v in sl.items() if k in ("state", "ver", "version", "lset")}
out["new"] = meta(1)[1]["knobs"][0]["long"]
print(json.dumps(out))
""" % (os.path.join(self.card, "Plugins", "dep.rmp"), new))
        self.assertEqual(r["old"], "OPBD")
        self.assertEqual(r["new"], "NPBD", r["slot"])   # the old code: "NFCP" (set 2 of the new package) for 150 ms


if __name__ == "__main__":
    unittest.main()
