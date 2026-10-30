# tools/tests/test_rmp_pack.py - build sine_test with the ARM toolchain, pack it,
# and check the .rmp's header, relocations and manifest; check that the Python
# layout matches teensy/libraries/RytmPlugins/src/rmp_format.h.
import json, os, re, struct, subprocess, sys, tempfile, unittest, zlib
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rmp_pack

PLUG = os.path.join(ROOT, "plugins", "sine_test")
FMT_DIR = os.path.join(ROOT, "teensy", "libraries", "RytmPlugins", "src")

class PackSineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", PLUG, "sine_test.elf"], check=True)
        cls.rmp = rmp_pack.pack(os.path.join(PLUG, "sine_test.elf"), os.path.join(PLUG, "manifest.json"))

    def test_header(self):
        h = rmp_pack.parse_header(self.rmp)
        self.assertEqual(h["magic"], 0x31504D52)
        self.assertEqual(h["format"], 3)
        self.assertEqual(h["api"], 1)   # the entry table's own api (sine_test: API 1)
        self.assertEqual(h["id"], "rytm.sine_test")
        self.assertEqual(h["version"], 0x010000)
        self.assertEqual(h["entry_offset"], 0)
        self.assertEqual(h["crc32"], zlib.crc32(self.rmp[96:]) & 0xFFFFFFFF)

    def test_entry_relocated(self):
        h = rmp_pack.parse_header(self.rmp)
        relocs = rmp_pack.parse_relocs(self.rmp)
        # the entry table's six function pointers (offsets 16..36) are ABS32 relocations
        abs_offsets = {off for (off, typ, val) in relocs if typ == 0}
        for k in range(6):
            self.assertIn(16 + 4 * k, abs_offsets)
        self.assertTrue(all(off < h["image_size"] for (off, typ, val) in relocs))

    def test_manifest(self):
        m = rmp_pack.parse_manifest(self.rmp)
        self.assertEqual(m["name"], "SINE TEST")
        self.assertEqual(m["knobs"][0]["short"], "TUN")
        self.assertEqual(len(m["knobs"]), 3)

    def test_rejects_init_array(self):
        with self.assertRaises(rmp_pack.PackError):
            rmp_pack.check_sections({".init_array": 4})

    def test_movw_movt_pairing(self):
        # MOVW r0,#0x5678 = F245 6078 ; MOVT r0,#0x1234 = F2C1 2034
        img = bytearray(b"\0" * 8)
        struct.pack_into("<HH", img, 0, 0xF245, 0x6078)
        struct.pack_into("<HH", img, 4, 0xF2C1, 0x2034)
        self.assertEqual(rmp_pack._thumb_imm16(0xF245, 0x6078), 0x5678)
        self.assertEqual(rmp_pack._thumb_imm16(0xF2C1, 0x2034), 0x1234)
        rel = rmp_pack.pair_movs([(0, rmp_pack.R_ARM_THM_MOVW_ABS_NC, 7), (4, rmp_pack.R_ARM_THM_MOVT_ABS, 7)], img)
        self.assertEqual(rel, [(0, 1, 0x12345678), (4, 2, 0x12345678)])
        with self.assertRaises(rmp_pack.PackError):
            rmp_pack.pair_movs([(0, rmp_pack.R_ARM_THM_MOVW_ABS_NC, 7)], img)
        with self.assertRaises(rmp_pack.PackError):
            rmp_pack.pair_movs([(4, rmp_pack.R_ARM_THM_MOVT_ABS, 7)], img)

class HeaderLayout(unittest.TestCase):
    def test_matches_rmp_format_h(self):
        src = r'''
#include <stdio.h>
#include <stddef.h>
#include "rmp_format.h"
int main(void) {
  printf("%zu %zu %zu %zu %zu %zu %zu %zu\n", sizeof(rmp_header), offsetof(rmp_header, id),
    offsetof(rmp_header, version), offsetof(rmp_header, image_size), offsetof(rmp_header, entry_offset),
    offsetof(rmp_header, crc32), offsetof(rmp_header, reserved), sizeof(rmp_reloc));
  printf("%zu %zu %zu %zu %zu %zu %zu %zu\n", sizeof(rmp_knob), offsetof(rmp_knob, enum_first), offsetof(rmp_knob, enum_count),
    offsetof(rmp_knob, long_name), offsetof(rmp_knob, short_name), sizeof(rmp_manifest_head), offsetof(rmp_manifest_head, page2),
    sizeof(rmp_enum_name));
  printf("%zu %zu %zu %zu %zu %zu %zu\n", sizeof(rmp_knob3), offsetof(rmp_knob3, role), offsetof(rmp_knob3, def),
    offsetof(rmp_knob3, long_name), sizeof(rmp_manifest_head3), offsetof(rmp_manifest_head3, page2_title),
    offsetof(rmp_manifest_head3, preset_ext));
  printf("%u %u %d %d %d %d %d %d\n", (unsigned)RMP_FORMAT, (unsigned)RMP_FILE_MAGIC, RMP_ENUM_NAME_BYTES, RMP_ENUM_MAX,
    RMP_TEXT_PRESET, RMP_TEXT_POOL, RMP_ROLE_OFFSET, RMP_LOOK_LIST);
  return 0;
}'''
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "l.c"), os.path.join(d, "l")
            with open(c, "w") as f:
                f.write(src)
            subprocess.run(["cc", "-std=c11", "-I", FMT_DIR, c, "-o", exe], check=True)
            cpp = os.path.join(d, "l.cpp")
            with open(cpp, "w") as f:
                f.write('#include "rmp_format.h"\nint main(){return 0;}\n')
            subprocess.run(["c++", "-std=c++11", "-fsyntax-only", "-I", FMT_DIR, cpp], check=True)
            got = [int(x) for x in subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split()]
        H = rmp_pack.HEADER_FMT
        self.assertEqual(got[0:8], [struct.calcsize(H), struct.calcsize("<IHH"), struct.calcsize("<IHH32s"),
                                    struct.calcsize("<IHH32sII"), struct.calcsize("<IHH32sIIII"),
                                    struct.calcsize("<IHH32sIIIIIIII"), struct.calcsize("<IHH32sIIIIIIIII"), 8])
        self.assertEqual(got[0], 96)
        self.assertEqual(got[8:16], [rmp_pack.KNOB2_BYTES, 3, 4, 6, 18, rmp_pack.HEAD2_BYTES, 62, rmp_pack.ENUM_NAME_BYTES])
        self.assertEqual(got[16:23], [rmp_pack.KNOB_BYTES, 5, 8, 10, rmp_pack.HEAD_BYTES, 64, 77])
        self.assertEqual(got[23:31], [rmp_pack.FORMAT, 0x31504D52, rmp_pack.ENUM_NAME_BYTES, rmp_pack.ENUM_MAX,
                                      rmp_pack.TEXT["preset"], rmp_pack.TEXT["pool"], rmp_pack.ROLE["offset"], rmp_pack.LOOK["list"]])

SINE = {"id": "rytm.sine_test", "version": "1.0.0", "name": "SINE TEST", "short": "SINE", "licence": "MIT",
        "knobs": [{"p": 1, "long": "Tune", "short": "TUN", "look": "centred", "text": "bipolar"},
                  {"p": 2, "long": "Level", "short": "LEV", "look": "fader"},
                  {"p": 3, "long": "Decay", "short": "DEC"}]}


def fake(m):
    """A package of no image around manifest m (the header's fields the parsers read; no ELF, no toolchain)."""
    man, ver, compat = rmp_pack._manifest(m)
    f = [0x31504D52, rmp_pack.package_format(m), 2, m["id"].encode(), ver, compat, 0, 0, 0, 0, len(man), 0,
         zlib.crc32(man) & 0xFFFFFFFF, b"\0" * 20]
    return struct.pack(rmp_pack.HEADER_FMT, *f) + man


class Labels(unittest.TestCase):
    def test_type_labels(self):
        """2026-10-26 (plugins/mdkit): labels per value of a list knob, a block at the manifest's end (footer "RLB1");
        the manifest before it is unchanged, so an older host reads the same knobs and names."""
        m = json.loads(json.dumps(SINE))
        m["knobs"].append(dict(p=9, long="Type", short="TYPE", look="list", text="enum", names=["BD", "SD"]))
        plain = fake(m)
        m["type_labels"] = dict(knob=9, params=[1, 3], sets=[[["Pitch", "PTCH"], ["Decay", "DEC"]],
                                                             [["Snappy", "SNAP"], None]])
        rmp = fake(m)
        self.assertEqual(rmp_pack.parse_manifest(rmp), rmp_pack.parse_manifest(plain))   # the old reading
        self.assertIsNone(rmp_pack.parse_labels(plain))
        self.assertEqual(rmp_pack.parse_labels(rmp), m["type_labels"])
        h, h0 = rmp_pack.parse_header(rmp), rmp_pack.parse_header(plain)
        self.assertEqual((h["format"], h["manifest_size"] - h0["manifest_size"]), (3, 4 + 2 + 2 * 2 * 16 + 6))
        end = 96 + h["image_size"] + 8 * h["reloc_count"] + h["manifest_size"]
        self.assertEqual(rmp[end - 4:end], b"RLB1")
        for bad, text in ((dict(knob=1, params=[3], sets=[[None]]), "not a list knob"),
                          (dict(knob=9, params=[9], sets=[[None], [None]]), "other knobs"),
                          (dict(knob=9, params=[4], sets=[[None], [None]]), "other knobs"),
                          (dict(knob=9, params=[1, 1], sets=[[None, None], [None, None]]), "other knobs"),
                          (dict(knob=9, params=[1], sets=[[None]]), "must hold 2 sets"),
                          (dict(knob=9, params=[1], sets=[[None], [None, None]]), "must hold 1 entries"),
                          (dict(knob=9, params=[1], sets=[[["Way too long label", "X"]], [None]]), "limit is 12"),
                          (dict(knob=9, params=[1], sets=[[["A", "TOOLONG"]], [None]]), "limit is 4"),
                          (dict(knob=9, params=list(range(1, 10)), sets=[[None] * 9] * 2), "1..8"),
                          (dict(knob=9, params=[1]), "exactly")):
            mm = json.loads(json.dumps(m))
            mm["type_labels"] = bad
            with self.assertRaises(rmp_pack.PackError) as cm:
                rmp_pack._manifest(mm)
            self.assertIn(text, str(cm.exception))

    def test_type_lists(self):
        """2026-10-27 (plugins/mdkit's MD): a list knob whose names follow another ("follows", "lists"; footer
        "RLB2"): one label set per name of all the lists; the manifest before it is unchanged (an older host shows
        P10's own names, lists[0])."""
        m = json.loads(json.dumps(SINE))
        m["knobs"].append(dict(p=9, long="Engine", short="ENG", look="list", text="enum", names=["FM", "PI"]))
        m["knobs"].append(dict(p=10, long="Type", short="TYPE", look="list", text="enum", names=["BD", "SD"]))
        plain = fake(m)
        tl = dict(knob=10, follows=9, lists=[["BD", "SD"], ["BD", "MT", "HH"]], params=[1, 3],
                  sets=[[["Pitch", "PTCH"], None], [["Snap", "SNAP"], None], [None, ["Decay", "DEC"]],
                        [["Tune", "TUNE"], None], [["Closed", "CLOS"], ["Ring", "RING"]]])
        m["type_labels"] = tl
        rmp = fake(m)
        self.assertEqual(rmp_pack.parse_manifest(rmp), rmp_pack.parse_manifest(plain))   # the old reading
        self.assertEqual(rmp_pack.parse_labels(rmp), tl)
        h, h0 = rmp_pack.parse_header(rmp), rmp_pack.parse_header(plain)
        self.assertEqual(h["manifest_size"] - h0["manifest_size"], 5 + 2 + 2 + 5 * 8 + 5 * 2 * 16 + 6)
        # review finding 3: a package with an RLB2 block is format 4, so a host from before it (RMP_FORMAT 3) refuses
        # it ("format not supported") instead of showing the first list's names for every value of "follows"; a
        # package without one (RLB1 or none) stays format 3, byte for byte
        self.assertEqual((rmp_pack.package_format(m), rmp_pack.package_format(json.loads(json.dumps(SINE)))), (4, 3))
        m1 = json.loads(json.dumps(m)); m1["type_labels"] = dict(knob=10, params=[1], sets=[[None], [None]])
        self.assertEqual(rmp_pack.package_format(m1), 3)
        self.assertEqual(rmp_pack.FORMAT, 4)
        end = 96 + h["image_size"] + 8 * h["reloc_count"] + h["manifest_size"]
        self.assertEqual(rmp[end - 4:end], b"RLB2")
        five = [[None, None]] * 5
        for bad, text in ((dict(tl, follows=10), "another list knob"),
                          (dict(tl, follows=1), "another list knob"),
                          (dict(tl, lists=[["BD", "SD"]]), "must hold 2 lists"),
                          (dict(tl, lists=[["BD", "SD"], []]), "1..16 names"),
                          (dict(tl, lists=[["BD", "SD"], ["TOOLONGNAME"]], sets=five[:3]), "limit is 7"),
                          (dict(tl, lists=[["SD", "BD"], ["BD", "MT", "HH"]]), "must equal P10's names"),
                          (dict(tl, lists=[["BD", "SD"], ["X"] * 17], sets=[[None, None]] * 19), "1..16 names"),
                          (dict(tl, params=[9]), "other knobs"),
                          (dict(tl, sets=five[:4]), "must hold 5 sets"),
                          ({k: v for k, v in tl.items() if k != "lists"}, "neither")):
            mm = json.loads(json.dumps(m))
            mm["type_labels"] = bad
            with self.assertRaises(rmp_pack.PackError) as cm:
                rmp_pack._manifest(mm)
            self.assertIn(text, str(cm.exception))
        # more than 32 names in all
        mm = json.loads(json.dumps(m))
        mm["knobs"][3]["names"] = ["A", "B", "C"]
        mm["knobs"][4]["names"] = ["X"] * 16
        mm["type_labels"] = dict(knob=10, follows=9, lists=[["X"] * 16] * 3, params=[1], sets=[[None]] * 48)
        with self.assertRaises(rmp_pack.PackError) as cm:
            rmp_pack._manifest(mm)
        self.assertIn("limit is 32", str(cm.exception))


def _armbin():
    """The newest installed Teensyduino compiler (versions compared numerically), else the one this was written with."""
    import glob
    home = os.path.expanduser("~")
    found = glob.glob(os.path.join(home, "Library/Arduino15/packages/teensy/tools/teensy-compile/*/arm/bin"))
    key = lambda p: [int(x) if x.isdigit() else 0 for x in p.split("teensy-compile/")[1].split("/")[0].split(".")]
    return max(found, key=key) if found else os.path.join(home, "Library/Arduino15/packages/teensy/tools/teensy-compile/15.2.1/arm/bin")


ARMBIN = _armbin()
SDK = os.path.join(ROOT, "sdk", "rmp")
ARMFLAGS = ["-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-O2", "-ffunction-sections",
            "-fdata-sections", "-fno-common", "-mword-relocations", "-fno-exceptions", "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables", "-ffreestanding", "-I" + os.path.join(SDK, ".."), "-I" + SDK]
ENTRY = """#include "rmp_api.h"
static int32_t init(const rmp_host *h) { (void)h; return 0; }
RMP_ENTRY const rmp_plugin rmp_entry = { RMP_PLUGIN_MAGIC, RMP_API, 4, 1, init, 0, 0, 0, 0, 0, {0} };
"""

def build_elf(d, body):
    """Compile ENTRY + body with the ARM toolchain and link it with plugin.ld (no --gc-sections, so the odd sections stay)."""
    c, o, elf = (os.path.join(d, n) for n in ("t.c", "t.o", "t.elf"))
    with open(c, "w") as f:
        f.write(ENTRY + body)
    subprocess.run([os.path.join(ARMBIN, "arm-none-eabi-gcc")] + ARMFLAGS + ["-c", "-o", o, c], check=True)
    subprocess.run([os.path.join(ARMBIN, "arm-none-eabi-g++")] + ARMFLAGS + ["-nostdlib", "-nostartfiles", "-Wl,--emit-relocs", "-Wl,--unresolved-symbols=ignore-all", "-Wl,--no-warn-rwx-segments",
                    "-Wl,-T," + os.path.join(SDK, "plugin.ld"), "-o", elf, o, "-lgcc"], check=True)
    return elf

class Negative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.man = os.path.join(PLUG, "manifest.json")
        with open(cls.man) as f:
            cls.manifest = json.load(f)
        subprocess.run(["make", "-C", PLUG, "sine_test.elf"], check=True)
        with open(os.path.join(PLUG, "sine_test.elf"), "rb") as f:
            cls.good = f.read()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def pack_bytes(self, elf, manifest=None):
        e = os.path.join(self.tmp.name, "x.elf"); m = os.path.join(self.tmp.name, "x.json")
        with open(e, "wb") as f:
            f.write(elf)
        with open(m, "w") as f:
            json.dump(manifest if manifest is not None else self.manifest, f)
        return rmp_pack.pack(e, m)

    def fails(self, elf, text, manifest=None):
        with self.assertRaises(rmp_pack.PackError) as cm:
            self.pack_bytes(elf, manifest)
        self.assertIn(text, str(cm.exception))

    def compiled(self, body):
        with tempfile.TemporaryDirectory() as d:
            with open(build_elf(d, body), "rb") as f:
                return f.read()

    def test_minimal_plugin_packs(self):
        rmp = self.pack_bytes(self.compiled(""))
        self.assertEqual(rmp_pack.parse_header(rmp)["entry_offset"], 0)

    def test_entry_not_word_aligned(self):
        real = rmp_pack._symbols
        def shifted(elf, secs):
            out = real(elf, secs)
            for sy in out:
                if sy["name"] == "rmp_entry":
                    sy["value"] += 2
            return out
        rmp_pack._symbols = shifted
        try:
            self.fails(self.compiled(""), "not 4-byte aligned")
        finally:
            rmp_pack._symbols = real

    def test_input_section_aligned_over_8(self):
        self.fails(self.compiled("static const char big[16] __attribute__((used, aligned(16))) = {1};"), "aligned to 16")
        self.pack_bytes(self.compiled("static const char ok[16] __attribute__((used, aligned(8))) = {1};"))   # 8 is fine

    def test_undefined_symbol(self):
        self.fails(self.compiled("extern int host_var; int f(void) { return host_var; }\n"
                                 "static int (*keep)(void) __attribute__((used)) = f;"), "undefined symbols")

    def test_extra_alloc_section(self):
        self.fails(self.compiled("static void f(void) {}\nstatic void (*c)(void) __attribute__((used, section(\".ctors\"))) = f;"),
                   ".ctors")

    def test_init_array(self):
        self.fails(self.compiled("static void f(void) {}\nstatic void (*c)(void) __attribute__((used, section(\".init_array\"))) = f;"),
                   "static constructors")

    def rel_entries(self):
        secs = rmp_pack._sections(self.good)
        rs = next(s for s in secs if s["type"] == 9 and secs[s["info"]]["name"] == ".image")
        return rs, [rs["off"] + 8 * k for k in range(rs["size"] // 8)]

    def patch(self, pos, data):
        b = bytearray(self.good); b[pos:pos + len(data)] = data
        return bytes(b)

    def test_unsupported_relocation(self):
        rs, ent = self.rel_entries()
        off, info = struct.unpack_from("<II", self.good, ent[0])
        self.fails(self.patch(ent[0], struct.pack("<II", off, (info & ~0xFF) | 9)), "unsupported relocation type 9")

    def test_pc_relative_to_outside_image(self):
        rs, ent = self.rel_entries()
        for e in ent:
            off, info = struct.unpack_from("<II", self.good, e)
            if info & 0xFF in (10, 30):   # THM_CALL / THM_JUMP24 -> point at the null symbol
                self.fails(self.patch(e, struct.pack("<II", off, info & 0xFF)), "outside .image")
                return
        self.fail("no PC-relative call relocation in sine_test.elf")

    def test_duplicate_and_misaligned_abs32(self):
        rs, ent = self.rel_entries()
        abs_ = [e for e in ent if struct.unpack_from("<I", self.good, e + 4)[0] & 0xFF == 2]
        o0, i0 = struct.unpack_from("<II", self.good, abs_[0])
        self.fails(self.patch(abs_[1], struct.pack("<II", o0, i0)), "duplicate relocation")
        self.fails(self.patch(abs_[0], struct.pack("<II", o0 + 1, i0)), "not 4-byte aligned")

    def test_elf_type_and_machine(self):
        self.fails(self.patch(0x10, struct.pack("<H", 1)), "ET_EXEC")
        self.fails(self.patch(0x12, struct.pack("<H", 3)), "not ARM")

    def test_malformed_elf(self):
        for n in (8, 60, 200, len(self.good) - 100):
            with self.subTest(n=n):
                self.fails(self.good[:n], "malformed ELF" if n > 52 else "not a 32-bit")
        b = bytearray(self.good); struct.pack_into("<I", b, 0x20, len(b) + 1000)   # section table offset past the end
        self.fails(bytes(b), "malformed ELF")

    def test_missing_input(self):
        with self.assertRaises(rmp_pack.PackError):
            rmp_pack.pack(os.path.join(PLUG, "sine_test.elf"), os.path.join(self.tmp.name, "nope.json"))

    def test_movw_movt_value_near_symbol(self):
        img = bytearray(8)
        struct.pack_into("<HH", img, 0, 0xF245, 0x6078); struct.pack_into("<HH", img, 4, 0xF2C1, 0x2034)
        syms = [dict(name="s", value=0x1000, type=0)]
        movs = [(0, rmp_pack.R_ARM_THM_MOVW_ABS_NC, 0), (4, rmp_pack.R_ARM_THM_MOVT_ABS, 0)]
        with self.assertRaises(rmp_pack.PackError):
            rmp_pack.pair_movs(movs, img, syms)           # 0x12345678 is far from 0x1000
        syms[0]["value"] = 0x12345000
        self.assertEqual(len(rmp_pack.pair_movs(movs, img, syms)), 2)

    def test_bad_manifests(self):
        M = self.manifest
        def with_(**kw):
            m = json.loads(json.dumps(M)); m.update(kw); return m
        def knob(**kw):
            m = json.loads(json.dumps(M)); m["knobs"][0].update(kw); return m
        cases = [
            ({k: v for k, v in M.items() if k != "id"}, "missing required key 'id'"),
            (with_(bogus=1), "unknown key"),
            (with_(id="x" * 32), "limit is 31"),
            (with_(id=""), "must not be empty"),
            (with_(id="a b"), "spaces"),
            (with_(id="a/b"), "'/'"),
            (with_(id="a\\b"), "'/'"),
            (with_(id=".hidden"), "start with '.'"),
            (with_(id="a..b"), "'..'"),
            (with_(id="a\tb"), "printable ASCII"),
            (with_(name="ELEVENCHARS"), "limit is 10"),
            (with_(short="LONGS"), "limit is 4"),
            (with_(licence="L" * 49), "limit is 48"),
            (with_(name="S\u00e9ance"), "printable ASCII"),
            (with_(version="1.0"), "bad version"),
            (with_(version="1.256.0"), "bad version"),
            (with_(compatible_from="2.0.0"), "compatible_from"),
            (with_(page2="both"), "page2"),
            (with_(knobs=[dict(M["knobs"][0], p=0)]), "1..16"),
            (with_(knobs=[dict(M["knobs"][0], p=17)]), "1..16"),
            (with_(knobs=[M["knobs"][0], M["knobs"][0]]), "used twice"),
            (knob(look="round"), "look"),
            (knob(text="roman"), "text"),
            (knob(color="red"), "unknown key"),
            (knob(names=["a", "b"]), "only for look 'list'"),
            (knob(look="list", names=[]), "1..16 names"),
            (knob(look="list", names="ab"), "1..16 names"),
            (knob(look="list", names=["n%d" % i for i in range(17)]), "1..16 names"),
            (knob(look="list", names=["a", ""]), "must not be empty"),
            (knob(look="list", names=["EIGHTCHR"]), "limit is 7"),
            (knob(look="list", names=["caf\u00e9"]), "printable ASCII"),
            (knob(look="list", names=[3]), "must be a string"),
            (knob(long="L" * 13), "limit is 12"),
            (with_(knobs=[dict(M["knobs"][0], p=1 + i % 16) for i in range(33)]), "at most 32"),
            (knob(min=-1), "0..127"),
            (knob(max=128), "0..127"),
            (knob(default=True), "0..127"),
            (knob(min=100, max=10), "above max"),
            (knob(min=10, max=20, default=30), "outside 10..20"),
            (knob(role="sometimes"), "role"),
            (knob(text="preset", role="preset"), "role 'free'"),
            (with_(knobs=[dict(M["knobs"][0], text="preset"), dict(M["knobs"][1], text="preset")]), "at most one"),
            (with_(page2_title="T" * 14), "limit is 13"),
            (with_(preset_ext="t.x"), "letters and digits"),
            (with_(preset_ext="TXT"), "not 'txt'"),
            (with_(preset_ext="EIGHTCHR"), "limit is 7"),
        ]
        for m, text in cases:
            with self.subTest(text=text):
                self.fails(self.good, text, m)

    def test_manifest_encoding(self):
        rmp = self.pack_bytes(self.good)
        m = rmp_pack.parse_manifest(rmp)
        k = m["knobs"]
        self.assertEqual((k[0]["p"], k[0]["look"], k[0]["text"], k[0]["long"]), (1, 1, 1, "Tune"))     # centred, bipolar
        self.assertEqual((k[1]["p"], k[1]["look"], k[1]["text"], k[1]["long"]), (2, 2, 0, "Level"))    # fader, number
        self.assertEqual((k[2]["p"], k[2]["look"], k[2]["text"], k[2]["long"]), (3, 0, 0, "Decay"))    # dial, number
        self.assertEqual((m["short"], m["licence"], m["page2"]), ("SINE", "MIT", 0))
        self.assertEqual(rmp_pack.parse_header(rmp)["manifest_size"], 84 + 26 * 3)
        self.assertTrue(all((x["role"], x["min"], x["max"], x["default"]) == (0, 0, 127, 64) for x in k))
        self.assertEqual((m["page2_title"], m["preset_ext"]), ("", ""))
        self.assertTrue(all(x["enum_count"] == 0 and "names" not in x for x in k))

    def test_enum_names_encoding(self):
        m = json.loads(json.dumps(self.manifest))
        m["knobs"][0].update(look="list", text="enum", names=["ONE", "TWO", "SEVEN77"])
        m["knobs"].append(dict(p=9, long="Mode", short="MOD", look="list", text="enum", names=["A", "B"]))
        rmp = self.pack_bytes(self.good, m)
        k = rmp_pack.parse_manifest(rmp)["knobs"]
        self.assertEqual((k[0]["enum_first"], k[0]["enum_count"], k[0]["names"]), (0, 3, ["ONE", "TWO", "SEVEN77"]))
        self.assertEqual((k[3]["enum_first"], k[3]["enum_count"], k[3]["names"]), (3, 2, ["A", "B"]))
        self.assertEqual((k[1]["enum_count"], k[2]["enum_count"]), (0, 0))
        h = rmp_pack.parse_header(rmp)
        self.assertEqual(h["manifest_size"], 84 + 26 * 4 + 8 * 5)
        table = rmp[len(rmp) - 8 * 5:]                 # the manifest is last: 5 NUL-padded 8-byte names
        self.assertEqual(table[:8], b"ONE\0\0\0\0\0")
        self.assertEqual(table[16:24], b"SEVEN77\0")

    def _downgrade(self, rmp, fmt):
        """The same package as format fmt (1: 20-byte knobs, no names; 2: 22-byte knobs and the name table)."""
        h = rmp_pack.parse_header(rmp)
        start = 96 + h["image_size"] + 8 * h["reloc_count"]
        man3 = rmp[start:start + h["manifest_size"]]
        n, kb, hb = man3[63], rmp_pack.KNOB_BYTES, rmp_pack.HEAD_BYTES
        knobs = [man3[hb + kb * i:hb + kb * (i + 1)] for i in range(n)]
        if fmt == 1:
            man = man3[:64] + b"".join(k[:3] + b"\0" + k[10:26] for k in knobs)
        else:
            man = man3[:64] + b"".join(k[:5] + b"\0" + k[10:26] for k in knobs) + man3[hb + kb * n:]
        body = rmp[96:start] + man
        body += b"\0" * (-len(body) % 4)
        f = list(struct.unpack(rmp_pack.HEADER_FMT, rmp[:96]))
        f[1] = fmt; f[10] = len(man); f[12] = zlib.crc32(body) & 0xFFFFFFFF
        return struct.pack(rmp_pack.HEADER_FMT, *f) + body

    def test_reads_formats_1_and_2(self):
        """Packages installed before format 3 still parse, with format 3's defaults."""
        rmp = self.pack_bytes(self.good)          # sine_test: every knob at the defaults
        want = rmp_pack.parse_manifest(rmp)
        for fmt in (1, 2):
            old = self._downgrade(rmp, fmt)
            self.assertEqual(rmp_pack.parse_header(old)["format"], fmt)
            self.assertEqual(rmp_pack.parse_manifest(old), want)

    def test_format3_fields(self):
        m = json.loads(json.dumps(self.manifest))
        m.update(page2="params", page2_title="SINE PAGE 2", preset_ext="sin")
        m["knobs"] = [dict(p=1, long="Preset", short="PRE", text="preset"),
                      dict(p=2, long="Level", short="LEV", look="fader", role="preset", min=10, max=120, default=100),
                      dict(p=3, long="Detune", short="DET", look="centred", text="bipolar", role="offset"),
                      dict(p=5, long="Sample", short="SMP", text="pool", default=0),
                      dict(p=9, long="Shape", short="SHP", look="list", text="enum", names=["SIN", "TRI"])]
        rmp = self.pack_bytes(self.good, m)
        x = rmp_pack.parse_manifest(rmp)
        self.assertEqual((x["page2"], x["page2_title"], x["preset_ext"]), (1, "SINE PAGE 2", "sin"))
        k = x["knobs"]
        self.assertEqual((k[0]["text"], k[0]["role"], k[0]["min"], k[0]["max"], k[0]["default"]), (6, 0, 0, 127, 64))
        self.assertEqual((k[1]["role"], k[1]["min"], k[1]["max"], k[1]["default"]), (1, 10, 120, 100))
        self.assertEqual((k[2]["text"], k[2]["role"]), (1, 2))
        self.assertEqual((k[3]["text"], k[3]["default"]), (7, 0))
        self.assertEqual(k[4]["names"], ["SIN", "TRI"])
        h = rmp_pack.parse_header(rmp)
        self.assertEqual((h["format"], h["manifest_size"]), (3, 84 + 26 * 5 + 8 * 2))

    def test_voices_and_pitch_flags(self):   # seqx (E0): manifest max_voices / pitch -> the header's flags
        self.assertEqual(rmp_pack.parse_header(self.pack_bytes(self.good))["flags"], 0)   # an old manifest: 0 (mono)
        m = json.loads(json.dumps(self.manifest))
        m.update(max_voices=6, pitch="none")
        f = rmp_pack.parse_header(self.pack_bytes(self.good, m))["flags"]
        self.assertEqual(f, 0x1 | 5 << 4 | 2 << 8)
        m.update(max_voices=1, pitch="note")
        self.assertEqual(rmp_pack.parse_header(self.pack_bytes(self.good, m))["flags"], 0)
        for bad in (dict(max_voices=0), dict(max_voices=9), dict(max_voices="4"), dict(pitch="tune")):
            mm = json.loads(json.dumps(self.manifest))
            mm.update(bad)
            with self.assertRaises(rmp_pack.PackError):
                rmp_pack._manifest(mm)

    def test_api_from_entry_table(self):
        """The header's api is the entry table's own (an API-1 plugin stays an API-1 package)."""
        with open(os.path.join(SDK, "rmp_api.h")) as f:
            n = int(re.search(r"#define RMP_API (\d+)u", f.read()).group(1))
        self.assertEqual(rmp_pack.rmp_api(), n)
        self.assertEqual(rmp_pack.parse_header(self.pack_bytes(self.good))["api"], 1)
        two = self.compiled("")                   # ENTRY uses RMP_API (2)
        self.assertEqual(rmp_pack.parse_header(self.pack_bytes(two))["api"], 2)

    def test_entry_api_out_of_range(self):
        bad = self.compiled("").replace(struct.pack("<II", 0x504D5252, 2), struct.pack("<II", 0x504D5252, 9), 1)
        self.fails(bad, "api 9")

class Cli(unittest.TestCase):
    def test_cli_end_to_end(self):
        subprocess.run(["make", "-C", PLUG, "sine_test.elf"], check=True)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "o.rmp")
            r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "rmp_pack.py"), os.path.join(PLUG, "sine_test.elf"),
                                os.path.join(PLUG, "manifest.json"), out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("rytm.sine_test", r.stdout)
            with open(out, "rb") as f:
                data = f.read()
            self.assertEqual(rmp_pack.parse_header(data)["magic"], 0x31504D52)
            bad = os.path.join(d, "bad.json")
            with open(bad, "w") as f:
                f.write('{"id": "x"}')
            r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "rmp_pack.py"), os.path.join(PLUG, "sine_test.elf"),
                                bad, os.path.join(d, "n.rmp")], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("rmp_pack:", r.stderr)
            self.assertNotIn("Traceback", r.stderr)

if __name__ == "__main__":
    unittest.main()
