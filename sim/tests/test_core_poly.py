"""Poly FM's chords (core/poly.cpp) against sim/protocol.py's twin: every expansion, the detection's round trip, the
value texts, the 28 write-back / reply bytes.

    make -C core lib && uv run python -m unittest sim/tests/test_core_poly.py
"""
import ctypes
import itertools
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402

LIB = ctypes.CDLL(os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib"))


def c_expand(root, chd, inv, spr, voi):
    out = (ctypes.c_uint8 * 8)()
    n = LIB.core_c_poly_expand(root, chd, inv, spr, voi, out)
    return list(out[:n])


def c_detect(notes):
    a = (ctypes.c_uint8 * len(notes))(*notes)
    r = (ctypes.c_int * 6)()
    LIB.core_c_poly_detect(a, len(notes), r)
    return list(r)


class TestCorePoly(unittest.TestCase):
    def test_expand_matches(self):
        for chd, inv, spr, voi in itertools.product(range(16), range(4), range(3), range(7)):
            for root in (0, 36, 60, 100, 120):
                self.assertEqual(c_expand(root, chd, inv, spr, voi), P.chord_expand(root, chd, inv, spr, voi),
                                 (root, chd, inv, spr, voi))

    def test_detect_round_trip(self):
        for chd, inv, spr, voi in itertools.product(range(1, 16), range(4), range(3), range(7)):
            notes = P.chord_expand(55, chd, inv, spr, voi)
            if len(notes) < 2:
                continue
            root, c, i, s, v, exact = c_detect(notes)
            self.assertTrue(exact)
            self.assertEqual(P.chord_expand(root, c, i, s, v), notes)
        self.assertEqual(c_detect([62])[:2], [62, 0])   # one note: OFF

    def test_texts(self):
        buf = ctypes.create_string_buffer(16)
        for k in range(4):
            for v in range(128):
                LIB.core_c_poly_text(k, v, buf, 16)
                self.assertEqual(buf.value.decode(), P.chord_value_text(k, v), (k, v))
        self.assertEqual(P.chord_params([0] * 12 + [0, 120, 0, 100]), (0, 0, 0, 0))   # an old sound's sample words

    def test_messages(self):
        w = P.chord_write(8, 60, 7, 1, 0, 4)
        self.assertEqual(w, bytes.fromhex("f07d52542801083c07010004f7"))
        self.assertEqual(len(w), 13)
        cmd, payload = P.parse(w)
        self.assertEqual((cmd, P.decode_chord(payload)), (0x28, ("write", 8, 60, 7, 1, 0, 4)))
        cmd, payload = P.parse(P.chord_reply(8, 4, 0))
        self.assertEqual(P.decode_chord(payload), ("reply", 8, 4, 0))
        self.assertIsNone(P.decode_chord(bytes([1, 8, 60])))   # a short write: refused


if __name__ == "__main__":
    unittest.main()
