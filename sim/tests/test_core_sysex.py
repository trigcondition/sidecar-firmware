"""The core's SysEx builders (core/sysex.cpp) against sim/protocol.py's encoders and decoders.

    make -C core lib && uv run python -m unittest sim/tests/test_core_sysex.py
"""
import ctypes
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402
from rmp_e2e import decode_listing  # noqa: E402

LIB = ctypes.CDLL(os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib"))
BUF = ctypes.c_uint8 * 256


def call(fn, *args):
    m = BUF()
    n = fn(m, *args)
    return bytes(m[:n])


class TestCoreSysex(unittest.TestCase):
    def test_names_match_protocol(self):
        names = ["Strings 1", None, "Bräss", "x" * 20, "A", "B", "C", "D", "E"]   # 9: two messages
        want = P.juno_names([n.encode("ascii", "replace").decode() if n else n for n in names])
        joined = "\n".join((n.encode("ascii", "replace").decode() if n else "") for n in names).encode()
        got1 = call(LIB.core_c_names, 0x16, 1, len(names), 0, joined, 8)
        rest = "\n".join(n or "" for n in names[8:]).encode()
        got2 = call(LIB.core_c_names, 0x16, 0, len(names), 8, rest, 1)
        self.assertEqual([got1, got2], want)
        self.assertEqual(call(LIB.core_c_names, 0x17, 1, 0, 0, b"", 0), P.fm_names([])[0])

    def test_juno_knobs_match_protocol(self):
        patch = bytes(range(10, 28))
        self.assertEqual(call(LIB.core_c_juno_knobs, 3, patch), P.juno_knobs(3, list(patch)))

    def test_pool_decodes(self):
        slots = (ctypes.c_uint8 * 2)(1, 9)
        folders = (ctypes.c_uint16 * 2)(5, 0x3FFF)
        files = (ctypes.c_uint16 * 2)(200, 0x3FFF)
        m = call(LIB.core_c_pool, 1, slots, folders, files, b"kick\nsnare long name!", 2)
        self.assertEqual(P.parse(m)[0], P.POOL_TABLE)
        self.assertEqual(P.decode_pool_table(P.parse(m)[1]),
                         (True, [(1, 5, 200, "kick"), (9, 0x3FFF, 0x3FFF, "snare long na")]))

    def test_listing_decodes(self):
        kinds = (ctypes.c_uint8 * 2)(1, 0)
        ids = (ctypes.c_uint32 * 2)(300, 7)
        sizes = (ctypes.c_uint32 * 2)(0, 123456)
        m = call(LIB.core_c_listing, 131, 2, 6, 8, kinds, ids, sizes, b"Loops\nhat", 2)
        self.assertEqual(decode_listing(m[5:-1]), (131, 8, [(1, 300, 0, "Loops"), (0, 7, 123456, "hat")]))

    def test_empty_slots_match_protocol(self):
        # The gate's check-teensy leaves 1A-1D out (plugin generations), so these are checked here.
        for slot in range(1, 6):
            self.assertEqual(call(LIB.core_c_slot_empty, slot), P.meta_messages(slot, 1, 0, fp=0)[0])
            self.assertEqual(call(LIB.core_c_slot_unchanged, slot), P.meta_unchanged(slot, 1))
            m = call(LIB.core_c_slots, slot, P.RES_NOT_INSTALLED)   # 16 bytes: the host's caps (params-delta)
            self.assertTrue(m[-2] & P.HOST_CAPS_DELTA)
            self.assertEqual(m, P.slots([1] * 5, 1, slot, P.RES_NOT_INSTALLED, caps=m[-2]))
        self.assertEqual(call(LIB.core_c_installed_empty), P.list_messages(0, P.LIST_INSTALLED, 1, [])[0])
        m = call(LIB.core_c_slots, 0, 0)
        self.assertEqual(m, P.slots([1] * 5, 1, caps=m[-2]))
        self.assertEqual(P.decode_slots(m[5:-1])["caps"] & P.HOST_CAPS_DELTA, P.HOST_CAPS_DELTA)


class TestCoreNoCard(unittest.TestCase):
    """A card without /Samples (card_ok false, the libraries never begin()): the Rytm's 10 <project> and the other
    messages must not crash the core (a null log function in set_project's load_list took the Mac's Python down)."""

    def test_project_without_card_samples(self):
        import subprocess
        import tempfile
        card = tempfile.mkdtemp(prefix="nocard-")
        code = (f"import sys; sys.path.insert(0, {os.path.join(HERE, '..')!r})\n"
                "from coreengine import CoreEngine\n"
                f"e = CoreEngine({card!r})\n"
                "assert not e.card_ok\n"
                "for p in (b'PRESETS', b'OTHER', b'PRESETS'):\n"
                "    e.report(b'\\xf0\\x7d\\x52\\x54\\x10' + p + b'\\xf7')\n"
                "import random\n"
                "r = random.Random(7)\n"   # the other host messages too, random bodies: none may take the core down
                "for i in range(1500):\n"
                "    c = r.choice([0x10, 0x12, 0x13, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1e, 0x20, 0x23, 0x25, 0x30, 0x31, 0x32, 0x38, 0x39, 0x3a, 0x3c])\n"
                "    e.report(b'\\xf0\\x7d\\x52\\x54' + bytes([c]) + bytes(r.randrange(128) for _ in range(r.randrange(40))) + b'\\xf7')\n"
                "e.frames(64)\n"
                "print('ok')\n")
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout.strip().splitlines()[-1:]), (0, ["ok"]), r.stderr[-500:])


if __name__ == "__main__":
    unittest.main()
