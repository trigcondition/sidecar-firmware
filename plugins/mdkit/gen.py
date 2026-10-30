#!/usr/bin/env python3
"""plugins/mdkit/gen.py - writes the machines' manifests (md.json: MD, the three in one; pi.json, efm.json, trx.json) from mdkit's own voice
tables (control names, defaults; third_party/mdkit, vendored) and the resampler's table (mdk_fir.h). The outputs
are committed; run it again only after the pinned mdkit commit or the names below change.
MIT, Copyright 2026 trigcondition and contributors.

    python3 gen.py
"""
import json, math, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VEND = os.path.join(HERE, "..", "..", "third_party", "mdkit")
VERSION = "1.0.0"
FAMILIES = {   # TYPE order (families.h)
    "pi": ["bd", "sd", "mt", "ml", "ma", "rs", "rc", "cc", "hh"],
    "efm": ["bd", "sd", "xt", "cp", "rs", "cb", "hh", "cy"],
    "trx": ["bd", "sd", "xt", "cp", "rs", "cb", "ch", "oh", "cy", "ma", "cl", "xc", "b2"],
}
NAME = {"pi": ("MD-PI", "MDPI"), "efm": ("MD-EFM", "MDEF"), "trx": ("MD-TRX", "MDTX")}
# mdkit names each control with 2..4 letters (its short label here); the long label (12 at most) in words. Where the
# meaning is not clear from mdkit's source the letters stay (AG, AU, CLSN).
LONG = {"PTCH": "Pitch", "DEC": "Decay", "RAMP": "Ramp", "HOLD": "Hold", "TICK": "Tick", "NOIS": "Noise",
        "DIRT": "Dirt", "DIST": "Distortion", "RDEC": "Ramp Decay", "STRT": "Start", "HARM": "Harmonics",
        "CLIP": "Clip", "ENH": "Enhance", "DAMP": "Damp", "TONE": "Tone", "BUMP": "Bump", "GAP": "Gap",
        "HPF": "Highpass", "LPF": "Lowpass", "MTAL": "Metal", "DUAL": "Dual", "TUNE": "Tune", "CLIC": "Click",
        "CLPY": "Claps", "HARD": "Hardness", "RICH": "Richness", "RATE": "Rate", "ROOM": "Room", "RSIZ": "Room Size",
        "RTUN": "Room Tune", "TOP": "Top", "TTUN": "Top Tune", "SIZE": "Size", "PEAK": "Peak", "ATT": "Attack",
        "SUS": "Sustain", "REV": "Reverse", "RATL": "Rattle", "RTYP": "Rattle Type", "BENV": "Bump Env",
        "SNAP": "Snap", "DTYP": "Dist Type", "MOD": "Mod", "MFRQ": "Mod Freq", "MDEC": "Mod Decay",
        "MFB": "Mod Feedback", "FB": "Feedback", "CLPS": "Claps", "CDEC": "Clap Decay", "TREM": "Tremolo",
        "TFRQ": "Trem Freq", "SNAR": "Snare", "SPTC": "Snare Pitch", "SDEC": "Snare Decay", "SMOD": "Snare Mod",
        "HAMR": "Hammer", "NDEC": "Noise Decay", "TENS": "Tension", "RING": "Ring", "AG": "AG", "AU": "AU", "BR": "Brightness",
        "GRAB": "Grab", "CLSN": "CLSN", "CLOS": "Closed", "GRNS": "Grains", "GLEN": "Grain Length",
        "RVOL": "Ring Level", "POS": "Position"}
LONG_PI = {"RDEC": "Ring Decay"}   # the PI family's RDEC is the ring's decay (PI-RS, PI-SD: RING RVOL RDEC)

DUMP = r'''
#include <stdio.h>
#include "mdkit.h"
%s
static const mdkit_voice *const all[] = {%s};
int main(void) {
  printf("[");
  for (unsigned i = 0; i < sizeof all / sizeof all[0]; ++i) {
    const mdkit_voice *v = all[i];
    printf("%%s{\"name\":\"%%s\",\"id\":%%u,\"state\":%%zu,\"controls\":[", i ? "," : "", v->name, v->machine_id, v->state_bytes);
    for (unsigned c = 0; c < v->control_count; ++c)
      printf("%%s[\"%%s\",%%u]", c ? "," : "", v->control_names[c], v->control_defaults ? v->control_defaults[c] : 64u);
    printf("]}");
  }
  printf("]\n");
  return 0;
}
'''


def voices():
    keys = [f"{f}_{v}" for f in FAMILIES for v in FAMILIES[f]]
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "dump.c")
        with open(src, "w") as fh:
            fh.write(DUMP % ("\n".join(f"extern const mdkit_voice mdkit_voice_{k};" for k in keys),
                             ",".join(f"&mdkit_voice_{k}" for k in keys)))
        cs = [os.path.join(VEND, "src", k.split("_")[0], k.split("_")[1], f"mdkit_{k}.c") for k in keys]
        incs = sum((["-I", os.path.dirname(c)] for c in cs), ["-I", os.path.join(VEND, "include")])
        exe = os.path.join(d, "dump")
        subprocess.run(["cc", "-std=c99", "-O1", *incs, src, *cs, "-o", exe], check=True)
        out = json.loads(subprocess.run([exe], check=True, capture_output=True, text=True).stdout)
    return dict(zip(keys, out))


def long_of(fam, short):
    return (LONG_PI if fam == "pi" else {}).get(short, LONG[short])


def manifest(fam, vs):
    rows = [vs[f"{fam}_{v}"] for v in FAMILIES[fam]]
    bd = rows[0]
    knobs = []
    for k in range(8):   # the static labels (an older host's): the family's most used name at P(k+1) not yet taken, BD's default
        names = [r["controls"][k][0] for r in rows if k < len(r["controls"])]
        used = {x["short"] for x in knobs}   # (each name once: the next most used when it is taken)
        best = max(sorted(set(names) - used) or sorted(set(names)), key=names.count)
        d = bd["controls"][k][1] if k < len(bd["controls"]) else 64
        knobs.append({"p": k + 1, "long": long_of(fam, best), "short": best, "default": d})
    knobs.append({"p": 9, "long": "Type", "short": "TYPE", "look": "list", "text": "enum", "default": 0,
                  "names": [v.upper() for v in FAMILIES[fam]]})
    knobs.append({"p": 10, "long": "Level", "short": "LEV", "look": "fader", "default": 100})
    sets = [[[long_of(fam, r["controls"][k][0]), r["controls"][k][0]] if k < len(r["controls"]) else None
             for k in range(8)] for r in rows]
    name, short = NAME[fam]
    return {"id": f"mdkit.{fam}", "version": VERSION, "name": name, "short": short,
            "licence": "CC0-1.0 mdkit voices (soejrd); glue MIT", "page2": "params", "page2_title": f"{name} TYPE",
            "max_voices": 1, "pitch": "none", "knobs": knobs,
            "type_labels": {"knob": 9, "params": list(range(1, 9)), "sets": sets}}


ENGINES = [("efm", "FM"), ("pi", "PI"), ("trx", "TRX")]   # MD's P9 ENGINE, in this order (families.h MDK_FAM_MD)


def manifest_md(vs):
    """MD: P9 ENGINE picks the family, P10 TYPE its voice (its names follow ENGINE: type_labels "follows", an RLB2
    block), P1..P8 the voice's controls (labels per ENGINE and TYPE), P11 LEV. The static labels and P10's own names
    (what an older host shows) are the first ENGINE's, as MD-EFM's."""
    first = manifest(ENGINES[0][0], vs)
    knobs = [dict(k) for k in first["knobs"][:8]]
    lists = [[v.upper() for v in FAMILIES[f]] for f, _ in ENGINES]
    knobs.append({"p": 9, "long": "Engine", "short": "ENG", "look": "list", "text": "enum", "default": 0,
                  "names": [n for _, n in ENGINES]})
    knobs.append({"p": 10, "long": "Type", "short": "TYPE", "look": "list", "text": "enum", "default": 0,
                  "names": lists[0]})
    knobs.append({"p": 11, "long": "Level", "short": "LEV", "look": "fader", "default": 100})
    sets = [s for f, _ in ENGINES for s in manifest(f, vs)["type_labels"]["sets"]]
    return {"id": "mdkit.md", "version": VERSION, "name": "MD", "short": "MD",
            "licence": "CC0-1.0 mdkit voices (soejrd); glue MIT", "page2": "params", "page2_title": "MD ENGINE",
            "max_voices": 1, "pitch": "none", "knobs": knobs,
            "type_labels": {"knob": 10, "follows": 9, "lists": lists, "params": list(range(1, 9)), "sets": sets}}


def i0(x):
    s, t, k = 1.0, 1.0, 1
    while t > 1e-12 * s:
        t *= (x / (2 * k)) ** 2; s += t; k += 1
    return s


PHASES, TAPS, CUT, BETA = 160, 32, 0.92, 8.0   # 44100 -> 48000 is 147/160; cutoff 0.92 x 22050 Hz, Kaiser beta 8


def fir():
    rows = []
    for ph in range(PHASES):
        f = ph / PHASES
        h = []
        for k in range(TAPS):
            d = k - (TAPS // 2 - 1) - f            # tap k is core sample base-15+k, the output at base + f
            x = math.pi * CUT * d
            s = CUT * (math.sin(x) / x if abs(x) > 1e-12 else 1.0)
            r = d / (TAPS // 2)
            w = i0(BETA * math.sqrt(max(0.0, 1 - r * r))) / i0(BETA)
            h.append(s * w)
        g = sum(h)
        rows.append([v / g for v in h])            # unity gain at DC in every phase
    out = ["// plugins/mdkit/mdk_fir.h - generated by gen.py: the 44.1 -> 48 kHz resampler's polyphase table, one row",
           f"// per output phase ph/{PHASES} of a core sample, {TAPS} taps (core samples base-{TAPS // 2 - 1} .. base+{TAPS // 2}),",
           f"// a Kaiser-windowed sinc (beta {BETA}, cutoff {CUT} x 22050 Hz), each row summing to 1.",
           "#pragma once", f"#define MDK_FIR_PHASES {PHASES}", f"#define MDK_FIR_TAPS {TAPS}",
           f"#define MDK_FIR_LEFT {TAPS // 2 - 1}",
           "static const float mdk_fir[MDK_FIR_PHASES][MDK_FIR_TAPS] = {"]
    for r in rows:
        out.append("  {" + ", ".join(f"{v:.9g}f" for v in r) + "},")
    out.append("};")
    return "\n".join(out) + "\n"


def main():
    if not os.path.isdir(VEND):
        sys.exit("gen.py: ./fetch.sh first")
    vs = voices()
    for fam in FAMILIES:
        with open(os.path.join(HERE, f"{fam}.json"), "w") as fh:
            json.dump(manifest(fam, vs), fh, indent=1)
            fh.write("\n")
    with open(os.path.join(HERE, "md.json"), "w") as fh:
        json.dump(manifest_md(vs), fh, indent=1)
        fh.write("\n")
    with open(os.path.join(HERE, "mdk_fir.h"), "w") as fh:
        fh.write(fir())
    with open(os.path.join(HERE, "voices.json"), "w") as fh:   # what the README's tables and the checks read
        json.dump({k: v for k, v in vs.items()}, fh, indent=1)
        fh.write("\n")
    print("md.json pi.json efm.json trx.json mdk_fir.h voices.json")


if __name__ == "__main__":
    main()
