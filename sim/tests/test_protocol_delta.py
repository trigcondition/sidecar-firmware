"""56 PARAMS DELTA and the host's caps in its 1D (params-delta): the protocol module's builders and decoders, and the
engine core (core/build/libcore.dylib) playing a stream of 56s exactly as the 03s they stand for.

    make -C core engines lib && uv run python -m unittest sim/tests/test_protocol_delta.py

The core keeps module state, so each stream runs in its own process (this file run as a script: stream NAME OUT).
The combinations: a new host and a new Rytm (56s: the same audio as 03s); a new host and an older Rytm (03s only:
the core's 1Ds carry caps the Rytm reads as an answer naming no slot); an older host and a new Rytm (the Rytm sends
56 only after a 1D with HOST_CAPS_DELTA, so an older host's 13 / 15-byte 1Ds keep it on 03s: rytm-mods 0021).
"""
import os
import random
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402
from core_golden import juno_p, plaits_p, project  # noqa: E402

LIB = os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib")


def v14(ps):
    return [int(p) << 7 for p in ps]


def stream(seed=7):
    """Two voices (PLAITS on 0, JUNO on 1): a trig each, then 60 rounds of one or a few values moving (an LFO on one
    knob, a knob turned, now and then several), as (whole 03 / 04 message, voice, values after it) - the Rytm's view."""
    rnd = random.Random(seed)
    last = {0: v14(plaits_p(40)), 1: v14(juno_p(5) + [64] * 8)}
    vbs = {0: P.PLAITS | 0, 1: P.JUNO | 1}
    out = [(P.trig_params(vbs[vi], 48, 100, last[vi], 0), vi, list(last[vi])) for vi in (0, 1)]
    for k in range(60):
        vi = k & 1
        ps = list(last[vi])
        idx = [2] if k % 7 else rnd.sample(range(1, 16), rnd.randint(2, 5))   # (not P1: PLAITS' ENG, JUNO's PAT)
        for i in idx:
            ps[i] = max(0, min(127 << 7 | 127, ps[i] + rnd.randint(-300, 300)))
        last[vi] = ps
        out.append((P.params_report(vbs[vi], ps, 0), vi, list(ps)))
    return out


def as_deltas(msgs):
    """The same stream as a new Rytm sends it to a host that reads 56: trigs whole, the first parameter-only report
    of each voice whole, then the changed values only."""
    seen, res = {}, []
    for m, vi, ps in msgs:
        if m[4] == P.PARAMS and vi in seen:
            changed = {i: ps[i] for i in range(16) if ps[i] != seen[vi][i]}
            vb = m[5]
            res.append(P.params_delta(vb, changed, 0) if 0 < len(changed) < 14 else m)
        else:
            res.append(m)
        seen[vi] = ps
    return res


def run_stream(name, out_path):
    from coreengine import CoreEngine
    card = tempfile.mkdtemp(prefix="core-delta-")
    os.makedirs(os.path.join(card, "Samples"))
    eng = CoreEngine(card)
    eng.report(project("DELTA")[1])   # (a project: the core's 1D)
    msgs = [m for m, _, _ in stream()]
    if name == "delta":
        msgs = as_deltas(stream())
    frames = []
    for m in msgs:
        eng.report(m)
        frames.append(eng.frames(96))
    np.save(out_path, np.concatenate(frames))
    sx = [m for m in eng.take_sysex() if m[4] == P.SLOTS]
    with open(out_path + ".1d", "w") as f:
        f.write("\n".join(x.hex() for x in sx))


class TestProtocol(unittest.TestCase):
    def test_roundtrip(self):
        m = P.params_delta(P.FM | 3, {0: 5 << 7, 15: 127 << 7 | 127}, 0x1234)
        self.assertEqual(len(m), 13 + 2 * 2)
        cmd, body = P.parse(m)
        self.assertEqual(cmd, P.PARAMS_DELTA)
        d = P.decode_params_delta(body)
        self.assertEqual(d, dict(voice=3, machine=P.FM, vb=P.FM | 3, changed={0: 5 << 7, 15: 127 << 7 | 127}, tag=0x1234))
        self.assertEqual(body[2:5], bytes([1, 0, 2]))   # mask: P1 in m0, P16 in m2 bit 1

    def test_malformed(self):
        body = bytearray(P.parse(P.params_delta(0, {3: 100}))[1])
        self.assertIsNone(P.decode_params_delta(bytes(body[:-1])))           # one byte short
        self.assertIsNone(P.decode_params_delta(bytes([2]) + bytes(body[1:])))   # another version
        b = bytearray(body); b[4] = 4                                         # a mask bit past P16
        self.assertIsNone(P.decode_params_delta(bytes(b)))
        self.assertIsNone(P.decode_params_delta(bytes([1, 0, 0, 0, 0, 0, 0])))   # no value

    def test_apply_equals_03(self):
        last = v14(plaits_p(40))
        new = list(last)
        new[2], new[13] = 77 << 7 | 5, 3 << 7
        d = P.params_delta(P.PLAITS | 2, {2: new[2], 13: new[13]}, 0x21)
        self.assertEqual(P.apply_params_delta(last, P.parse(d)[1]), P.params_report(P.PLAITS | 2, new, 0x21))
        self.assertEqual(len(P.params_report(0, new, 0)), 41)

    def test_slots_caps(self):
        self.assertEqual(len(P.slots([1] * 5, 1)), 13)
        self.assertEqual(len(P.slots([1] * 5, 1, caps=P.HOST_CAPS_DELTA)), 16)
        self.assertEqual(P.decode_slots(P.parse(P.slots([1] * 5, 1, caps=3))[1]),
                         dict(gens=[1] * 5, installed=1, answer=None, caps=3))
        self.assertEqual(P.decode_slots(P.parse(P.slots([1] * 5, 1, 2, P.RES_NO_ROOM, caps=1))[1])["answer"],
                         (2, P.RES_NO_ROOM))
        # an older Rytm (0021 before params-delta) reads <slot> at byte 12: 0 names no slot, no answer taken
        m = P.slots([1] * 5, 1, caps=1)
        self.assertEqual(m[12], 0)

    def test_deltas_are_smaller(self):
        s = stream()
        full = sum(len(m) for m, _, _ in s)
        d = sum(len(m) for m in as_deltas(s))
        self.assertLess(d, full * 0.6)


@unittest.skipUnless(os.path.exists(LIB), "core/build/libcore.dylib: make -C core engines lib")
class TestCore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="delta-")
        cls.out = {}
        for name in ("full", "delta"):
            p = os.path.join(cls.tmp, name + ".npy")
            subprocess.run([sys.executable, os.path.abspath(__file__), "stream", name, p], check=True)
            with open(p + ".1d") as f:
                cls.out[name] = (np.load(p), f.read().split())

    def test_delta_plays_as_03(self):
        full, delta = self.out["full"][0], self.out["delta"][0]
        self.assertGreater(np.abs(full).max(), 1000)
        self.assertTrue(np.array_equal(full, delta))   # every voice, every frame: the same

    def test_host_says_caps(self):
        sx = [bytes.fromhex(x) for x in self.out["full"][1]]
        self.assertTrue(sx)
        for m in sx:   # every 1D the core sent: 16 bytes, its caps
            d = P.decode_slots(P.parse(m)[1])
            self.assertTrue(d["caps"] & P.HOST_CAPS_DELTA)
        self.assertTrue(P.decode_slots(P.parse(sx[0])[1])["caps"] & P.HOST_CAPS_RESYNC)   # the first: a resync


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "stream":
        run_stream(sys.argv[2], sys.argv[3])
    else:
        unittest.main()
