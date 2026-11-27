"""The remote-UI messages (sim/protocol.py 0x30..0x3F, rytm-mods 0026-remote-ui): packing, a full frame's size, the
Rytm's reports, and no clash with the other ids.

    python3 -m unittest sim/tests/test_protocol_rui.py
"""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402
import remote_ui as R  # noqa: E402


class RemoteUi(unittest.TestCase):
    def test_pack_roundtrip(self):
        rnd = random.Random(1)
        for n in (1, 6, 7, 8, 13, 14, 1024):
            d = bytes(rnd.randrange(256) for _ in range(n))
            p = P.pack7(d)
            self.assertTrue(all(b < 0x80 for b in p))
            self.assertEqual(len(p), n + (n + 6) // 7)
            self.assertEqual(P.unpack7(p), d)

    def test_full_frame(self):
        fb = R.checkerboard()
        m = P.rui_rect(5, 0, 0, 16, 8, fb)
        self.assertEqual(len(m), 1183)
        cmd, body = P.parse(m)
        self.assertEqual((cmd, list(body[:6])), (P.RUI_RECT, [1, 5, 0, 0, 16, 8]))
        self.assertEqual(P.unpack7(body[6:]), bytes(fb))   # column-major = the framebuffer itself

    def test_tile_rect_order(self):
        fb = bytes(range(256)) * 4
        cmd, body = P.parse(P.rui_rect(0, 2, 3, 1, 2, fb))
        raw = P.unpack7(body[6:])
        self.assertEqual(list(raw[:4]), [fb[3 + 8 * 16], fb[4 + 8 * 16], fb[3 + 8 * 17], fb[4 + 8 * 17]])

    def test_reports(self):
        self.assertEqual(P.decode_rui(P.RUI_MODE, bytes([1, 0, 1, 2])), dict(page=0, on=1, reason="timeout"))
        self.assertEqual(P.decode_rui(P.RUI_KEY, bytes([1, 1, 6, 1])), dict(key=0x86, down=1))
        self.assertEqual(P.decode_rui(P.RUI_ENC, bytes([1, 3, 61, 0])), dict(enc=3, delta=-3, func=0))
        self.assertIsNone(P.decode_rui(P.RUI_ENC, bytes([2, 3, 61, 0])))

    def test_hello_splash(self):   # 1023
        self.assertEqual(P.decode_rui(P.RUI_HELLO, bytes([1, 5, 0, 1])), dict(up=640, caps=1))
        self.assertIsNone(P.decode_rui(P.RUI_HELLO, bytes([1, 5, 0])))
        self.assertEqual(P.rui_splash(64), bytes([0xF0, 0x7D, 0x52, 0x54, 0x36, 1, 64, 0xF7]))
        self.assertEqual(P.rui_splash(500)[6], 127)
        used = {P.RUI_MODE, P.RUI_KEY, P.RUI_ENC, P.RUI_RECT, P.RUI_COMMIT, P.RUI_OPEN, P.RUI_KEEPALIVE, P.RUI_TEXT,
                P.RUI_TEXT_CLEAR, P.RUI_ECHO, P.RUI_MASK}
        self.assertFalse({P.RUI_HELLO, P.RUI_SPLASH} & used)
        self.assertFalse({P.RUI_HELLO, P.RUI_SPLASH} & {0x33, 0x34, 0x3B})   # reserved for M2 (pads, LEDs)

    def test_ids_clear(self):
        sendfx = (P.SENDFX, P.SENDFX_NAMES, P.SENDFX_STATE, P.SENDFX_ASK)
        self.assertEqual(sendfx, (0x20, 0x21, 0x22, 0x23))
        rui = (P.RUI_MODE, P.RUI_KEY, P.RUI_ENC, P.RUI_RECT, P.RUI_COMMIT, P.RUI_OPEN, P.RUI_KEEPALIVE)
        self.assertTrue(all(P.RUI_FIRST <= i <= P.RUI_LAST for i in rui))
        self.assertFalse(set(rui) & set(sendfx))
        self.assertEqual(P.RUI_FIRST, 0x30)

    def test_machines_1025(self):
        m = P.rui_machines(0, [(41, 0x08)] + [(0, P.RUI_NOT_OURS)] * 11)   # the snapshot: JUNO on BD, the rest analog
        cmd, body = P.parse(m)
        self.assertEqual((cmd, len(m)), (P.RUI_MACHINES, 33))
        d = P.decode_rui(cmd, body)
        self.assertEqual((d["t0"], len(d["tracks"]), d["tracks"][0]), (0, 12, [41, 0x08]))
        one = P.decode_rui(*P.parse(P.rui_machines(5, [(40, 0x4D)])))   # MT/HT on 6-OP FM: voice 5 | 0x48
        self.assertEqual(one, dict(t0=5, tracks=[[40, 0x4D]]))
        self.assertIsNone(P.decode_rui(P.RUI_MACHINES, bytes([1, 11, 2, 0, 0x7F, 0, 0x7F])))   # past track 11

    def test_src_view_1027(self):
        m = P.rui_src_view(1, 1)   # SD's stock SRC page shown
        self.assertEqual(m, bytes([0xF0, 0x7D, 0x52, 0x54, 0x29, 1, 1, 1, 0xF7]))
        self.assertEqual(P.decode_rui(*P.parse(m)), dict(track=1, on=1))
        v = P.rui_src_vals(0, 0, [3, 100.5, 20])   # BD's PAT CUT RES
        cmd, body = P.parse(v)
        self.assertEqual((cmd, len(v)), (P.RUI_SRC_VALS, 10 + 2 * 3))
        self.assertEqual(P.decode_rui(cmd, body), dict(track=0, first=0, values=[3, 100.5, 20]))
        self.assertIsNone(P.decode_rui(P.RUI_SRC_VALS, bytes([1, 0, 15, 2, 1, 0, 1, 0])))   # past P16
        self.assertIsNone(P.decode_rui(P.RUI_SRC_VIEW, bytes([1, 12, 1])))                 # past track 11
        self.assertFalse({P.RUI_SRC_VIEW, P.RUI_SRC_VALS} & {P.FXIN, P.FXIN_STATE, P.SLOT_PRINTS, P.SENDFX})

    def test_bank_list_param_and_p13_announce(self):
        # 2026-11-24 TBLB: 4A's optional 6th byte (12 = P13, the only value) and the host's 4A 7D 1 (caps2 bit 32)
        self.assertEqual((P.RUI_CAPS2_LISTP13, P.RUI_BANK_PARAM_P13), (32, 12))
        self.assertEqual(P.rui_list_p13_announce(), bytes([0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0x7D, 1, 0xF7]))
        old = P.decode_bank_list(bytes([1, 0, 1, 5, 0x50, 0]))
        self.assertEqual(old, dict(track=0, on=1, code=5 * 128 + 5, why="open"))   # no byte: as before
        new = P.decode_bank_list(bytes([1, 0, 1, 5, 0x50, 0, 12]))
        self.assertEqual(new, dict(track=0, on=1, code=5 * 128 + 5, why="open", param=12))
        self.assertIsNone(P.decode_bank_list(bytes([1, 0, 1, 5, 0x50, 0, 5])))      # any other value
        self.assertIsNone(P.decode_bank_list(bytes([1, 0, 1, 5, 0x50, 0, 12, 0])))  # longer
        self.assertFalse(P.RUI_CAPS2_LISTP13 & (P.RUI_CAPS2_ARROWS | 2 | 4 | 8 | P.RUI_CAPS2_REHELLO))

    def test_pixel_layout(self):
        fb = bytearray(1024)
        R.pset(fb, 0, 0)
        R.pset(fb, 127, 63)
        self.assertEqual((fb[7], fb[8 * 127]), (0x01, 0x80))


if __name__ == "__main__":
    unittest.main()
