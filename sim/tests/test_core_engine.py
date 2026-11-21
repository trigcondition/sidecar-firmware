"""The engine core on the Mac (core/build/libcore.dylib through sim/coreengine.py): PLAITS against the simulator's
Plaits (sim/plaitsvoice.py) for the same report, the empty user slots' answers, a project's names, a listing.

    make -C core engines lib && sim/plaits/build.sh && uv run python -m unittest sim/tests/test_core_engine.py
"""
import os
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402
from core_golden import plaits_p, project, trig  # noqa: E402
from coreengine import CoreEngine  # noqa: E402
from plaitsvoice import Voice  # noqa: E402

PLAITS_MODELS = (0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20)


def snr(ref, got):
    ref, got = ref.astype(np.float64), got.astype(np.float64)
    e = np.sqrt(np.mean((ref - got) ** 2))
    return np.inf if e == 0 else 20 * np.log10(np.sqrt(np.mean(ref ** 2)) / e)


class TestCoreEngine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.card = tempfile.mkdtemp(prefix="core-py-")
        os.makedirs(os.path.join(cls.card, "Samples"))
        cls.eng = CoreEngine(cls.card)   # one engine per process: the tests share it, voice by voice

    def test_plaits_matches_the_simulator(self):
        eng_value, note, vel = 40, 48, 100            # zone 5: Plaits model 8
        p = plaits_p(eng_value)
        self.eng.report(trig(0x00 | 0, note, vel, p)[1])
        got = self.eng.frames(9600)[:, 0].astype(np.float64)
        v = Voice()
        v.engine = PLAITS_MODELS[(eng_value * 17) >> 7]
        v.harmonics, v.timbre, v.morph = p[1] / 127, p[2] / 127, p[3] / 127
        v.decay, v.lpg_colour = p[4] / 127, p[5] / 127
        v.fm_amount, v.timbre_amount, v.morph_amount, v.aux_mix = (p[8] - 64) / 64, p[11] / 127, p[12] / 127, p[10] / 127
        sens = p[13] / 127
        v.trigger(note + round(p[6] - 64) + (p[9] - 64) / 64, (1 - sens + sens * vel / 127) * p[7] / 127)
        ref = np.concatenate([v.render(12) for _ in range(800)]) * 32768.0 * (p[15] / 100 * 0.7)
        ref = np.trunc(np.clip(ref, -32768, 32767))
        self.assertGreater(np.abs(got).max(), 1000)
        self.assertGreaterEqual(snr(ref, got), 60.0)

    def test_empty_user_slots(self):
        self.eng.take_sysex()
        self.eng.report(P.ask(2, 0, 0))
        self.eng.report(P.assign(3, 0, 0, 1))
        a, b = [m for m in self.eng.take_sysex() if m[4] != P.FXIN]   # (the first message since boot asks the IN L/R source)
        self.assertEqual(P.decode_meta(P.parse(a)[1])["state"], 0)
        d = P.decode_slots(P.parse(b)[1])
        self.assertTrue(d.pop("caps") & P.HOST_CAPS_DELTA)   # (params-delta: the host's caps in every 1D)
        self.assertEqual(d, dict(gens=[1] * 5, installed=1, answer=(3, 1)))

    def test_project_names_and_listing(self):
        self.eng.take_sysex()
        self.eng.report(project("PYTEST")[1])
        cmds = [P.parse(m)[0] for m in self.eng.take_sysex()]
        # the 1D a project owes when no plugins are linked (core/slots.cpp's slots_1d), then the lists
        self.assertEqual(cmds[:4], [P.SLOTS, P.POOL_TABLE, P.JUNO_NAMES, P.FM_NAMES])
        self.assertEqual(cmds.count(P.FM_NAMES), 12)   # Plaits' 96 ROM voices, 8 a message
        self.eng.report(bytes(P.HDR) + bytes([0x0D, 0, 0, 0xF7]))
        (m,) = self.eng.take_sysex()
        self.assertEqual(m[4], 0x0E)


if __name__ == "__main__":
    unittest.main()
