"""The core's send FX host link (core/fx/send_fx.cpp) against sim/protocol.py: 0x20 SENDFX in (sendfx(),
sendfx_tags(): what the Rytm's reporter sends), 0x21 / 0x22 out (decode_sendfx_names, decode_sendfx_state),
the console command.

    make -C core lib && uv run python -m unittest sim/tests/test_core_sendfx.py
"""
import ctypes
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402

LIB = ctypes.CDLL(os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib"))


def values():
    b = (ctypes.c_uint8 * 64)()
    n = LIB.core_c_sendfx_values(b)
    v = bytes(b[:n])
    return dict(send=[(v[2 * t], v[2 * t + 1]) for t in range(12)], knobs=[list(v[24:30]), list(v[30:36])],
                route=v[36], tags=[v[37] << 7 | v[38], v[39] << 7 | v[40]])


def take(m):
    return LIB.core_c_sendfx_take(m, len(m))


class TestCoreSendFx(unittest.TestCase):
    def test_tracks_and_fx_words(self):
        self.assertTrue(take(P.sendfx(4, [0x3342])))
        self.assertEqual(values()["send"][4], (0x33, 0x21))
        words = [0x0102, 0x0204, 0x0306, 0x0408, 0x050A, 0x060C]   # FX words 0, 29, 38, 39, 40, 41
        m = P.sendfx(P.FX_TRACK, words)
        self.assertTrue(take(m))
        self.assertEqual(values()["knobs"], P.decode_sendfx(m[5:-1])["knobs"])
        self.assertEqual(values()["knobs"][0][0], 2)   # FX 1 TYPE = word 29 hi
        self.assertEqual(values()["knobs"][1][0], 1)   # FX 2 TYPE = word 0 hi
        t = P.sendfx_tags(0x1A2B, 0x3FFF)
        self.assertTrue(take(t))
        self.assertEqual(values()["tags"], P.decode_sendfx(t[5:-1])["tags"])
        self.assertFalse(take(P.sendfx(0, [1, 2])))         # one word too many
        self.assertFalse(take(bytes(P.sendfx(0, [0x100]))[:-2] + b"\x01\xf7"[1:]))

    def test_names_and_state(self):
        b = (ctypes.c_uint8 * 256)()
        total = LIB.core_c_sendfx_names_total()
        n = LIB.core_c_sendfx_names(b, 1, total, 0, min(total, 8))
        m = bytes(b[:n])
        self.assertEqual(m[4], P.SENDFX_NAMES)
        rep, tot, entries = P.decode_sendfx_names(m[5:-1])
        self.assertEqual((rep, tot), (True, total))
        self.assertEqual(entries[:7], [(0, "OFF"), (1, "CHORUS"), (2, "COMB"), (5, "SPACE"), (6, "RINGS"),
                                       (8, "CLOUDSEED"), (15, "STUTTER")])
        self.assertEqual(m, P.sendfx_names(entries + [(0, "")] * (total - len(entries)))[0])   # the first message
        reply = ctypes.create_string_buffer(400)
        for cmd, i, state, name in ((b"sendfx=2:rings", 6, P.ST_READY, "RINGS"),
                                    (b"sendfx=2:cloudseed", 8, P.ST_READY, "CLOUDSEED"),
                                    (b"sendfx=2:s4 reso", 7, P.ST_MISSING, "S4 RESO")):   # still reserved
            self.assertTrue(LIB.core_c_sendfx_command(cmd, reply, 400))
            n = LIB.core_c_sendfx_state(b, 1)
            st = P.decode_sendfx_state(bytes(b[5:n - 1]))
            self.assertEqual(st, dict(fx=1, id=i, state=state, name=name))
            self.assertEqual(bytes(b[:n]), P.sendfx_state(1, i, state, name))
            self.assertEqual(P.SENDFX_TYPES[i], name)
        LIB.core_c_sendfx_command(b"sendfx=2:off", reply, 400)

    def test_labels(self):
        """0x24 SENDFX LABELS: the core's tables (knob_labels.h) are protocol.py's SENDFX_LABEL_SETS, for every TYPE
        this build plays and none other; every message fits 128 bytes and decodes back."""
        b = (ctypes.c_uint8 * 256)()
        for i in range(128):
            n = LIB.core_c_sendfx_labels(b, i)
            if i not in P.SENDFX_LABEL_SETS:
                self.assertEqual(n, 0, f"id {i}")
                continue
            m = bytes(b[:n])
            labels, zones = P.SENDFX_LABEL_SETS[i]
            self.assertEqual(m, P.sendfx_labels(i, labels, zones), f"id {i}")
            self.assertLessEqual(len(m), 128)
            self.assertEqual(P.decode_sendfx_labels(m[5:-1]), (i, list(labels), [tuple(z) for z in zones]))
        self.assertEqual(sorted(P.SENDFX_LABEL_SETS), sorted(set(P.SENDFX_BUILT)))

    def test_label_steps(self):
        """The step bit (0x24 zone byte bit 3): set on every zone of STUTTER ENG and LEN (pure selectors) and nowhere
        else (zones with a number inside stay continuous); a knob's zones agree; the stored values are the zones' lo,
        and each lo reads back as its own zone in the DSP's mapping; an older Sidecar's 5-tuple zones encode without
        it (the same bytes as before)."""
        b = (ctypes.c_uint8 * 256)()
        stepped = {}
        for i in P.SENDFX_LABEL_SETS:
            n = LIB.core_c_sendfx_labels(b, i)
            m = bytes(b[:n])
            for k in range(23, n - 1, 8):
                self.assertEqual(m[k] & ~0x0F, 0, f"id {i}: only knob, show and step bits")
                if m[k] & P.ZONE_STEP:
                    stepped.setdefault(i, set()).add(m[k] & 3)
            for knob in range(4):
                zs = [z for z in P.SENDFX_LABEL_SETS[i][1] if z[0] == knob]
                self.assertIn(len({z[5] for z in zs}), (0, 1), f"id {i} knob {knob}: all zones or none")
                if any(z[5] for z in zs):
                    self.assertFalse(any(z[3] for z in zs), f"id {i} knob {knob}: a stepped knob shows no number")
        self.assertEqual(stepped, {15: {0, 1}, 5: {2}})   # STUTTER ENG, LEN; SPACE MODE
        self.assertEqual(P.sendfx_steps(5, 2), [0, 16, 32, 48, 64, 80, 96, 112])
        self.assertIsNone(P.sendfx_steps(5, 0))   # SPACE TIME: continuous (the number, then the note values)
        self.assertEqual(P.sendfx_steps(15, 0), [0, 64])
        self.assertEqual(P.sendfx_steps(15, 1), [0, 19, 37, 55, 74, 92, 110])
        for k, (i, knob) in enumerate(((15, 2), (15, 3), (6, 1), (8, 0), (2, 1), (1, 0), (7, 0))):
            self.assertIsNone(P.sendfx_steps(i, knob), (i, knob))
        names = ["1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1BAR"]
        for z, lo in enumerate(P.sendfx_steps(15, 1)):
            self.assertEqual(lo * 7 // 128, z)          # the DSP (stutter_fx.h zone_half_ticks) reads lo as zone z
            self.assertEqual(P.sendfx_value_text(15, 1, lo), names[z])
        self.assertEqual([P.sendfx_value_text(15, 0, v) for v in P.sendfx_steps(15, 0)], ["OFF", "ON"])
        old = [z[:5] for z in P.SENDFX_LABEL_SETS[15][1]]   # an older Sidecar: no step bit, otherwise the same
        a, c = P.sendfx_labels(15, P.SENDFX_LABEL_SETS[15][0], old), P.sendfx_labels(15, *P.SENDFX_LABEL_SETS[15])
        zb = set(range(23, len(c) - 1, 8))   # the zone bytes (knob | show | step)
        self.assertEqual(bytes(x & ~P.ZONE_STEP if k in zb else x for k, x in enumerate(c)), a)
        self.assertTrue(all(z[5] == 0 for z in P.decode_sendfx_labels(a[5:-1])[2]))

    def test_label_texts(self):
        """The value texts the Rytm's 0029 makes from them (protocol.py sendfx_value_text, the same rule as sfu.c)."""
        t = P.sendfx_value_text
        self.assertEqual(t(6, 1, 0), "MODAL 0")
        self.assertEqual(t(6, 1, 63), "MONO 31")
        self.assertEqual(t(6, 1, 95), "STRNG31")
        self.assertEqual(t(6, 1, 127), "CHORD31")
        self.assertEqual(t(8, 0, 40), "HALL 8")
        self.assertEqual(t(8, 0, 127), "AMBNC31")
        self.assertEqual(t(15, 0, 63), "OFF")
        self.assertEqual(t(15, 0, 64), "ON")
        self.assertEqual([t(15, 1, v) for v in (0, 18, 19, 36, 37, 54, 55, 73, 74, 91, 92, 109, 110, 127)],
                         ["1/64", "1/64", "1/32", "1/32", "1/16", "1/16", "1/8", "1/8", "1/4", "1/4", "1/2", "1/2",
                          "1BAR", "1BAR"])
        for v in range(128):   # STUTTER P3's zones are the DSP's (stutter_fx.h: zone = v * 7 / 128)
            names = ["1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1BAR"]
            self.assertEqual(t(15, 1, v), names[v * 7 // 128], v)
        self.assertEqual([t(15, 2, v) for v in (0, 1, 63, 64, 127)], ["NONE", "DCY 1", "DCY 63", "PTCH 1", "PTCH 64"])
        self.assertEqual([t(2, 1, v) for v in (0, 63, 64, 127)], ["POS 0", "POS 63", "NEG 0", "NEG 63"])
        self.assertEqual(t(1, 0, 77), "77")     # no zones: the number
        # SPACE (5): TIME the number up to 63, then the shortest head's note value; MODE the heads (+S: the spring)
        self.assertEqual([t(5, 0, v) for v in (0, 63, 64, 79, 80, 96, 112, 127)],
                         ["0", "63", "1/16", "1/16", "1/8T", "1/8", "1/8D", "1/8D"])
        self.assertEqual([t(5, 2, v) for v in range(0, 128, 16)],
                         ["H1", "H2", "H3", "H1+2", "H2+3", "H123", "H23+S", "H123S"])
        self.assertEqual(t(5, 1, 100), "100")   # REPEAT: percent
        self.assertEqual(t(15, 3, 5), "5")
        self.assertIsNone(t(7, 0, 5))           # no labels for the type: the Rytm keeps P2..P5 generic
        self.assertEqual([P.sendfx_route_text(v) for v in (0, 63, 64, 127)], ["PRE", "PRE", "POST", "POST"])
        for i, (labels, zones) in P.SENDFX_LABEL_SETS.items():
            self.assertTrue(all(len(x) <= P.LABEL_CHARS for x in labels))
            self.assertTrue(all(len(z[4]) <= P.ZONE_CHARS for z in zones))
            self.assertLessEqual(len(zones), P.MAX_ZONES)
            for k in range(4):   # ascending lo per knob, every value a 7-character text at most
                los = [z[1] for z in zones if z[0] == k]
                self.assertEqual(los, sorted(set(los)))
                self.assertTrue(all(len(t(i, k, v)) <= 7 for v in range(128)))

    def test_process(self):
        """core_c_sendfx_process (sim/sendfx_link.py's live render): the values taken from 0x20 drive the DSP - COMB
        at P2 24 (110 Hz) rings at 48000 / 110 samples; TYPE off is silent."""
        import numpy as np
        fp = ctypes.POINTER(ctypes.c_float)
        LIB.core_c_sendfx_process.argtypes = [fp, ctypes.c_int, fp, ctypes.c_float]

        def run(n):
            st = np.zeros((n, 8), np.float32)
            st[0, 0] = 0.5
            out = np.zeros((n, 2), np.float32)
            faults = LIB.core_c_sendfx_process(st.ctypes.data_as(fp), n, out.ctypes.data_as(fp), 48000.0)
            return out, faults
        self.assertTrue(take(P.sendfx(0, [0x7F7E])))                           # BD: SEND FX 1 and 2 = 127
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0, 0x0200, 0x1820, 0, 0, 0])))   # FX 1 COMB, P2 24, P3 16; FX 2 off
        run(48000)                                     # the sends' and the TYPE's 10 ms ramps settle
        out, faults = run(9600)
        x = out[:, 0].astype(np.float64)
        ac = np.correlate(x, x, "full")[len(x) - 1:]
        per = 48000 / (55 * 2 ** (24 / 24))
        lag = 300 + int(np.argmax(ac[300:600]))
        self.assertLessEqual(abs(lag - per), 1, f"{lag} vs {per:.1f}")
        self.assertEqual(faults, 0)
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0] * 6)))                  # both off
        run(4800)
        out, _ = run(4800)
        self.assertEqual(float(np.max(np.abs(out))), 0.0)

    def test_process_rings_cloudseed(self):
        """RINGS (P2 42 = A3, P3 0 modal) rings at 220 Hz after a BD impulse; CLOUDSEED (P2 32 HALL) leaves a tail that
        decays; FX 2 runs a second CLOUDSEED at the same time."""
        import numpy as np
        fp = ctypes.POINTER(ctypes.c_float)
        LIB.core_c_sendfx_process.argtypes = [fp, ctypes.c_int, fp, ctypes.c_float]

        def run(n, hit=True):
            st = np.zeros((n, 8), np.float32)
            if hit:
                st[0, 0] = 0.5
            out = np.zeros((n, 2), np.float32)
            faults = LIB.core_c_sendfx_process(st.ctypes.data_as(fp), n, out.ctypes.data_as(fp), 48000.0)
            return out, faults

        def word(hi, lo):
            return hi << 8 | lo << 1
        self.assertTrue(take(P.sendfx(0, [0x7F7E])))   # BD: SEND FX 1 and 2 = 127
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0, word(6, 0), word(42, 0), 0, 0, 0])))   # FX 1 RINGS A3
        run(4800, hit=False)
        out, faults = run(24000)
        x = out[2400:, 0].astype(np.float64) + out[2400:, 1]
        f = np.fft.rfftfreq(len(x), 1 / 48000)
        spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
        band = (f > 150) & (f < 300)
        self.assertAlmostEqual(float(f[band][np.argmax(spec[band])]), 220.0, delta=3.0)
        self.assertEqual(faults, 0)
        # FX 1 and FX 2 both CLOUDSEED HALL
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [word(8, 0), word(8, 0), word(32, 0), 0, word(32, 0), 0])))
        run(9600, hit=False)                           # RINGS fades out; both start clean
        out, faults = run(48000)
        e1 = float(np.sqrt(np.mean(out[4800:9600] ** 2)))
        e2 = float(np.sqrt(np.mean(out[43200:48000] ** 2)))
        self.assertGreater(e1, 1e-4)
        self.assertLess(e2, e1)
        self.assertGreater(e2, 0.0)
        self.assertEqual(faults, 0)
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0] * 6)))

    def test_process_stutter(self):
        """STUTTER (FX 1, TYPE 15): the stems' plain sum at unity while P2 < 64 (the sends ignored); a 0x20 P2 lock
        at a 1/16 grid point (120 BPM MIDI clock) repeats the 1/16 before it sample for sample."""
        import numpy as np
        fp = ctypes.POINTER(ctypes.c_float)
        LIB.core_c_sendfx_process.argtypes = [fp, ctypes.c_int, fp, ctypes.c_float]
        LIB.core_c_sendfx_clock.argtypes = [ctypes.c_int, ctypes.c_int]
        frame = [0]

        def x(i):
            return ((i % 200000) / 262144.0 - 0.25).astype(np.float32)

        def run(n):   # 1000-frame chunks, one tick before each (120 BPM)
            outs, ins = [], []
            for _ in range(n // 1000):
                LIB.core_c_sendfx_clock(0xF8, 0)
                st = np.zeros((1000, 8), np.float32)
                st[:, 0] = x(np.arange(frame[0], frame[0] + 1000))
                st[:, 3] = 0.125
                out = np.zeros((1000, 2), np.float32)
                LIB.core_c_sendfx_process(st.ctypes.data_as(fp), 1000, out.ctypes.data_as(fp), 48000.0)
                outs.append(out), ins.append(st[:, 0] + st[:, 3])
                frame[0] += 1000
            return np.concatenate(outs), np.concatenate(ins)
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0, P.SENDFX_STUTTER << 8, 40 << 1, 0, 0, 0])))   # P2 0, P3 1/16
        LIB.core_c_sendfx_clock(0xFA, 0)   # Start: the next tick is bar 1
        out, dry = run(96000)
        np.testing.assert_array_equal(out[2000:, 0], dry[2000:])   # after the old TYPE fades out and STUTTER in
        np.testing.assert_array_equal(out[2000:, 1], dry[2000:])
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0, P.SENDFX_STUTTER << 8, 127 << 8 | 40 << 1, 0, 0, 0])))
        out2, _ = run(6000)
        np.testing.assert_array_equal(out2[240:, 0], dry[90000 + 240:96000])   # the 1/16 before the grid point
        self.assertTrue(take(P.sendfx(P.FX_TRACK, [0] * 6)))
        run(2000)


if __name__ == "__main__":
    unittest.main()
