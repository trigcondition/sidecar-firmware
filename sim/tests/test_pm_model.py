"""The param-modulation contract's Rytm model (sim/pm_model.py, rytm-param-mod 1, 2) on the bytes protocol.py builds:
the encodings, the arithmetic (offset, override + mix, the scene lock's precedence), the ramps, the kit check, CLEAR, the
timeout, the slots and the refusals, 07's answer. The host's own tests run the same arithmetic in C++
(core/tests/pm_fake.h, test_rytm_param).

    cd sim/tests && PYTHONPATH=.. python3 -m unittest test_pm_model
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import protocol as P  # noqa: E402
from pm_model import BLOCK_US, Rytm  # noqa: E402


class PmModel(unittest.TestCase):
    def setUp(self):
        self.r = Rytm(kit=4)

    def run_us(self, us):
        self.r.advance(self.r.us + us)

    def test_encoding(self):
        self.assertEqual(P.pm_offset(4, 7, [(0, 30, -64)]).hex(" "), "f0 7d 52 54 5a 01 04 04 07 01 00 1e 7f 40 f7")
        self.assertEqual(P.pm_override(4, 0, [(2, 31, 90 << 7, 127, True)]).hex(" "),
                         "f0 7d 52 54 5a 01 05 04 00 01 02 5f 5a 00 7f f7")
        self.assertEqual(P.pm_clear(4, 3, 0x1FFF).hex(" "), "f0 7d 52 54 5a 01 06 04 03 7f 3f f7")
        self.assertEqual(P.pm_status().hex(" "), "f0 7d 52 54 5a 01 07 f7")
        self.assertEqual(len(P.pm_offset(0, 0, [(0, 30, 1)] * 28)), 8 + 3 + 4 * 28)
        self.assertEqual(len(P.pm_override(0, 0, [(0, 30, 1, 1, 0)] * 22)), 8 + 3 + 5 * 22)
        self.assertLessEqual(len(P.pm_offset(0, 0, [(0, 30, 1)] * 28)), 123)

    def test_offset_and_ramp(self):
        r = self.r
        self.assertTrue(r.take(P.pm_offset(4, 0, [(0, 30, 10 * 64)])))
        self.assertEqual(r.eff(0, 30), 74)
        r.take(P.pm_offset(4, 10, [(0, 30, -10 * 64)]))
        self.run_us(5 * BLOCK_US + 1)
        self.assertAlmostEqual(r.eff(0, 30), 64, places=6)   # halfway
        self.run_us(5 * BLOCK_US + 1)
        self.assertAlmostEqual(r.eff(0, 30), 54, places=6)
        r.take(P.pm_offset(4, 0, [(0, 30, -100 * 64)]))
        self.assertEqual(r.eff(0, 30), 0)   # clamped

    def test_override_mix_scene(self):
        r = self.r
        r.take(P.pm_override(4, 0, [(1, 31, 100 << 7, 127, True)]))
        self.assertEqual(r.eff(1, 31), 100)
        r.take(P.pm_override(4, 0, [(1, 31, 100 << 7, 32, True)]))
        self.assertAlmostEqual(r.eff(1, 31), 64 + 32 / 127 * 36, places=6)
        r.take(P.pm_offset(4, 0, [(1, 31, 5 * 64)]))
        r.scene[1][31] = True   # O3: a Rytm scene lock wins over the override, the offset still adds
        self.assertEqual(r.eff(1, 31), 69)
        r.scene[1][31] = False
        r.take(P.pm_override(4, 0, [(1, 31, 100 << 7, 0, True)]))
        self.assertEqual(r.eff(1, 31), 69)

    def test_kit_clear_timeout(self):
        r = self.r
        r.take(P.pm_offset(5, 0, [(0, 30, 640)]))
        self.assertEqual((r.kit_drops, r.eff(0, 30)), (1, 64))
        r.take(P.pm_offset(4, 0, [(0, 30, 640), (12, 9, 640)]))
        r.take(P.pm_clear(4, 0, 1))
        self.assertEqual((r.eff(0, 30), r.eff(12, 9)), (64, 74))
        self.run_us(260000)   # no op for 250 ms: to 0 over 32 blocks
        self.assertEqual(r.timeouts, 1)
        self.run_us(33 * BLOCK_US)
        self.assertEqual((r.eff(12, 9), len(r.e)), (64, 0))

    def test_refusals_slots_status(self):
        r = self.r
        r.take(P.pm_offset(4, 0, [(0, 4, 1)]))           # w 4 (LFO DST): never
        r.take(P.pm_override(4, 0, [(0, 49, 1, 127, 0)]))  # VM2: offsets only
        r.take(P.pm_offset(4, 0, [(12, 29, 1)]))          # the FX track's empty word
        self.assertEqual([a[3:] for a in r.answers], [bytes([4, 1]), bytes([5, 1]), bytes([4, 1])])
        r.answers.clear()
        words = [w for w in list(range(1, 42)) + list(range(48, 57)) if w != 4]
        recs = [(t, w, 64) for t in range(12) for w in words][:129]
        for i in range(0, len(recs), 28):
            r.take(P.pm_offset(4, 0, recs[i:i + 28]))
        self.assertEqual(len(r.e), 128)
        self.assertEqual(r.answers[-1][3:], bytes([4, 2]))
        r.answers.clear()
        r.take(P.pm_status())
        st = P.decode_pm_status(r.answers[-1][3:])
        self.assertEqual(r.answers[-1][2], P.PM_STATUS)
        self.assertEqual((st["slots"], st["kit_drops"], st["ring_drops"]), (127, 0, 0))   # (128 used: the 7-bit field says 127)
        r.kit_drops, r.timeouts = 20000, 300   # the counters saturate
        r.take(P.pm_status())
        st = P.decode_pm_status(r.answers[-1][3:])
        self.assertEqual((st["kit_drops"], st["timeouts"]), (16383, 127))


if __name__ == "__main__":
    unittest.main()
