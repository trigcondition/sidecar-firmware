"""The mdkit machines (plugins/mdkit: MD - the three in one -, MD-PI, MD-EFM, MD-TRX) on the card host (core/user/um.cpp, the Pi's and
the Mac core lib's), the real RMPA packages: each scans as fitting (state, code, manifest with its TYPE labels),
assigns to a user slot, every TYPE sounds on a trig through the Sidecar's voice, the slot's 1A names P1..P8 after the
TYPE the voice plays (inert Ps unnamed), P9 TYPE and P10 LEV keep theirs. MD: P9 ENGINE picks the family, P10 TYPE
its voice - the slot's 1A gives P10 that ENGINE's TYPE count and its 1B that ENGINE's names (the RLB2 block) -, every
one of the 30 voices sounds, P1..P8 are named after it, P11 LEV. plugins/mdkit/tests/mdk_check.cpp checks
the voices themselves (bit-exact against mdkit, the resampler, TYPE changes).

    make -C core engines lib && make -C plugins/mdkit packages
    cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_core_mdkit
"""
import json
import os
import shutil
import tempfile
import unittest

from test_core_um import scenario

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
MDK = os.path.join(REPO, "plugins", "mdkit")
PKGS = {f: os.path.join(MDK, "out", f"mdk_{f}.rmp") for f in ("pi", "efm", "trx")}
PKGS = {f: p for f, p in PKGS.items() if os.path.exists(p)}   # the families built (make packages FAMS=...)
HAVE = bool(PKGS)
NAMES = {"pi": "MD-PI", "efm": "MD-EFM", "trx": "MD-TRX"}


@unittest.skipUnless(HAVE, "make -C plugins/mdkit packages; make -C core engines lib")
class TestCoreMdkit(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="mdk-")
        os.makedirs(os.path.join(self.card, "Samples"))
        os.makedirs(os.path.join(self.card, "Plugins"))
        for f, p in PKGS.items():
            shutil.copy(p, os.path.join(self.card, "Plugins", f"mdk_{f}.rmp"))
        with open(os.path.join(MDK, "voices.json")) as fh:
            self.voices = json.load(fh)
        self.man = {}
        for f in PKGS:
            with open(os.path.join(MDK, f"{f}.json")) as fh:
                self.man[f] = json.load(fh)

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def test_families_load_play_and_label(self):
        types = {f: self.man[f]["knobs"][8]["names"] for f in PKGS}
        r = scenario(self.card, """
import time
out = {"play": {}, "labels": {}}
gen, names = inst()
out["names"] = names
tx(P.HDR + bytes([0x10]) + b"MDKIT" + b"\\xf7")
FAMS = %r
for s, f in enumerate(FAMS, 1):
    tx(P.assign(s, P.OP_ASSIGN, names.index(NAMES[f]), gen))
for _ in range(50): e.idle()
out["json"] = [x for x in e.um_json() if x["t"] in ("plugin", "slot")]
TYPES = %r
for s, f in enumerate(FAMS, 1):
    vb = 0x50 + 8 * (s - 1) + s   # U<s>, voice s
    n = len(TYPES[f])
    out["play"][f], out["labels"][f] = [], []
    for t in range(n):
        ps = [64 << 7] * 16
        ps[8] = ((t * 128 + 64) // n) << 7
        ps[9] = 100 << 7
        e.report(P.params_report(vb, ps)); e.idle()
        e.report(P.trig_params(vb, 60, 100, ps)); e.idle()
        a = e.frames(24000)[:, vb & 7].astype(float)
        out["play"][f].append(float(abs(a).max()))
        time.sleep(0.16); e.idle(); time.sleep(0.11); e.idle(); e.take_sysex()   # (label changes: 150 ms apart at most)
        m = meta(s)
        out["labels"][f].append([(k["p"], k["long"], k["short"]) for x in m[1:] for k in x["knobs"] if k["p"] <= 10])
print(json.dumps(out))
""".replace("NAMES[f]", "%r[f]" % NAMES) % (list(PKGS), types))
        self.assertTrue({NAMES[f] for f in PKGS} <= set(r["names"]))
        plugins = {x["id"]: x for x in r["json"] if x["t"] == "plugin"}
        for f in PKGS:
            p = plugins[f"mdkit.{f}"]
            self.assertEqual((p["fit"], p["status"], p["target"]), (True, "ok", "a64"), p)
            self.assertLessEqual(p["state"], 65536)
        slots = [x for x in r["json"] if x["t"] == "slot"][:len(PKGS)]
        self.assertEqual([s["state"] for s in slots], ["READY"] * len(PKGS))
        for f in PKGS:
            for t, (pk, lab) in enumerate(zip(r["play"][f], r["labels"][f])):
                v = self.voices[f"{f}_{types[f][t].lower()}"]
                self.assertGreater(pk, 30, f"{f} {types[f][t]}: silent")   # int16 frames
                want = [[k + 1, self.man[f]["type_labels"]["sets"][t][k][0], c[0]] if k < len(v["controls"]) else [k + 1, "", ""]
                        for k, c in enumerate(v["controls"] + [None] * (8 - len(v["controls"])))]
                self.assertEqual(lab[:8], want, f"{f} {types[f][t]}")
                self.assertEqual(lab[8:], [[9, "Type", "TYPE"], [10, "Level", "LEV"]])


MD = os.path.join(MDK, "out", "mdk.rmp")


@unittest.skipUnless(os.path.exists(MD), "make -C plugins/mdkit packages; make -C core engines lib")
class TestCoreMd(unittest.TestCase):
    def setUp(self):
        self.card = tempfile.mkdtemp(prefix="mdone-")
        os.makedirs(os.path.join(self.card, "Samples"))
        os.makedirs(os.path.join(self.card, "Plugins"))
        shutil.copy(MD, os.path.join(self.card, "Plugins", "mdk.rmp"))
        with open(os.path.join(MDK, "voices.json")) as fh:
            self.voices = json.load(fh)
        with open(os.path.join(MDK, "md.json")) as fh:
            self.man = json.load(fh)

    def tearDown(self):
        shutil.rmtree(self.card, True)

    def test_engine_type_play_and_label(self):
        lists = self.man["type_labels"]["lists"]
        r = scenario(self.card, """
import time
out = {"play": [], "labels": [], "names": []}
gen, names = inst()
out["inst"] = names
tx(P.HDR + bytes([0x10]) + b"MDONE" + b"\\xf7")
tx(P.assign(1, P.OP_ASSIGN, names.index("MD"), gen))
for _ in range(50): e.idle()
out["json"] = [x for x in e.um_json() if x["t"] in ("plugin", "slot")]
LISTS = %r
vb = 0x51   # U1, voice 1
for en, ln in enumerate(LISTS):
    for t in range(len(ln)):
        ps = [64 << 7] * 16
        ps[8] = ((en * 128 + 64) // len(LISTS)) << 7
        ps[9] = ((t * 128 + 64) // len(ln)) << 7
        ps[10] = 100 << 7
        e.report(P.params_report(vb, ps)); e.idle()
        time.sleep(0.01); e.idle()   # (an ENGINE change: the 2 ms fade, then the trig sets the new voice up)
        e.report(P.trig_params(vb, 60, 100, ps)); e.idle()
        a = e.frames(24000)[:, vb & 7].astype(float)
        out["play"].append(float(abs(a).max()))
        time.sleep(0.16); e.idle(); time.sleep(0.11); e.idle(); e.take_sysex()
        m = meta(1)
        out["labels"].append([(k["p"], k["long"], k["short"], k["enum_count"]) for x in m[1:] for k in x["knobs"] if k["p"] <= 11])
        ls = [P.decode_list(x[5:-1]) for x in of(tx(P.ask(1, 0, P.ASK_ENUM, 9)), P.LIST)]
        out["names"].append([n for l in ls for _, n in l["entries"]])
print(json.dumps(out))
""" % (lists,))
        self.assertIn("MD", r["inst"])
        p = [x for x in r["json"] if x["t"] == "plugin" and x["id"] == "mdkit.md"][0]
        self.assertEqual((p["fit"], p["status"], p["target"]), (True, "ok", "a64"), p)
        self.assertLessEqual(p["state"], 65536)
        self.assertEqual([x for x in r["json"] if x["t"] == "slot"][0]["state"], "READY")
        i = 0
        for en, (fam, ln) in enumerate(zip(("efm", "pi", "trx"), lists)):
            for t, name in enumerate(ln):
                v = self.voices[f"{fam}_{name.lower()}"]
                pk, lab, nm = r["play"][i], r["labels"][i], r["names"][i]
                self.assertGreater(pk, 30, f"{fam} {name}: silent")
                want = [[k + 1, self.man["type_labels"]["sets"][i][k][0], c[0], 0] if k < len(v["controls"]) else [k + 1, "", "", 0]
                        for k, c in enumerate(v["controls"] + [None] * (8 - len(v["controls"])))]
                self.assertEqual(lab[:8], want, f"{fam} {name}")
                self.assertEqual(lab[8:], [[9, "Engine", "ENG", 3], [10, "Type", "TYPE", len(ln)], [11, "Level", "LEV", 0]],
                                 f"{fam} {name}")
                self.assertEqual(nm, ln, f"{fam} {name}")
                i += 1
        self.assertEqual(i, 30)


if __name__ == "__main__":
    unittest.main()
