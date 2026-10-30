"""The Sidecar sequencer's messages and pattern file (sim/protocol.py 2C..2F, .sqp; core/seq/seq.h): encode / decode,
the file's round trip, and the core reading Python's file byte for byte (core/build/libcore.dylib).

    make -C core lib && python3 -m unittest sim/tests/test_protocol_seq.py
"""
import ctypes
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import protocol as P  # noqa: E402


def body(m):
    cmd, payload = P.parse(m)
    return cmd, payload


class Messages(unittest.TestCase):
    def test_tick(self):
        m = P.seq_tick(0x0ABCDEF, [(0, 3, 16, 2), (4, 63, 64, 0)])
        self.assertEqual(len(m), 4 + 2 + 4 + 1 + 8 + 1)
        d = P.decode_seq(*body(m))
        self.assertEqual(d, dict(kind="tick", blk=0x0ABCDEF, ticks=[(0, 3, 16, 2), (4, 63, 64, 0)]))
        self.assertIsNone(P.decode_seq(P.SEQ_TICK, bytes([1, 0, 0, 0, 0, 1, 12, 0, 16, 2])))   # track 12: no
        self.assertIsNone(P.decode_seq(P.SEQ_TICK, bytes([1, 0, 0, 0, 0, 2, 0, 0, 16, 2])))    # n 2, one tick

    def test_blk_wrap(self):
        b = (1 << 28) - 1
        self.assertEqual(P.unblk4(P.blk4(b)), b)
        self.assertEqual(P.unblk4(P.blk4(b + 5)), 4)

    def test_state(self):
        cases = [(P.seq_transport(P.SEQ_PAUSE, 1234), dict(kind="transport", state=2, blk=1234)),
                 (P.seq_pattern(5, 6), dict(kind="pattern", now=5, next=6)),
                 (P.seq_pattern(5), dict(kind="pattern", now=5, next=None)),
                 (P.seq_switch(0, 1, 99), dict(kind="switch", frm=0, to=1, blk=99)),
                 (P.seq_mutes(0x0FFF), dict(kind="mutes", mask=0x0FFF)),
                 (P.seq_rec(1, 0), dict(kind="rec", live=1, grid=0)),
                 (P.seq_tempo(127.5), dict(kind="tempo", bpm=127.5)),
                 (P.seq_seqmode(True, 4), dict(kind="seqmode", on=1, track=4)),
                 (P.seq_seqmode(False), dict(kind="seqmode", on=0, track=None)),
                 (P.seq_pos(77, 1, 15, 300), dict(kind="pos", blk=77, playing=1, master_step=15, dropped=127))]
        for m, want in cases:
            self.assertEqual(P.decode_seq(*body(m)), want)

    def test_host(self):
        m = P.seq_mask(0b100000010001, P.SEQ_FLAG_RESYNC)
        self.assertEqual(len(m), 10)
        self.assertEqual(P.decode_seq(*body(m)), dict(kind="mask", mask=0b100000010001, flags=2, clock=None, pin=None))
        self.assertEqual(P.decode_seq(*body(P.seq_mask(1, 0, clock=0x800))),
                         dict(kind="mask", mask=1, flags=0, clock=0x800, pin=None))
        # seq-p2 (rytm-mods 1033): the pin part (7 bytes), the silence (46)
        m = P.seq_mask(1, P.SEQ_FLAG_NO_PIN, pin=0b100)
        self.assertEqual(len(m), 14)
        self.assertEqual(P.decode_seq(*body(m)), dict(kind="mask", mask=1, flags=4, clock=0, pin=4))
        self.assertEqual(P.decode_seq(*body(P.seq_silent(0x801))), dict(kind="silent", mask=0x801))
        self.assertIsNone(P.decode_seq(P.SEQ_SILENT, bytes([1, 1, 0x20])))
        self.assertIsNone(P.decode_seq(P.SEQ_MASK, bytes([1, 0, 0x20, 0])))   # hi > 0x1F refused
        self.assertEqual(P.decode_seq(*body(P.seq_page_leds([47, 43, 111, P.SEQ_NONE]))),
                         dict(kind="page_leds", leds=[47, 43, 111, 0x7F]))

    def test_p2_state(self):
        cases = [(P.seq_scale(3, 64, 2, 1), dict(kind="scale", track=3, length=64, speed=2, mode=1)),
                 (P.seq_scale(P.SEQ_NONE, 256, 2, 1, 16), dict(kind="scale", track=None, length=256, speed=2, mode=1, chg=16)),
                 (P.seq_edit(2, 5), dict(kind="edit", op=2, name="PASTE PAT", pattern=5, track=None, ok=1, src=0, arg=0)),
                 (P.seq_edit(14, 0, 3, 1, 1, 0x8005), dict(kind="edit", op=14, name="PASTE TRIGS", pattern=0, track=3, ok=1,
                                                           src=1, arg=0x8005)),
                 (P.seq_dirty(7), dict(kind="dirty", pattern=7)),
                 (P.seq_fill(1), dict(kind="fill", held=1))]
        for m, want in cases:
            self.assertEqual(P.decode_seq(*body(m)), want)
        self.assertIsNone(P.decode_seq(P.SEQ_STATE, bytes([1, P.SEQ_EDIT, 16, 0, 0x7F, 1, 0, 0, 0, 0])))   # op 16: no
        self.assertIsNone(P.decode_seq(P.SEQ_STATE, bytes([1, P.SEQ_SCALE, 12, 16, 0, 2, 1])))   # track 12: no

    def test_ids_free(self):
        self.assertEqual((P.SEQ_TICK, P.SEQ_STATE, P.SEQ_MASK, P.SEQ_PAGE_LEDS), (0x2C, 0x2D, 0x2E, 0x2F))
        taken = {P.CHORD, P.RUI_SRC_VIEW, P.RUI_SRC_VALS, P.RUI_SRC_DIAG, P.FXIN, P.FXIN_STATE, P.SENDFX_LABELS}
        self.assertFalse({0x2C, 0x2D, 0x2E, 0x2F} & taken)
        self.assertFalse({0x2C, 0x2D, 0x2E, 0x2F} & set(range(P.RUI_FIRST, P.RUI_LAST + 1)))
        self.assertNotIn(P.SEQ_SILENT, taken | {P.SLOT_PRINTS, P.SENDFX} | set(range(P.RUI_FIRST, P.RUI_LAST + 1)))


class RytmDump(unittest.TestCase):
    """The stock pattern dump (verified in rytm-emu: rytm-mods tools/emu/sq_emu.py fetch, where this packing gave the
    Rytm's own answer byte for byte)."""
    REC = bytes([0, 0, 0, 5]) + bytes((i * 37 + (i >> 5)) & 0xFF for i in range(P.RYTM_RECORD - 4))

    def test_roundtrip(self):
        m = P.rytm_dump(self.REC, 3)
        self.assertEqual(len(m), 14988)
        self.assertEqual(m[:10], bytes.fromhex("f000203c070054010103"))
        self.assertEqual(P.rytm_dump_parse(m), (3, self.REC))
        bad = bytearray(m)
        bad[200] ^= 1
        self.assertIsNone(P.rytm_dump_parse(bytes(bad)))
        old = bytearray(m)   # the checksum with the pattern number in it (0.3.0's): a real Rytm's never is
        cs = (old[-5] << 7 | old[-4]) + 3
        old[-5], old[-4] = cs >> 7 & 0x7F, cs & 0x7F
        self.assertIsNone(P.rytm_dump_parse(bytes(old)))
        self.assertEqual(P.RYTM_DUMP_REQ, bytes.fromhex("f000203c0700640101800000" "0005f7"))
        self.assertEqual(P.rytm_dump_request(5), bytes.fromhex("f000203c0700640101050000" "0005f7"))

    @unittest.skipUnless(os.path.exists(os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib")), "make -C core lib")
    def test_core_reads_it(self):
        from coreengine import CoreEngine
        card = tempfile.mkdtemp(prefix="rytm-dump-")
        os.makedirs(os.path.join(card, "Samples"))
        core = CoreEngine(card)
        for c in P.rytm_dump(self.REC, 9):
            core.lib.core_c_rytm_byte(c)
        out = ctypes.create_string_buffer(P.RYTM_RECORD)
        gen = ctypes.c_uint32(0)
        core.lib.core_c_rytm_record.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32)]
        self.assertEqual(core.lib.core_c_rytm_record(out, ctypes.byref(gen)), 9)
        self.assertEqual(out.raw, self.REC)


PATTERN = [dict(track=4, hint=0x48, length=96, defaults=(60, 100, 72), swing=62,
                steps={0: dict(flags=1, strum=6, chd=2, inv=0, prob=100, notes=[(57, 100, 384, 0), (60, 100, 384, 6), (64, 100, 384, 12)]),
                       95: dict(flags=1, strum=0, chd=0, inv=0, prob=100, notes=[(72, 80, 30, -40)])}),
           dict(track=0, hint=0x7F, length=0, defaults=(36, 127, 24),
                steps={3: dict(flags=3, strum=0, chd=0, inv=0, prob=50, notes=[(36, 127, 9, 0)])})]


class File(unittest.TestCase):
    def test_roundtrip(self):
        b = P.sqp_pack(PATTERN)
        self.assertEqual(b[:4], b"SQP1")
        self.assertEqual(P.sqp_unpack(b), PATTERN)
        self.assertLess(len(b), 120)   # sparse
        bad = bytearray(b)
        bad[20] ^= 1
        self.assertIsNone(P.sqp_unpack(bytes(bad)))
        bad = bytearray(b)
        bad[4] = 5   # rui1040: v3 reads (trig conditions); seqx: v4 (E2's blocks) too; a later version does not
        self.assertIsNone(P.sqp_unpack(bytes(bad)))

    def test_v4_blocks(self):   # seqx (E2): blocks only when used; known ones onto their tracks, unknown ones kept
        self.assertEqual(P.sqp_pack(PATTERN)[4], 2)
        tr = [dict(track=3, poly=(4, 1, 2, 30, 64, 0), tseed=(1, 77, 1), steps={0: dict(notes=[(60, 100, 24, 0)])})]
        b = P.sqp_pack(tr, seed=(1, 41), blocks=[(0x40, P.SQP_BLK_TRACK, bytes([3, 9]))])
        self.assertEqual(b[4], 4)
        ex = {}
        got = P.sqp_unpack(b, ex)
        self.assertEqual(got[0]["poly"], (4, 1, 2, 30, 64, 0))
        self.assertEqual(got[0]["tseed"], (1, 77, 1))
        self.assertEqual(ex, {"seed": (1, 41), "blocks": [(0x40, P.SQP_BLK_TRACK, bytes([3, 9]))]})
        self.assertEqual(P.sqp_pack(got, seed=ex["seed"], blocks=ex["blocks"]), b)

    def test_on_block(self):   # per pattern poly: a track's on flag, written only where the old-file rule would not say it
        steps = {0: dict(notes=[(60, 100, 24, 0)])}
        self.assertEqual(P.sqp_pack([dict(track=8, on=1, steps=steps), dict(track=9, on=0)])[4], 2)   # the rule: no block
        b = P.sqp_pack([dict(track=8, on=0, steps=steps), dict(track=9, on=1)])
        self.assertEqual(b[4], 4)
        self.assertIn(bytes([P.SQP_BLK_ON, P.SQP_BLK_TRACK, 2, 0, 8, 0]), b)   # not required: an older Sidecar keeps it
        self.assertIn(bytes([P.SQP_BLK_ON, P.SQP_BLK_TRACK, 2, 0, 9, 1]), b)
        got = P.sqp_unpack(b)
        self.assertEqual((got[0]["on"], got[1]["on"]), (0, 1))
        self.assertEqual(P.sqp_pack(got), b)

    def test_seqtop_blocks(self):   # seqtop: the top-8 features' blocks, required, only when used, read back
        tr = [dict(track=2, speed=P.SEQ_SPEEDS.index("3/4"), dir=P.SEQ_DIRS.index("RND"),
                   steps={0: dict(notes=[(60, 100, 24, 0)])})]
        b = P.sqp_pack(tr)
        self.assertEqual(b[4], 4)
        self.assertIn(bytes([P.SQP_BLK_SPEED, P.SQP_BLK_TRACK | P.SQP_BLK_REQUIRED, 2, 0, 2, 4]), b)
        self.assertIn(bytes([P.SQP_BLK_DIR, P.SQP_BLK_TRACK | P.SQP_BLK_REQUIRED, 2, 0, 2, 4]), b)
        got = P.sqp_unpack(b)
        self.assertEqual((got[0]["speed"], got[0]["dir"]), (4, 4))
        self.assertEqual(P.sqp_pack(got), b)
        self.assertEqual(P.sqp_pack([dict(track=2, speed=0, steps={0: dict(notes=[(60, 100, 24, 0)])})])[4], 2)
        r1 = [dict(track=0, rnd=(2, 48, 2, 1), smod=(9, 16, 1, 0), steps={0: dict(notes=[(60, 100, 24, 0)], rnd=1)})]
        b = P.sqp_pack(r1)   # R1 / R1b: a mode, a flagged step, a SEED MOD source
        got = P.sqp_unpack(b)
        self.assertEqual((got[0]["rnd"], got[0]["smod"], got[0]["steps"][0]["rnd"]), ((2, 48, 2, 1), (9, 16, 1, 0), 1))
        self.assertEqual(P.sqp_pack(got), b)
        self.assertEqual(P.sqp_pack([dict(track=0, rnd=P.SEQ_RND_DEFAULT, smod=P.SEQ_SMOD_DEFAULT,
                                          steps={0: dict(notes=[(60, 100, 24, 0)])})])[4], 2)   # the defaults: v2
        v1 = [dict(track=0, steps={2: dict(notes=[(60, 100, 24, 0)], locks=[(1, 85 << 7, 45), (6, 13 << 7)])})]
        b = P.sqp_pack(v1)   # V1: a lock's condition (code 45: 2 %), the other lock none
        self.assertIn(bytes([P.SQP_BLK_LOCK_COND, 7, 4, 0, 0, 2, 1, 45]), b)
        self.assertEqual(P.sqp_unpack(b)[0]["steps"][2]["locks"], [(1, 85 << 7, 45), (6, 13 << 7)])
        self.assertEqual(P.sqp_pack(P.sqp_unpack(b)), b)
        p4 = [dict(track=1, steps={0: dict(notes=[(60, 100, 96, 0)], rt=(4, 0, 1, -12, 0, 0, 50)),
                                   1: dict(notes=[(62, 100, 96, 0)], rt=(0, 0, 0, 0, 0, 0, 0))})]   # (step 2's: off, none)
        b = P.sqp_pack(p4)
        self.assertIn(bytes([P.SQP_BLK_RTRG, 7, 9, 0, 1, 0, 4, 0, 1, 0xF4, 0, 0, 50]), b)
        self.assertEqual(P.sqp_unpack(b)[0]["steps"][0]["rt"], (4, 0, 1, -12, 0, 0, 50))
        x1 = [dict(track=0, steps={2: dict(notes=[(60, 100, 24, 0)], locks=[(1, 85 << 7), (6, 13 << 7)], slide=[6, 9])})]
        b = P.sqp_pack(x1)   # X1: P7's lock slides (9: not locked here, nothing)
        self.assertIn(bytes([P.SQP_BLK_SLIDE, 7, 3, 0, 0, 2, 6]), b)
        self.assertEqual(P.sqp_unpack(b)[0]["steps"][2]["slide"], [6])
        h1 = [dict(track=0, steps={2: dict(notes=[(60, 100, 24, 0)], fx=((7, -2), (4, 30))),
                                   9: dict(notes=[], fx=((9, 1), (0, 0)))})]   # H1: OCT -2 at 30 %; an empty step's JMP 1
        b = P.sqp_pack(h1)
        self.assertIn(bytes([P.SQP_BLK_FX, 7, 11, 0, 0, 2, 7, 0xFE, 4, 30, 9, 9, 1, 0, 0]), b)
        got = P.sqp_unpack(b)
        self.assertEqual((got[0]["steps"][2]["fx"], got[0]["steps"][9]["fx"]), (((7, -2), (4, 30)), ((9, 1), (0, 0))))
        self.assertEqual(P.sqp_pack(got), b)
        ex = {}
        b = P.sqp_pack(tr, scale=(2, 3, 1))   # M1: D dorian, QNT EDIT (not required)
        self.assertIn(bytes([P.SQP_BLK_SCALE, 0, 3, 0, 2, 3, 1]), b)
        got = P.sqp_unpack(b, ex)
        self.assertEqual(ex["scale"], (2, 3, 1))
        self.assertEqual(P.sqp_pack(got, scale=ex["scale"]), b)

    def test_locks(self):
        # seq-ux: v2 keeps each step's locks, a lock trig (locks, no notes) too; a v1 file (no locks) still reads
        import zlib
        t = [dict(track=0, hint=0x48, length=16, defaults=(60, 100, 72),
                  steps={2: dict(flags=1, strum=0, chd=0, inv=0, prob=100, notes=[(64, 100, 48, 0)],
                                 locks=[(1, 85 << 7), (6, 13 << 7 | 64), (17, 41 << 7)]),
                         5: dict(flags=0, strum=0, chd=0, inv=0, prob=100, notes=[], locks=[(0, 11 << 7)])})]
        b = P.sqp_pack(t)
        self.assertEqual(b[4], 2)
        self.assertEqual(P.sqp_unpack(b), t)
        v1 = P.sqp_pack(PATTERN)
        v1 = bytearray(v1)
        v1[4] = 1
        self.assertEqual(P.sqp_unpack(bytes(v1)), PATTERN)
        self.assertEqual(zlib.crc32(bytes(v1[12:])), int.from_bytes(v1[8:12], "little"))

    def test_conditions(self):
        # rui1040: a step's trig condition in flags bits 2..7 (1:2 = 9 << 2): the file is v3 then, v2 without one
        t = [dict(track=1, hint=0x48, length=16, defaults=(60, 100, 72),
                  steps={0: dict(flags=1 | 9 << 2, strum=0, chd=0, inv=0, prob=100, notes=[(60, 100, 48, 0)])})]
        b = P.sqp_pack(t)
        self.assertEqual(b[4], 3)
        self.assertEqual(P.sqp_unpack(b), t)
        t[0]["steps"][0]["flags"] = 1
        self.assertEqual(P.sqp_pack(t)[4], 2)

    def test_rui1036(self):
        m = P.rui_names(0, True, 4, 0, ["pads", "vox"])
        self.assertEqual(m[:9], bytes([0xF0, 0x7D, 0x52, 0x54, 0x47, 1, 0, 1, 4]))
        self.assertEqual(len(m), 10 + 2 * 14)
        self.assertEqual(m[10:14], b"PADS")
        self.assertEqual(P.rui_write(3, 0, 5), bytes([0xF0, 0x7D, 0x52, 0x54, 0x48, 1, 3, 0, 5, 0xF7]))
        d = P.decode_rui(P.RUI_MODE, bytes([1, 0x15, 1, 0]))
        self.assertEqual(d["page"] - P.RUI_PAGE_SEQ, 5)

    def test_strum(self):
        n = [(64, 100, 96, 0), (60, 100, 96, 0), (67, 100, 96, 0)]
        self.assertEqual([x[3] for x in P.seq_strum(n, 10)], [10, 0, 20])
        self.assertEqual([x[3] for x in P.seq_strum(n, -10)], [10, 20, 0])

    @unittest.skipUnless(os.path.exists(os.path.join(HERE, "..", "..", "core", "build", "libcore.dylib")), "make -C core lib")
    def test_core_reads_it(self):
        # the core loads Python's file for its project and pattern, and writes back the same bytes
        from coreengine import CoreEngine
        card = tempfile.mkdtemp(prefix="seq-proto-")
        os.makedirs(os.path.join(card, "Samples"))
        os.makedirs(os.path.join(card, ".rytm", "seq", "PROTO"))
        b = P.sqp_pack(PATTERN)
        with open(os.path.join(card, ".rytm", "seq", "PROTO", "p000.sqp"), "wb") as f:
            f.write(b)
        core = CoreEngine(card)
        core.report(bytes([0xF0, 0x7D, 0x52, 0x54, 0x10]) + b"PROTO" + b"\xf7")
        for _ in range(3000):   # the RAM store (2026-10-26): the project's files are loaded by the idle passes' fill
            core.lib.core_c_idle(10)
        out = ctypes.create_string_buffer(65536)
        core.lib.core_c_seq_pack.argtypes = [ctypes.c_char_p, ctypes.c_int]
        n = core.lib.core_c_seq_pack(out, 65536)
        self.assertEqual(P.sqp_unpack(out.raw[:n]), PATTERN)
        self.assertEqual(out.raw[:n], b)


if __name__ == "__main__":
    unittest.main()
