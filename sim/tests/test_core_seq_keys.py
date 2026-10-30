"""updown1029 (the user, 2026-10-29: "in poly mode i want to use the chromatic mode / pads to play notes. i cant change
octaves now with the up and down arrow. instead of down for the poly track menu it should function down"): the poly
SEQ page's catch mask (37) through the core's host (core/build/libcore.dylib, sim/rui_live.py) - UP / DOWN left to
the Rytm (CHRO's octave), named only under a question or with FUNC held; FUNC + DOWN the track's menu, DOWN alone not.

    make -C core engines lib && uv run python -m unittest sim/tests/test_core_seq_keys.py
"""
import ctypes
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from rui_live import Live, report03  # noqa: E402

UP, DOWN, YES, NO, FUNC, LEFT = 0x42, 0x43, 0x44, 0x45, 0x80, 0x40
FM = 0x48   # core/machines.h kFm


class MaskLive(Live):
    """Live, keeping the last catch mask (37) the host sent."""

    def __init__(self):
        super().__init__()
        self.mask = None

    def drain(self):
        while True:
            k = self.lib.core_c_take_sysex(self.buf, 8192)
            if not k:
                return
            m = self.buf.raw[:k]
            if m[:5] == b"\xF0\x7D\x52\x54\x37":
                self.mask = m[6:-1]   # (the byte after the command: the link's, as Live.send writes it)

    def caught(self, code):
        b = self.mask
        at = 3 + code // 7
        return b is not None and at < len(b) and bool(b[at] >> (code % 7) & 1)

    def hold(self, code, down):
        self.send(0x31, code >> 7, code & 0x7F, 1 if down else 0)
        self.tick(30)


class TestSeqUpDown(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lv = cls.lv = MaskLive()
        lv.begin(tempfile.mkdtemp(prefix="seq_keys_"))
        lv.lib.core_c_seq_command.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        report03(lv, 0, FM, [64] * 8 + [0] * 8)   # BD on 6-OP FM: a Sidecar machine
        lv.tick(30)
        b = ctypes.create_string_buffer(1000)
        for _ in range(200000):   # (after another suite in this process: a project may still be loading)
            lv.lib.core_c_seq_command(b"seq=track:1:on", b, 1000)
            if b"still loading" not in b.value:
                break
            lv.lib.core_c_idle()
        assert b"mask 0x001" in b.value, b.value
        lv.send(0x30, 0x10, 1, 0)   # TRK + TRIG on BD: its SEQ page
        lv.tick(100)

    def test_updown(self):
        lv = self.lv
        self.assertIsNotNone(lv.mask)
        self.assertFalse(lv.caught(UP) or lv.caught(DOWN), "UP / DOWN the Rytm's on the poly SEQ page")
        self.assertTrue(lv.caught(LEFT) and lv.caught(NO) and lv.caught(FUNC))
        lv.key(DOWN)   # DOWN alone (were one to come): no menu, so no question - the mask stays without them
        lv.tick(60)
        self.assertFalse(lv.caught(UP) or lv.caught(DOWN) or lv.caught(YES))
        lv.hold(FUNC, True)   # FUNC held: UP / DOWN (and YES) named
        self.assertTrue(lv.caught(UP) and lv.caught(DOWN) and lv.caught(YES))
        lv.key(DOWN)          # FUNC + DOWN: the menu
        lv.hold(FUNC, False)
        lv.tick(60)   # (FUNC let go: only a question keeps them - the menu is open)
        self.assertTrue(lv.caught(UP) and lv.caught(DOWN) and lv.caught(YES), "the menu: its rows and YES")
        lv.key(NO)            # the menu closed: UP / DOWN the Rytm's again
        lv.tick(60)
        self.assertFalse(lv.caught(UP) or lv.caught(DOWN) or lv.caught(YES))


if __name__ == "__main__":
    unittest.main()
