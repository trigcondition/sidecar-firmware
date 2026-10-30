#!/usr/bin/env python3
"""rmp_pack.py ELF MANIFEST.json OUT.rmp - a machine plugin package.

The ELF is linked by sdk/rmp/plugin.ld: one section .image at address 0 (entry
table first, code, rodata, data, then bss as NOBITS space at its end) with
--emit-relocs. Relocations kept: R_ARM_ABS32 (2), R_ARM_THM_MOVW_ABS_NC (47),
R_ARM_THM_MOVT_ABS (48); PC-relative ones need nothing (they stay valid when the
whole image moves). Anything else is refused, as are a non-empty .init_array,
undefined symbols and an image over 1 MB. Relocation sections for other
sections (.debug_frame ...) are ignored. Format: rmp_format.h.

An AArch64 ELF (ELFCLASS64, EM_AARCH64 183; sdk/rmp/plugin.mk's a64 target, linked by sdk/rmp/plugin.ld the same
way) gives an RMPA package (magic "RMPA", 2026-10-17, docs/user-machines-pi.md): R_AARCH64_ABS64 (257) words become
RMP_REL_ABS64 (3). The image has two parts (sdk/rmp/plugin_a64.ld): [0, __rmp_data_start) code and read-only data,
placed in executable memory, and [__rmp_data_start, end) data and bss, placed apart in RW memory (W^X); the
offset goes in the header's reserved[0..3]. An ADRP in the code that reaches the data part becomes
RMP_REL_A64_ADRP_DATA (4); the :lo12: offsets, B/BL and literal loads inside one part need nothing (both parts
start on a page); anything else that crosses the parts, and anything absolute that is not a 64-bit word (ABS32,
MOVW_UABS, GOT), is refused."""
import json, os, re, struct, sys, zlib

class PackError(Exception):
    pass

R_ARM_ABS32, R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS = 2, 47, 48
# PC-relative (stay valid when the whole image moves): REL32, THM_CALL, THM_JUMP*, PREL31,
# THM_ALU_PREL_11_0, THM_PC12, V4BX (needs nothing), ...
PC_RELATIVE = {3, 10, 11, 25, 28, 29, 30, 40, 42, 51, 52, 53, 54, 102, 103}
LOOK = {"dial": 0, "centred": 1, "fader": 2, "list": 3}
TEXT = {"number": 0, "bipolar": 1, "enum": 2, "list": 3, "note": 4, "time": 5, "preset": 6, "pool": 7}
ROLE = {"free": 0, "preset": 1, "offset": 2}
HEADER_FMT = "<IHH32sIIIIIIIII20s"   # = rmp_header in rmp_format.h: 76 bytes of fields + reserved[20] = 96
HEADER_KEYS = ["magic", "format", "api", "id", "version", "compatible_from", "image_size", "bss_size",
               "entry_offset", "reloc_count", "manifest_size", "flags", "crc32"]
SHT_PROGBITS, SHT_SYMTAB, SHT_RELA, SHT_REL = 1, 2, 4, 9
SHF_ALLOC = 2
STT_SECTION = 3
RMP_PLUGIN_BYTES = 72   # sizeof(rmp_plugin) on the Teensy (16 + 6 pointers + 8 reserved pointers)
MAX_IMAGE = 1 << 20
MAX_ALIGN = 8   # rmp_arena_alloc returns 8-aligned blocks: no input section may need more
API_H = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sdk", "rmp", "rmp_api.h")

def rmp_api():
    """RMP_API from sdk/rmp/rmp_api.h (`#define RMP_API <n>u`)."""
    try:
        with open(API_H) as f:
            m = re.search(r"^\s*#\s*define\s+RMP_API\s+(\d+)[uU]?\b", f.read(), re.M)
    except OSError as e:
        raise PackError(f"cannot read {API_H}: {e}")
    if not m:
        raise PackError(f"no '#define RMP_API <n>u' in {API_H}")
    return int(m.group(1))

def _sections(elf):
    if len(elf) < 0x34 or elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        raise PackError("not a 32-bit little-endian ELF")
    etype, emachine = struct.unpack_from("<HH", elf, 0x10)
    if etype != 2:
        raise PackError(f"ELF type {etype} is not ET_EXEC (link with sdk/rmp/plugin.ld, not -r/-shared)")
    if emachine != 40:
        raise PackError(f"ELF machine {emachine} is not ARM (40)")
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link, info=info, align=align, entsize=entsize))
    strtab = secs[shstrndx]
    for s in secs:
        end = elf.index(b"\0", strtab["off"] + s["name"])
        s["name"] = elf[strtab["off"] + s["name"]:end].decode()
    return secs

def check_sections(sizes):
    if sizes.get(".init_array", 0):
        raise PackError("static constructors (.init_array) are not supported: construct objects in voice_init")

def check_alloc_sections(secs):
    """Only .image (and the unwind index, which is not loaded) may carry loadable content."""
    for s in secs:
        if s["flags"] & SHF_ALLOC and s["size"] and s["name"] not in (".image", ".ARM.exidx"):
            raise PackError(f"output section {s['name']} ({s['size']} B) would be dropped: a plugin may only "
                            "have .image (no static constructors/destructors, .init/.fini, exception tables)")

def _symbols(elf, secs):
    symtab = next((s for s in secs if s["type"] == SHT_SYMTAB), None)
    if symtab is None:
        raise PackError("ELF has no symbol table")
    strs = secs[symtab["link"]]
    out = []
    for i in range(symtab["size"] // 16):
        name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf, symtab["off"] + 16 * i)
        end = elf.index(b"\0", strs["off"] + name)
        out.append(dict(name=elf[strs["off"] + name:end].decode(), value=value, shndx=shndx, bind=info >> 4, type=info & 15))
    return out

def _thumb_imm16(hw1, hw2):
    imm4 = hw1 & 0xF; i = (hw1 >> 10) & 1; imm3 = (hw2 >> 12) & 7; imm8 = hw2 & 0xFF
    return (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8

def pair_movs(movs, image, syms=None):
    """MOVW/MOVT: the linked instructions hold the two halves of the final value;
    pair each MOVW with the nearest MOVT of the same symbol within 64 bytes (gcc
    emits them together) and record the full value in both, so the loader can
    rebuild both halves. With `syms` (the ELF symbol list), the full value must
    lie near the symbol's value (+-64 KB; a section symbol: inside the image).
    Returns [(offset, type, full_value)]."""
    movw = [(o, si) for (o, t, si) in movs if t == R_ARM_THM_MOVW_ABS_NC]
    movt = [(o, si) for (o, t, si) in movs if t == R_ARM_THM_MOVT_ABS]
    used, out = set(), []
    for (ow, si) in movw:
        cand = [ot for (ot, sj) in movt if sj == si and ot not in used and abs(ot - ow) <= 64]
        if not cand:
            raise PackError(f"MOVW at {ow:#x} without a MOVT of the same symbol")
        ot = min(cand, key=lambda x: abs(x - ow)); used.add(ot)
        full = _thumb_imm16(*struct.unpack_from("<HH", image, ot)) << 16 | _thumb_imm16(*struct.unpack_from("<HH", image, ow))
        if syms is not None:
            sy = syms[si]
            ok = (0 <= full < MAX_IMAGE) if sy["type"] == STT_SECTION else abs(full - sy["value"]) < 1 << 16
            if not ok:
                raise PackError(f"MOVW/MOVT at {ow:#x}/{ot:#x}: value {full:#x} is not near its symbol "
                                f"{sy['name']!r} ({sy['value']:#x}); the pair does not hold one address")
        out += [(ow, 1, full), (ot, 2, full)]
    if len(used) != len(movt):
        raise PackError("a MOVT without its MOVW")
    return out

R_AARCH64_ABS64 = 257
A64_RELATIVE = {260, 261, 262, 273, 274, 275, 276, 277, 278, 279, 280, 282, 283, 284, 285, 286, 299}
A64_MAX_ALIGN = 4096
RMP_PLUGIN_BYTES_A64 = 128   # sizeof(rmp_plugin) on AArch64 (16 + 14 pointers)
MAGIC_A64 = 0x41504D52       # "RMPA"

def _sections64(elf):
    etype, emachine = struct.unpack_from("<HH", elf, 0x10)
    if etype != 2:
        raise PackError(f"ELF type {etype} is not ET_EXEC (link with sdk/rmp/plugin.ld, not -r/-shared)")
    if emachine != 183:
        raise PackError(f"ELF machine {emachine} is not AArch64 (183)")
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link, info=info, align=align, entsize=entsize))
    strtab = secs[shstrndx]
    for s in secs:
        end = elf.index(b"\0", strtab["off"] + s["name"])
        s["name"] = elf[strtab["off"] + s["name"]:end].decode()
    return secs

def _symbols64(elf, secs):
    symtab = next((s for s in secs if s["type"] == SHT_SYMTAB), None)
    if symtab is None:
        raise PackError("ELF has no symbol table")
    strs = secs[symtab["link"]]
    out = []
    for i in range(symtab["size"] // 24):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", elf, symtab["off"] + 24 * i)
        end = elf.index(b"\0", strs["off"] + name)
        out.append(dict(name=elf[strs["off"] + name:end].decode(), value=value, shndx=shndx, bind=info >> 4, type=info & 15))
    return out

def _build64(elf, man, manifest, ver, compat):
    """An AArch64 ELF -> an RMPA package (see the module docstring)."""
    secs = _sections64(elf)
    by = {s["name"]: s for s in secs}
    check_sections({n: s["size"] for n, s in by.items()})
    for s in secs:
        if s["flags"] & SHF_ALLOC and s["size"] and s["name"] != ".image":
            raise PackError(f"output section {s['name']} ({s['size']} B) would be dropped: a plugin may only "
                            "have .image (no static constructors/destructors, .init/.fini, exception tables)")
    img = by.get(".image")
    if not img or img["addr"] != 0:
        raise PackError(".image missing or not at address 0 (link with sdk/rmp/plugin.ld)")
    if img["type"] != SHT_PROGBITS:
        raise PackError(".image is not PROGBITS")
    if img["align"] > A64_MAX_ALIGN:
        raise PackError(f".image is aligned to {img['align']} B: the loader places RMPA images on 4096-byte boundaries")
    img_idx = secs.index(img)
    syms = _symbols64(elf, secs)
    und = [s["name"] for s in syms if s["shndx"] == 0 and s["name"]]
    if und:
        raise PackError("undefined symbols (a plugin may not use host symbols): " + ", ".join(sorted(set(und))))
    sym = {s["name"]: s["value"] for s in syms}
    for need in ("__rmp_bss_start", "__rmp_end", "rmp_entry"):
        if need not in sym:
            raise PackError(f"symbol {need} missing (link with sdk/rmp/plugin.ld and RMP_ENTRY)")
    bss_start, end = sym["__rmp_bss_start"], sym["__rmp_end"]
    if not 0 <= bss_start <= end or bss_start > img["size"]:
        raise PackError("bad __rmp_bss_start/__rmp_end")
    image = bytearray(elf[img["off"]:img["off"] + bss_start])
    bss = end - bss_start
    if len(image) + bss > MAX_IMAGE:
        raise PackError("image over 1 MB")
    entry = sym["rmp_entry"]
    if entry % 8:
        raise PackError(f"rmp_entry at {entry:#x} is not 8-byte aligned")
    if entry + RMP_PLUGIN_BYTES_A64 > len(image):
        raise PackError(f"rmp_entry at {entry:#x} does not fit inside the image ({len(image)} B)")
    api, = struct.unpack_from("<I", image, entry + 4)
    if not 1 <= api <= rmp_api():
        raise PackError(f"entry table api {api} is not 1..{rmp_api()} (sdk/rmp/rmp_api.h)")
    data_start = sym.get("__rmp_data_start", -1)
    if data_start < 0 or data_start % 4096 or data_start > len(image) or entry + RMP_PLUGIN_BYTES_A64 > data_start:
        raise PackError("__rmp_data_start missing or not page-aligned after the entry table (link with sdk/rmp/plugin_a64.ld)")
    ADRP = (275, 276)
    LO12 = (277, 278, 284, 285, 286, 299)   # page offsets: unchanged when both parts sit on page boundaries
    relocs, seen = [], set()
    for rs in secs:
        if rs["type"] not in (SHT_REL, SHT_RELA) or secs[rs["info"]]["name"] != ".image":
            continue
        step = 24 if rs["type"] == SHT_RELA else 16
        for k in range(rs["size"] // step):
            off, info = struct.unpack_from("<QQ", elf, rs["off"] + step * k)
            add = struct.unpack_from("<q", elf, rs["off"] + step * k + 16)[0] if step == 24 else 0
            typ, sidx = info & 0xFFFFFFFF, info >> 32
            if typ == 0:
                continue
            if off + 4 > len(image):
                raise PackError(f"relocation at {off:#x} outside the loaded image")
            if sidx >= len(syms):
                raise PackError(f"relocation at {off:#x}: bad symbol index {sidx}")
            sy = syms[sidx]
            in_data = off >= data_start
            if typ == R_AARCH64_ABS64:   # the loader moves it by its value: below __rmp_data_start the code's base
                if off % 8 or off + 8 > len(image) or off in seen:
                    raise PackError(f"ABS64 relocation at {off:#x}: not 8-byte aligned, outside the image or duplicated")
                seen.add(off)
                relocs.append((off, 3))
                continue
            if typ not in A64_RELATIVE:
                raise PackError(f"unsupported AArch64 relocation type {typ} at {off:#x} (build with -fno-pic "
                                "-mcmodel=small, as sdk/rmp/plugin.mk does)")
            if sy["shndx"] != img_idx:
                raise PackError(f"relative relocation type {typ} at {off:#x} targets a symbol outside .image "
                                f"({sy['name']!r}): it would break when the image moves")
            to_data = sy["value"] + add >= data_start
            if in_data:
                raise PackError(f"a PC-relative relocation (type {typ}) in the writable part at {off:#x} "
                                "(data must not hold code-relative offsets)")
            if typ in ADRP and to_data:
                relocs.append((off, 4))   # RMP_REL_A64_ADRP_DATA: the page distance to the data part changes
            elif typ in LO12:
                continue
            elif to_data:
                raise PackError(f"relocation type {typ} at {off:#x} reaches the writable part PC-relatively "
                                f"({sy['name']!r}): only ADRP + :lo12: may (the parts are placed apart)")
    relocs.sort()
    body = bytes(image) + b"".join(struct.pack("<II", off | typ << 28, 0) for off, typ in relocs) + manifest
    body += b"\0" * (-len(body) % 4)
    head = struct.pack(HEADER_FMT, MAGIC_A64, package_format(man), api, man["id"].encode(), ver, compat,
                       len(image), bss, entry, len(relocs), len(manifest), header_flags(man), zlib.crc32(body) & 0xFFFFFFFF,
                       struct.pack("<I", data_start) + b"\0" * 16)   # reserved[0..3]: the writable part's offset
    return head + body

def pack(elf_path, manifest_path):
    with open(elf_path, "rb") as f:
        elf = f.read()
    try:
        with open(manifest_path) as f:
            man = json.load(f)
    except (OSError, ValueError) as e:
        raise PackError(f"manifest {manifest_path}: {e}")
    manifest, ver, compat = _manifest(man)
    try:
        if len(elf) >= 0x40 and elf[:4] == b"\x7fELF" and elf[4] == 2 and elf[5] == 1:
            return _build64(elf, man, manifest, ver, compat)
        return _build(elf, man, manifest, ver, compat)
    except (struct.error, IndexError, ValueError, UnicodeDecodeError, StopIteration) as e:
        raise PackError(f"malformed ELF: {type(e).__name__}: {e}")

def _build(elf, man, manifest, ver, compat):
    secs = _sections(elf)
    by = {s["name"]: s for s in secs}
    check_sections({n: s["size"] for n, s in by.items()})
    check_alloc_sections(secs)
    img = by.get(".image")
    if not img or img["addr"] != 0:
        raise PackError(".image missing or not at address 0 (link with sdk/rmp/plugin.ld)")
    if img["type"] != SHT_PROGBITS:
        raise PackError(".image is not PROGBITS")
    if img["align"] > MAX_ALIGN:
        raise PackError(f".image is aligned to {img['align']} B (an input section asks for it): the arena base is only "
                        f"{MAX_ALIGN}-byte aligned, so the loader could not honour it")
    img_idx = secs.index(img)
    syms = _symbols(elf, secs)
    und = [s["name"] for s in syms if s["shndx"] == 0 and s["name"]]
    if und:
        raise PackError("undefined symbols (a plugin may not use host symbols): " + ", ".join(sorted(set(und))))
    sym = {s["name"]: s["value"] for s in syms}
    for need in ("__rmp_bss_start", "__rmp_end", "rmp_entry"):
        if need not in sym:
            raise PackError(f"symbol {need} missing (link with sdk/rmp/plugin.ld and RMP_ENTRY)")
    bss_start, end = sym["__rmp_bss_start"], sym["__rmp_end"]
    if not 0 <= bss_start <= end or bss_start > img["size"]:
        raise PackError("bad __rmp_bss_start/__rmp_end")
    image = bytearray(elf[img["off"]:img["off"] + bss_start])
    bss = end - bss_start
    if len(image) + bss > MAX_IMAGE:
        raise PackError("image over 1 MB")
    entry = sym["rmp_entry"]
    if entry % 4:
        raise PackError(f"rmp_entry at {entry:#x} is not 4-byte aligned (the loader reads the entry table as words)")
    if entry + RMP_PLUGIN_BYTES > len(image):
        raise PackError(f"rmp_entry at {entry:#x} does not fit inside the image ({len(image)} B)")
    api, = struct.unpack_from("<I", image, entry + 4)   # the entry table's own api: the header says the same
    if not 1 <= api <= rmp_api():
        raise PackError(f"entry table api {api} is not 1..{rmp_api()} (sdk/rmp/rmp_api.h)")
    relocs, movs, seen = [], [], set()
    for rs in secs:
        if rs["type"] not in (SHT_REL, SHT_RELA) or secs[rs["info"]]["name"] != ".image":
            continue
        if rs["type"] == SHT_RELA:
            raise PackError("SHT_RELA relocations against .image are not supported (expected REL from arm-none-eabi)")
        for k in range(rs["size"] // 8):
            off, info = struct.unpack_from("<II", elf, rs["off"] + 8 * k)
            typ, sidx = info & 0xFF, info >> 8
            if off + 4 > len(image):
                raise PackError(f"relocation at {off:#x} outside the loaded image")
            if typ in (R_ARM_ABS32, R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS):
                if off in seen:
                    raise PackError(f"duplicate relocation at {off:#x}")
                seen.add(off)
            if typ == R_ARM_ABS32:
                if off % 4:
                    raise PackError(f"ABS32 relocation at {off:#x} is not 4-byte aligned")
                relocs.append((off, 0, 0))
            elif typ in (R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS):
                movs.append((off, typ, sidx))
            elif typ in PC_RELATIVE:
                if sidx >= len(syms) or syms[sidx]["shndx"] != img_idx:
                    raise PackError(f"PC-relative relocation type {typ} at {off:#x} targets a symbol outside .image "
                                    f"({syms[sidx]['name'] if sidx < len(syms) else sidx!r}): it would break when the image moves")
            elif typ == 0:
                continue
            else:
                raise PackError(f"unsupported relocation type {typ} at {off:#x} (build with -mword-relocations)")
    relocs += pair_movs(movs, image, syms)
    body = bytes(image) + b"".join(struct.pack("<II", off | typ << 28, val) for off, typ, val in relocs) + manifest
    body += b"\0" * (-len(body) % 4)
    if package_format(man) > 3:
        raise PackError("manifest type_labels.follows: an RMPA (Pi) package only (format 4; the Teensy shows no "
                        "labels that follow a knob)")
    head = struct.pack(HEADER_FMT, 0x31504D52, package_format(man), api, man["id"].encode(), ver, compat,
                       len(image), bss, entry, len(relocs), len(manifest), header_flags(man), zlib.crc32(body) & 0xFFFFFFFF, b"\0" * 20)
    return head + body

def _ver(s):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", s) if isinstance(s, str) else None
    if not m or any(int(x) > 255 for x in m.groups()):
        raise PackError(f"bad version {s!r}: want major.minor.patch, each 0..255")
    a, b, c = (int(x) for x in m.groups())
    return a << 16 | b << 8 | c

TOP_KEYS = {"id", "version", "compatible_from", "name", "short", "licence", "page2", "knobs", "page2_title", "preset_ext",
            "max_voices", "pitch", "type_labels"}
# seqx (E0): the header's flags (rmp_format.h RMP_HF_*): "max_voices" 2..8 opts the machine into instancing (the host
# runs that many states of it on one voice: a poly track, a unison stack - its state must hold everything render()
# writes); "pitch" "note" (default: note_on's note is the pitch) or "none" (a fixed-pitch drum: the host sends 60).
# Absent: 0, as every older package (mono, the note as the pitch); an older host ignores the flags.
RMP_HF_INSTANCES, RMP_HF_VOICES_SHIFT, RMP_HF_PITCH_SHIFT, RMP_PITCH_NONE = 0x1, 4, 8, 2

def header_flags(m):
    f = 0
    mv = m.get("max_voices", 1)
    if mv > 1:
        f |= RMP_HF_INSTANCES | (mv - 1) << RMP_HF_VOICES_SHIFT
    if m.get("pitch", "note") == "none":
        f |= RMP_PITCH_NONE << RMP_HF_PITCH_SHIFT
    return f
KNOB_KEYS = {"p", "long", "short", "look", "text", "names", "role", "min", "max", "default"}
ENUM_NAME_BYTES, ENUM_MAX = 8, 16   # = RMP_ENUM_NAME_BYTES, RMP_ENUM_MAX in rmp_format.h
KNOB1_BYTES, KNOB2_BYTES, KNOB_BYTES = 20, 22, 26   # a knob in formats 1, 2, 3 (rmp_knob, rmp_knob3)
HEAD2_BYTES, HEAD_BYTES = 64, 84                    # the manifest head in formats 1/2, 3 (rmp_manifest_head3)
FORMAT = 4                          # = RMP_FORMAT, the newest; parse_manifest reads formats 1..4
# Format 4 (2026-10-27, review of plugins/mdkit's MD) is format 3's manifest with an RLB2 block ("type_labels" with
# "follows"): a host that does not read RLB2 would show the first list's names for every value of "follows", so the
# package says it needs one (an older host refuses format 4). Every other package stays format 3, byte for byte.
def package_format(m):
    tl = m.get("type_labels")
    return 4 if isinstance(tl, dict) and "follows" in tl else 3

def _str(v, what, limit, required=True):
    if not isinstance(v, str):
        raise PackError(f"manifest {what}: must be a string")
    if required and not v:
        raise PackError(f"manifest {what}: must not be empty")
    if any(not 0x20 <= ord(c) <= 0x7e for c in v):
        raise PackError(f"manifest {what}: printable ASCII only ({v!r})")
    if len(v) > limit:
        raise PackError(f"manifest {what}: {v!r} is {len(v)} characters, the limit is {limit}")
    return v

def _id(v):
    """Same rule as rmp_id_ok in rmp_loader.cpp: the id becomes a file name on the Teensy."""
    _str(v, "id", 31)
    if any(c == " " or c in "/\\" for c in v):
        raise PackError(f"manifest id: {v!r} must not contain spaces, '/' or '\\'")
    if v.startswith("."):
        raise PackError(f"manifest id: {v!r} must not start with '.'")
    if ".." in v:
        raise PackError(f"manifest id: {v!r} must not contain '..'")

def _int7(v, what):
    if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 127:
        raise PackError(f"manifest {what}: {v!r} is not an integer 0..127")
    return v

def _knob_range(k, w):
    """(min, max, default) of a knob: defaults 0, 127 and 64 clamped into the range."""
    lo, hi = _int7(k.get("min", 0), f"{w}.min"), _int7(k.get("max", 127), f"{w}.max")
    if lo > hi:
        raise PackError(f"manifest {w}: min {lo} is above max {hi}")
    d = _int7(k.get("default", min(max(64, lo), hi)), f"{w}.default")
    if not lo <= d <= hi:
        raise PackError(f"manifest {w}.default: {d} is outside {lo}..{hi}")
    return lo, hi, d

def _manifest(m):
    """Validate the manifest dict; returns (binary manifest, version, compatible_from)."""
    if not isinstance(m, dict):
        raise PackError("manifest: top level must be an object")
    for k in ("id", "version", "name", "short"):
        if k not in m:
            raise PackError(f"manifest: missing required key {k!r}")
    bad = set(m) - TOP_KEYS
    if bad:
        raise PackError(f"manifest: unknown key(s) {sorted(bad)}")
    _id(m["id"]); _str(m["name"], "name", 10); _str(m["short"], "short", 4)
    _str(m.get("licence", ""), "licence", 48, required=False)
    ver = _ver(m["version"]); compat = _ver(m.get("compatible_from", m["version"]))
    if compat > ver:
        raise PackError("manifest: compatible_from is newer than version")
    mv = m.get("max_voices", 1)
    if isinstance(mv, bool) or not isinstance(mv, int) or not 1 <= mv <= 8:
        raise PackError(f"manifest max_voices: {mv!r} is not an integer 1..8")
    if m.get("pitch", "note") not in ("note", "none"):
        raise PackError(f"manifest pitch: {m.get('pitch')!r} is not 'note' or 'none'")
    page2 = m.get("page2", "sample")
    if page2 not in ("sample", "params"):
        raise PackError(f"manifest page2: {page2!r} is not 'sample' or 'params'")
    _str(m.get("page2_title", ""), "page2_title", 13, required=False)
    ext = m.get("preset_ext", "")
    if ext:
        _str(ext, "preset_ext", 7)
        if not re.fullmatch(r"[A-Za-z0-9]+", ext) or ext.lower() == "txt":
            raise PackError(f"manifest preset_ext: {ext!r} must be letters and digits, without the dot, "
                            "and not 'txt' (the generic presets' extension)")
    knobs = m.get("knobs", [])
    if not isinstance(knobs, list) or len(knobs) > 32:
        raise PackError("manifest knobs: must be a list of at most 32")
    ps, total, npre = set(), 0, 0
    for i, k in enumerate(knobs):
        w = f"knobs[{i}]"
        if not isinstance(k, dict):
            raise PackError(f"manifest {w}: must be an object")
        for r in ("p", "long", "short"):
            if r not in k:
                raise PackError(f"manifest {w}: missing required key {r!r}")
        bad = set(k) - KNOB_KEYS
        if bad:
            raise PackError(f"manifest {w}: unknown key(s) {sorted(bad)}")
        p = k["p"]
        if isinstance(p, bool) or not isinstance(p, int) or not 1 <= p <= 16:
            raise PackError(f"manifest {w}.p: {p!r} is not an integer 1..16")
        if p in ps:
            raise PackError(f"manifest {w}.p: P{p} used twice")
        ps.add(p)
        _str(k["long"], f"{w}.long", 12); _str(k["short"], f"{w}.short", 4)
        if k.get("look", "dial") not in LOOK:
            raise PackError(f"manifest {w}.look: {k['look']!r} not in {sorted(LOOK)}")
        if k.get("text", "number") not in TEXT:
            raise PackError(f"manifest {w}.text: {k['text']!r} not in {sorted(TEXT)}")
        _knob_range(k, w)
        role = k.get("role", "free")
        if role not in ROLE:
            raise PackError(f"manifest {w}.role: {role!r} not in {sorted(ROLE)}")
        if k.get("text", "number") == "preset":
            npre += 1
            if role != "free":
                raise PackError(f"manifest {w}: the preset knob itself must have role 'free'")
        if "names" in k:
            names = k["names"]
            if k.get("look", "dial") != "list":
                raise PackError(f"manifest {w}.names: only for look 'list'")
            if not isinstance(names, list) or not 1 <= len(names) <= ENUM_MAX:
                raise PackError(f"manifest {w}.names: must be a list of 1..{ENUM_MAX} names")
            for j, n in enumerate(names):
                _str(n, f"{w}.names[{j}]", ENUM_NAME_BYTES - 1)
            total += len(names)
    if npre > 1:
        raise PackError("manifest: at most one knob with text 'preset'")
    if total > 255:
        raise PackError(f"manifest: {total} enum names in all, the limit is 255")
    return _manifest_bytes(m) + _labels_bytes(m), ver, compat

# Labels that follow a list knob (rmp_format.h RMP_LABELS_*; 2026-10-26, plugins/mdkit): "type_labels":
# {"knob": P of a list knob, "params": [P, ...] (1..8 of the manifest's other knobs), "sets": one per name of the list
# knob, each a list of len(params) entries [long, short] or null (unlabelled for that value)}. Appended to the manifest
# after the name table, footer last; an older host never reads it (the knobs' own labels are the fallback).
# A list whose names follow another list knob (RMP_LABELS_MAGIC2 "RLB2"; 2026-10-27, plugins/mdkit's MD): "type_labels"
# adds "follows": the P of the list knob that picks the names of "knob" (ENGINE picks the TYPE list), and "lists": one
# list of names per name of "follows" (1..16 each); "sets" then holds one set per name of all the lists together, in
# order (the first list's first). "knob"'s own static names must be lists[0]: what an older host shows.
LABELS_MAGIC, LABELS_MAGIC2, LABELS_PARAMS, LABELS_SETS1, LABELS_SETS, LABELS_DEPS, LABEL_BYTES = \
    0x31424C52, 0x32424C52, 8, 16, 32, 16, 16

def _labels(m):
    """Validate m["type_labels"]; returns (knob, params, sets, follows, lists) or None (follows, lists None: RLB1)."""
    tl = m.get("type_labels")
    if tl is None:
        return None
    if not isinstance(tl, dict) or not ({"knob", "params", "sets"} <= set(tl) <= {"knob", "params", "sets", "follows", "lists"}) \
            or ("follows" in tl) != ("lists" in tl):
        raise PackError("manifest type_labels: must be an object with exactly 'knob', 'params' and 'sets' "
                        "(and 'follows' with 'lists', or neither)")
    knobs = {k["p"]: k for k in m.get("knobs", [])}
    kp = tl["knob"]
    if kp not in knobs or knobs[kp].get("look", "dial") != "list" or not knobs[kp].get("names"):
        raise PackError(f"manifest type_labels.knob: {kp!r} is not a list knob with names")
    dp, lists = tl.get("follows"), tl.get("lists")
    if dp is not None:
        if dp == kp or dp not in knobs or knobs[dp].get("look", "dial") != "list" or not knobs[dp].get("names"):
            raise PackError(f"manifest type_labels.follows: {dp!r} is not another list knob with names")
        nd = len(knobs[dp]["names"])
        if not isinstance(lists, list) or len(lists) != nd:
            raise PackError(f"manifest type_labels.lists: must hold {nd} lists (one per name of P{dp})")
        for i, ln in enumerate(lists):
            if not isinstance(ln, list) or not 1 <= len(ln) <= ENUM_MAX:
                raise PackError(f"manifest type_labels.lists[{i}]: must be a list of 1..{ENUM_MAX} names")
            for j, n in enumerate(ln):
                _str(n, f"type_labels.lists[{i}][{j}]", ENUM_NAME_BYTES - 1)
        if lists[0] != knobs[kp]["names"]:
            raise PackError(f"manifest type_labels.lists[0]: must equal P{kp}'s names (what an older host shows)")
        n = sum(len(x) for x in lists)
        if n > LABELS_SETS:
            raise PackError(f"manifest type_labels.lists: {n} names in all, the limit is {LABELS_SETS}")
    ps = tl["params"]
    if not isinstance(ps, list) or not 1 <= len(ps) <= LABELS_PARAMS:
        raise PackError(f"manifest type_labels.params: must be a list of 1..{LABELS_PARAMS} knobs")
    for q in ps:
        if q not in knobs or q == kp or q == dp or ps.count(q) > 1:
            raise PackError(f"manifest type_labels.params: {q!r} must be one of the other knobs' p, once")
    sets = tl["sets"]
    if dp is None:
        n = len(knobs[kp]["names"])
        if not isinstance(sets, list) or len(sets) != n:
            raise PackError(f"manifest type_labels.sets: must hold {n} sets (one per name of P{kp})")
    elif not isinstance(sets, list) or len(sets) != n:
        raise PackError(f"manifest type_labels.sets: must hold {n} sets (one per name of the lists)")
    for i, row in enumerate(sets):
        if not isinstance(row, list) or len(row) != len(ps):
            raise PackError(f"manifest type_labels.sets[{i}]: must hold {len(ps)} entries (one per params)")
        for j, e in enumerate(row):
            if e is None:
                continue
            if not isinstance(e, list) or len(e) != 2:
                raise PackError(f"manifest type_labels.sets[{i}][{j}]: [long, short] or null")
            _str(e[0], f"type_labels.sets[{i}][{j}] long", 12); _str(e[1], f"type_labels.sets[{i}][{j}] short", 4)
    return kp, ps, sets, dp, lists

def _labels_bytes(m):
    tl = _labels(m)
    if tl is None:
        return b""
    kp, ps, sets, dp, lists = tl
    if dp is None:
        b = struct.pack("<BBBB", kp, len(sets), len(ps), 0) + bytes(ps)
    else:
        b = struct.pack("<BBBBB", kp, len(sets), len(ps), dp, len(lists)) + bytes(len(x) for x in lists) + bytes(ps)
        b += b"".join(_fixed(n, ENUM_NAME_BYTES) for x in lists for n in x)
    for row in sets:
        for e in row:
            b += b"\0" * LABEL_BYTES if e is None else _fixed(e[0], 12) + _fixed(e[1], 4)
    return b + struct.pack("<HI", len(b), LABELS_MAGIC if dp is None else LABELS_MAGIC2)

def parse_labels(rmp):
    """The type_labels block of a package ({"knob", "params", "sets"} as in the manifest, None for an entry that is
    all NUL; an RLB2 block adds "follows" and "lists"), or None."""
    h = parse_header(rmp)
    end = 96 + h["image_size"] + 8 * h["reloc_count"] + h["manifest_size"]
    if h["format"] < 3 or h["manifest_size"] < HEAD_BYTES + 10:
        return None
    n, magic = struct.unpack_from("<HI", rmp, end - 6)
    if magic not in (LABELS_MAGIC, LABELS_MAGIC2):
        return None
    at = end - 6 - n
    clean = lambda b: b.rstrip(b"\0").decode()
    if magic == LABELS_MAGIC:
        kp, ns, npar, _ = struct.unpack_from("<BBBB", rmp, at)
        q = at + 4
        out = dict(knob=kp)
    else:
        kp, ns, npar, dp, nd = struct.unpack_from("<BBBBB", rmp, at)
        counts = list(rmp[at + 5:at + 5 + nd])
        q = at + 5 + nd
        out = dict(knob=kp, follows=dp)
    ps = list(rmp[q:q + npar])
    q += npar
    if magic == LABELS_MAGIC2:
        names = [clean(rmp[q + 8 * i:q + 8 * i + 8]) for i in range(ns)]
        q += 8 * ns
        out["lists"], i = [], 0
        for c in counts:
            out["lists"].append(names[i:i + c]); i += c
    sets = []
    for _ in range(ns):
        row = []
        for _ in range(npar):
            e = rmp[q:q + LABEL_BYTES]; q += LABEL_BYTES
            row.append(None if not any(e) else [clean(e[:12]), clean(e[12:])])
        sets.append(row)
    out.update(params=ps, sets=sets)
    return out

def _fixed(s, n):
    return s.encode("ascii").ljust(n, b"\0")

def _manifest_bytes(m):
    knobs = m.get("knobs", [])
    out = _fixed(m["name"], 10) + _fixed(m["short"], 4) + _fixed(m.get("licence", ""), 48)
    out += struct.pack("<BB", 1 if m.get("page2") == "params" else 0, len(knobs))
    out += _fixed(m.get("page2_title", ""), 13) + _fixed(m.get("preset_ext", ""), 7)
    table = []
    for i, k in enumerate(knobs):
        names = k.get("names", [])
        lo, hi, d = _knob_range(k, f"knobs[{i}]")
        out += struct.pack("<10B", k["p"], LOOK[k.get("look", "dial")], TEXT[k.get("text", "number")],
                           len(table) if names else 0, len(names), ROLE[k.get("role", "free")], lo, hi, d, 0)
        out += _fixed(k["long"], 12) + _fixed(k["short"], 4)
        table += names
    return out + b"".join(_fixed(n, ENUM_NAME_BYTES) for n in table)

def parse_header(rmp):
    f = struct.unpack_from(HEADER_FMT, rmp, 0)[:len(HEADER_KEYS)]
    h = dict(zip(HEADER_KEYS, f)); h["id"] = h["id"].rstrip(b"\0").decode()
    return h

def parse_relocs(rmp):
    h = parse_header(rmp); base = 96 + h["image_size"]
    out = []
    for k in range(h["reloc_count"]):
        wt, val = struct.unpack_from("<II", rmp, base + 8 * k)
        out.append((wt & 0x0FFFFFFF, wt >> 28, val))
    return out

def parse_manifest(rmp):
    """The manifest of a package in formats 1..4 (1 and 2 with format 3's defaults; 4 reads as 3)."""
    h = parse_header(rmp); p = 96 + h["image_size"] + 8 * h["reloc_count"]
    fmt = h["format"]
    name, short, lic, page2, n = struct.unpack_from("<10s4s48sBB", rmp, p); p += HEAD2_BYTES
    title = ext = b""
    if fmt >= 3:
        title, ext = struct.unpack_from("<13s7s", rmp, p); p += HEAD_BYTES - HEAD2_BYTES
    clean = lambda b: b.rstrip(b"\0").decode()
    knobs = []
    for _ in range(n):
        role, lo, hi, d, first, count = 0, 0, 127, 64, 0, 0
        if fmt == 1:
            pp, look, text, _pad = struct.unpack_from("<BBBB", rmp, p)
            lng, sht = struct.unpack_from("<12s4s", rmp, p + 4); p += KNOB1_BYTES
        elif fmt == 2:
            pp, look, text, first, count, _res = struct.unpack_from("<BBBBBB", rmp, p)
            lng, sht = struct.unpack_from("<12s4s", rmp, p + 6); p += KNOB2_BYTES
        else:
            pp, look, text, first, count, role, lo, hi, d, _res = struct.unpack_from("<10B", rmp, p)
            lng, sht = struct.unpack_from("<12s4s", rmp, p + 10); p += KNOB_BYTES
        knobs.append(dict(p=pp, look=look, text=text, long=clean(lng), short=clean(sht), enum_first=first,
                          enum_count=count, role=role, min=lo, max=hi, default=d))
    for k in knobs:
        if k["enum_count"]:
            k["names"] = [clean(rmp[p + ENUM_NAME_BYTES * j:p + ENUM_NAME_BYTES * (j + 1)])
                          for j in range(k["enum_first"], k["enum_first"] + k["enum_count"])]
    return dict(name=clean(name), short=clean(short), licence=clean(lic), page2=page2, page2_title=clean(title),
                preset_ext=clean(ext), knobs=knobs)

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    try:
        data = pack(sys.argv[1], sys.argv[2])
    except PackError as e:
        sys.exit(f"rmp_pack: {e}")
    with open(sys.argv[3], "wb") as f:
        f.write(data)
    h = parse_header(data)
    print(f"{sys.argv[3]}: {h['id']} image {h['image_size']} B + bss {h['bss_size']} B, {h['reloc_count']} relocations")
