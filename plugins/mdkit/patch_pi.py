#!/usr/bin/env python3
"""plugins/mdkit/patch_pi.py SRC_DIR OUT_DIR VOICE - PI-CC, PI-HH, PI-RC with their delay memory windowed.

mdkit gives these three a 65536-word delay memory (256 KB a voice; the card hosts allow 64 KB a voice), of which the
voice uses the 1024 words from 0x135600 (its four pages, page_a .. page_b + 0x100, each walked modulo 0x100; measured
over every control at 0 and 127 and 300 random settings with control changes mid-note: offsets 22016..23039 only).
The copy keeps a window of 4096 words, 0x135000..0x135FFF, around them: a read outside it answers 0 and a write is
dropped (what the full memory would hold there, as nothing outside the window is ever written). Arithmetic, timing
and every other line are mdkit's own; tests/mdk_check.cpp compares the result bit for bit with the unwindowed voice.
MIT, Copyright 2026 trigcondition and contributors (the voices: mdkit, CC0 1.0).
"""
import os, sys

src, out, v = sys.argv[1:4]
WIN0, WORDS = 0x135000, 4096
name = f"mdkit_pi_{v}"
with open(os.path.join(src, name + ".h")) as f:
    h = f.read()
with open(os.path.join(src, name + ".c")) as f:
    c = f.read()
edits = [
    ("h", "uint32_t work[1024], fraction[1024], delay[65536], block[32];",
          f"uint32_t work[1024], fraction[1024], delay[{WORDS}], block[32];   /* windowed: plugins/mdkit/patch_pi.py */"),
    ("c", " if(address>=0x130000&&address<0x140000)return s->delay[address-0x130000];",
          f" if(address>=0x{WIN0:x}&&address<0x{WIN0 + WORDS:x})return s->delay[address-0x{WIN0:x}];\n"
          f" if(address>=0x130000&&address<0x140000)return 0;"),
    ("c", " else if(address>=0x130000&&address<0x140000)s->delay[address-0x130000]=word;",
          f" else if(address>=0x{WIN0:x}&&address<0x{WIN0 + WORDS:x})s->delay[address-0x{WIN0:x}]=word;"),
    ("c", " for(unsigned i=0;i<65536u;++i)s->delay[i]=0;", f" for(unsigned i=0;i<{WORDS}u;++i)s->delay[i]=0;"),
]
for which, old, new in edits:
    t = h if which == "h" else c
    if t.count(old) != 1:
        sys.exit(f"patch_pi.py: {name}.{which}: expected one {old.strip()!r} (another mdkit commit?)")
    t = t.replace(old, new)
    if which == "h":
        h = t
    else:
        c = t
if c.count("s->delay[") != 3 or "delay[65536" in h or "65536u;++i)s->delay" in c:
    sys.exit(f"patch_pi.py: {name}.c: another use of the delay memory (another mdkit commit?)")
os.makedirs(out, exist_ok=True)
for ext, t in (("h", h), ("c", c)):
    with open(os.path.join(out, f"{name}.{ext}"), "w") as f:
        f.write(f"/* windowed copy of mdkit's {name}.{ext} (plugins/mdkit/patch_pi.py) */\n" + t)
