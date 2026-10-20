#!/usr/bin/env python3
"""pi/engine/tests/mock_web.py - the Sidecar page (pi/engine/www/index.html) against a stand-in for web.cpp, to try
the page in a browser without the Pi: [MOCK_REFUSE=0.3] python3 pi/engine/tests/mock_web.py [PORT] [CARD_DIR] (default 8765, a temp
folder seeded with the boot files, /Samples, /Plugins, /.rytm and some macOS dot-files).

/wifi/* as wifi.cpp and web.cpp: fake networks (Fake-*), a join that "restarts" (503 for 3 s) then tries for a few
seconds: the password "wrong-pass" fails as wrong_password, an SSID not in the scan as not_found, else it is up; a
failed one reverts (the previous settings back). join/forget/power need the same-origin headers.

/status, /log (with the periodic stats and display/touch lines), /um (six machines, two of them measured), /cmd
(logged) and /fs/* over CARD_DIR with fsweb.cpp's rules: the boot files and config.txt.bak read-only ("ro":1), the
kept folders /.rytm, /Samples, /Plugins ("ro":2), put via <path>.part with its CRC, mkdir?p=1. A sketch of the
rules for the page's tests, not their reference: make -C pi/engine/tests (test_fsweb) is."""
import gzip, json, os, re, struct, sys, tempfile, time, zlib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlsplit, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "www", "index.html")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
ROOT = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="sidecar-card-")
T0 = time.time()
import random
REFUSE = float(os.environ.get("MOCK_REFUSE", "0") or 0)
REFUSED = [0]
# the supply (power_state.cpp's json): MOCK_POWER=weak: a Pi 5 on a hub port (3 A mode, under-voltage since boot)
POWER = ({"known": True, "throttled": 0x50000, "uvNow": False, "uvBoot": True, "throttledNow": False, "throttledBoot": True,
          "cappedNow": False, "cappedBoot": False, "maxCurrent": 3000,
          "warn": "power supply too weak (no 5 A USB-PD: 3 A mode): use the 27 W (5 V 5 A) adapter directly"}
         if os.environ.get("MOCK_POWER") == "weak" else
         {"known": True, "throttled": 0, "uvNow": False, "uvBoot": False, "throttledNow": False, "throttledBoot": False,
          "cappedNow": False, "cappedBoot": False, "maxCurrent": 5000, "warn": None})
LOG = []
BOOT = {"config.txt", "config.txt.bak", "cmdline.txt", "bootcode.bin", "licence.broadcom", "issue.txt"}
BOOTP = ("sdboot", "engine.img", "kernel", "start", "fixup", "armstub", "tryboot.txt", "bcm27")
KEPT = {"/.rytm", "/samples", "/plugins"}
UP = {}
STATE = {"screensaver": 60, "brightness": 100, "notes": False, "out": "main", "mac": True, "drone": -1, "fifo": 96,
         "machines": ["PLAITS"] * 8}
IGNORE = os.environ.get("MOCK_IGNORE", "")   # e.g. "out,mac": those commands are accepted but change nothing
# IN L/R source (core::fxin, host_link.cpp): the setting and the Rytm's answer; MOCK_FXIN=old: a Rytm without 0030 (no answer)
FXIN = {"want": 0, "rytm": -1 if os.environ.get("MOCK_FXIN") == "old" else 0}


def fxin_status():
    st = "on" if FXIN["rytm"] == 1 else "off" if FXIN["rytm"] == 0 else "unsupported"
    return {"want": FXIN["want"], "state": st, "rytm": FXIN["rytm"]}
# the recorder (rec_pi.cpp's rec= commands and /status "rec"): a take's files are written (short WAVs, recorder.h's
# names and 512-byte header) when it stops
RECN = ["MASTER", "MAIN_L", "MAIN_R", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB", "IN_L", "IN_R"] + \
       ["VOICE%d" % v for v in range(1, 9)] + ["FX1", "FX2"]
REC = {"state": 0, "arm": 0, "bits": 24, "prefix": "take", "take": "", "ch": [0], "t0": 0, "frames": 0, "drops": 0}


# the LFOs (core/lfo.cpp command / json, the Pi's eight): the settings kept, /status "lfo" as the engine's
LFO_TG = ["OFF", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "FX1", "FX2"]
LFO_TRI = "".join(f"{(-64 + k * 16 if k < 8 else 64 - (k - 8) * 16) + 64 if k != 8 else 127:02x}1" for k in range(16))
LFOS = [dict(tgt="OFF", par=0, wav=0, rate=64, sync=0, trig=0, dep=32, base=64, des=LFO_TRI) for _ in range(8)]


def lfo_cmd(c):
    m = re.match(r"lfo=(\d):(\w+)(?::(.*))?$", c)
    o = LFOS[int(m.group(1)) - 1] if m and 1 <= int(m.group(1)) <= 8 else None
    if not o:
        return
    k, v = m.group(2), m.group(3) or ""
    if k == "off":
        o["tgt"] = "OFF"
    elif k == "tgt" and v.upper() in LFO_TG:
        if o["tgt"] != v.upper():
            o["par"] = 1 if LFO_TG.index(v.upper()) < 9 else 0
        o["tgt"] = v.upper()
    elif k == "sync" and v in ("0", "1"):
        if int(v) != o["sync"]:
            o["rate"] = 8 if v == "1" else 64
        o["sync"] = int(v)
    elif k in ("par", "wav", "rate", "trig", "dep", "base") and re.match(r"-?\d+$", v):
        o[k] = int(v)
    elif k == "des" and re.match(r"[0-9a-f]{48}$", v):
        o["des"] = v
    elif k == "pre" and v in ("ramp", "tri", "steps", "rnd"):
        o["wav"] = 7
        o["des"] = {"ramp": "".join(f"{round(j * 127 / 15):02x}1" for j in range(16)), "tri": LFO_TRI,
                    "steps": "".join(f"{16 + (j & 3) * 32:02x}0" for j in range(16)),
                    "rnd": "".join(f"{(j * 37) % 126 + 1:02x}2" for j in range(16))}[v]


def lfo_status():
    def np_(t):
        i = LFO_TG.index(t)
        return 0 if not i else 16 if i < 9 else 4
    return {"n": 8, "l": [dict(o, pn=f"P{o['par'] + 1}", np=np_(o["tgt"]), rn="",
                                       ph=int(time.time() * 1000 / (4 + i)) % 1000) for i, o in enumerate(LFOS)]}


def page():
    """index.html with its /*@include web/X.js@*/ lines replaced as build.sh does (the exports dropped)"""
    out = []
    for line in open(PAGE, encoding="utf-8").read().split("\n"):
        m = re.match(r"\s*/\*@include (\S+)@\*/\s*$", line)
        if m:
            src = open(os.path.join(HERE, "..", "..", "..", m.group(1)), encoding="utf-8").read()
            line = re.sub(r"^export ", "", src, flags=re.M)
        out.append(line)
    return "\n".join(out).encode()


def state_cmd(c):
    """cores.cpp Command: the Sound tab's commands change what /status reports (MOCK_IGNORE=out,mac: not these)"""
    k, _, v = c.partition("=")
    if k == "lfo":
        return lfo_cmd(c)
    if not _ or k in IGNORE.split(","):
        return
    if k == "notes":
        STATE["notes"] = v == "1"
    elif k == "out":
        STATE["out"] = "tracks" if v == "tracks" else "main"
    elif k == "mac":
        STATE["mac"] = v == "on"
    elif k == "drone":
        STATE["drone"] = 8 if v == "all" else -1 if v == "off" else int(v) if v.isdigit() and int(v) < 8 else -1
    elif k == "fifo" and v.isdigit():
        STATE["fifo"] = max(24, min(int(v), 992))
    elif k == "notemach":
        n, _, name = v.partition(":")
        if n.isdigit() and int(n) < 8 and name in ("plaits", "juno", "daisy", "dsps", "dd", "fm"):
            STATE["machines"][int(n)] = name.upper()


# the Wi-Fi settings (wifi.cpp's Settings JSON); only fake networks and passwords
WIFI = {"mode": "off", "chip": False, "on": False, "ssid": None, "open": False, "scans": 0, "scanAt": 0, "scanEnd": 0,
        "join": "none", "reason": "", "t0": 0, "outcome": None, "prev": None, "rebootUntil": 0}
WIFI_TRIAL = float(os.environ.get("MOCK_TRIAL", "6"))
FAKE_NETS = [{"ssid": "Fake-Studio", "rssi": -48, "ch": 6, "band": "2.4", "secured": True},
             {"ssid": "Fake-Studio 5G", "rssi": -55, "ch": 36, "band": "5", "secured": True},
             {"ssid": "Fake-Cafe (open)", "rssi": -71, "ch": 11, "band": "2.4", "secured": False},
             {"ssid": "Fake-Neighbour", "rssi": -83, "ch": 1, "band": "2.4", "secured": True}]


def wifi_tick():
    w, now = WIFI, time.time()
    if w["join"] == "restarting" and now >= w["rebootUntil"]:
        w.update(join="trying", t0=w["rebootUntil"], mode="net", chip=True)
    if w["join"] == "trying" and now - w["t0"] >= WIFI_TRIAL:
        if w["outcome"] is None:
            w.update(join="up")
            log("wifi: join: the new network is up in 5 s (wifi.txt kept, wifi.txt.prev stays as a backup)")
        else:
            prev = w["prev"]
            w.update(join="failed", reason=w["outcome"], ssid=prev["ssid"], open=prev["open"], on=prev["on"],
                     mode="net" if prev["on"] and prev["ssid"] else "scan" if prev["chip"] else "off", chip=prev["chip"])
            log("wifi: join: the new network did not come up (%s): the previous wifi.txt is back, restarting on it" % w["outcome"])


def wifi_status(host):
    w, now = WIFI, time.time()
    wifi_tick()
    net = w["mode"] == "net"
    up = not net or w["join"] in ("up", "none", "failed")
    return {"mode": w["mode"], "chip": w["chip"], "starting": False, "on": w["on"], "net": "wifi" if net else "ethernet",
            "up": up, "ip": "192.168.1.20" if net else "192.168.77.2", "cmdline": None, "ssid": w["ssid"], "open": w["open"],
            "connected": net and up, "scanning": now < w["scanEnd"], "scans": w["scans"],
            "scanAge": int(now - w["scanAt"]) if w["scans"] else -1, "trialSecs": int(WIFI_TRIAL),
            "join": {"state": w["join"], "reason": w["reason"],
                     "left": max(0, int(WIFI_TRIAL - (now - w["t0"]))) if w["join"] == "trying" else 0},
            "networks": FAKE_NETS if w["scans"] and now >= w["scanEnd"] else []}


def same_origin(h):   # web.cpp's FromPage: Host one of ours, Origin (else Referer) that host
    host = h.get("Host", "")
    if host.split(":")[0] not in ("localhost", "127.0.0.1", "sidecar.local"):
        return False
    o, ref = h.get("Origin"), h.get("Referer")
    if o:
        return o == "http://" + host
    return bool(ref) and (ref == "http://" + host or ref.startswith("http://" + host + "/"))


def wifi_post(op, form):
    w = WIFI
    busy = w["join"] in ("restarting", "trying")
    if op == "power":
        if form.get("on") not in ("0", "1"):
            return None
        if busy:
            return "busy: a join is being tried"
        w["on"] = form["on"] == "1"
        if w["on"] and not w["chip"]:
            w.update(chip=True, mode="scan")
        log("wifi: stored on=%s in /.rytm/wifi.txt (the next boot)" % form["on"])
        return ""
    if op == "forget":
        if busy:
            return "busy: a join is being tried"
        w.update(ssid=None, open=False, on=False, join="none", reason="")
        log("wifi: forget: the stored network removed from /.rytm/wifi.txt (and wifi.txt.prev), on=0")
        return ""
    if op == "join":
        if busy:
            return "busy: a join is being tried"
        ssid, psk, op_ = form.get("ssid", ""), form.get("psk", ""), form.get("open") == "1"
        b = ssid.encode()
        if not 1 <= len(b) <= 32 or any(c < 0x20 or c == 0x7f for c in b):
            return "the network name must be 1-32 bytes, no control characters"
        if op_ and psk:
            return "an open network takes no password"
        if not op_ and not (8 <= len(psk) <= 63 and all(0x20 <= ord(c) <= 0x7e for c in psk) or re.fullmatch(r"[0-9a-fA-F]{64}", psk)):
            return 'the password must be 8-63 characters (printable ASCII) or 64 hex digits; an open network needs "open network" ticked'
        names = [n["ssid"] for n in FAKE_NETS]
        out = "wrong_password" if psk == "wrong-pass" else None if ssid in names else "not_found"
        w.update(prev={k: w[k] for k in ("ssid", "open", "on", "chip")}, ssid=ssid, open=op_, on=True, join="restarting",
                 reason="", outcome=out, rebootUntil=time.time() + 3)
        log("wifi: join: a new network stored in /.rytm/wifi.txt (its key, not the passphrase; the previous file kept as "
            "wifi.txt.prev): restarting on Wi-Fi, the previous setting back if it is not up in %d s" % WIFI_TRIAL)
        return ""
    return None


def rec_wav(path, ch, bits, frames):
    align = ch * bits // 8
    data = frames * align
    h = b"RIFF" + struct.pack("<I", 504 + data) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, ch, 48000, 48000 * align, align, bits)
    h += b"JUNK" + struct.pack("<I", 460) + b"\0" * 460 + b"data" + struct.pack("<I", data)
    open(path, "wb").write(h + b"\0" * data)


def rec_cmd(v):
    if v in ("start", "arm") and REC["state"] == 0:
        REC.update(state=2 if v == "start" else 1, arm=int(v == "arm"), t0=time.time(), frames=0)
        d = os.path.join(ROOT, "Recordings")
        os.makedirs(d, exist_ok=True)
        n = 1 + max([int(m.group(1)) for m in (re.match(re.escape(REC["prefix"]) + r"_(\d+)$", x) for x in os.listdir(d)) if m] or [0])
        REC["take"] = "%s_%03d" % (REC["prefix"], n)
    elif v == "stop" and REC["state"]:
        if REC["state"] == 2:
            REC["frames"] = int((time.time() - REC["t0"]) * 48000)
            d = os.path.join(ROOT, "Recordings", REC["take"])
            os.makedirs(d, exist_ok=True)
            for i in REC["ch"]:
                rec_wav(os.path.join(d, "%02d_%s.wav" % (i, RECN[i])), 2 if i in (0, 21, 22) else 1, REC["bits"], min(REC["frames"], 96000))
        REC.update(state=0, arm=0)
    elif v.startswith("ch:") and REC["state"] == 0:
        REC["ch"] = [] if v[3:] == "none" else sorted(int(x) for x in v[3:].split(",") if x.isdigit() and int(x) < 23)
    elif v.startswith("bits:") and REC["state"] == 0 and v[5:] in ("16", "24"):
        REC["bits"] = int(v[5:])
    elif v.startswith("name:") and REC["state"] == 0 and re.match(r"^[A-Za-z0-9_-]{1,24}$", v[5:]):
        REC["prefix"] = v[5:]
    log("rec: %s" % v)


def rec_status():
    if REC["state"] == 1 and time.time() - REC["t0"] > 4:   # armed: "PLAY" after 4 s
        REC.update(state=2, t0=time.time())
    fr = int((time.time() - REC["t0"]) * 48000) if REC["state"] == 2 else REC["frames"]
    samp = sum(2 if i in (0, 21, 22) else 1 for i in REC["ch"])
    need = samp * REC["bits"] // 8 * 48000 // 1024
    import random
    return {"state": REC["state"], "arm": REC["arm"], "ready": 1, "bits": REC["bits"], "prefix": REC["prefix"], "take": REC["take"],
            "ch": REC["ch"], "frames": fr, "bytes": need * fr // 48000, "drops": 0, "silence": 0, "outShort": 0,
            "cardKBps": 21000 if fr else 0, "needKBps": need, "worstWriteMs": 38 if fr else 0, "writes": fr // 20000,
            "ringMs": 11000, "ringUsed": 3, "ringMax": 40, "ringSlots": 65536, "freeMB": 28000, "totalMB": 30436, "error": "",
            "peak": [random.randint(100, 995) if REC["state"] and i in REC["ch"] else 0 for i in range(23)]}


def log(m):
    t = time.time() - T0
    LOG.append("%02d:%02d:%05.2f %s\n" % (t // 3600, t // 60 % 60, t % 60, m))


def seed():
    for d in ("Samples/Kit", "Plugins", ".rytm/slots", "overlays", ".Spotlight-V100", ".fseventsd"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    for f, b in (("config.txt", b"arm_64bit=1\n"), ("config.txt.bak", b"arm_64bit=1\n"), ("config.txt.prev", b"x\n"),
                 ("cmdline.txt", b"x\n"), ("kernel8-rpi4.img", b"\0" * 1000), ("._config.txt", b"\0"),
                 ("Samples/Kit/kick.wav", b"\0" * 4000), ("Plugins/sc_acid.rmp", b"\0" * 9000)):
        p = os.path.join(ROOT, f)
        if not os.path.exists(p):
            open(p, "wb").write(b)


def path_ok(p):
    if not p or len(p) > 200 or p[0] != "/" or "//" in p:
        return False
    if any(ord(c) < 0x20 or ord(c) > 0x7e or c in '\\:*?"<>|' for c in p):
        return False
    return not ("/../" in p or p.endswith("/..") or "/./" in p or p.endswith("/."))


def read_only(p):
    if p in ("", "/"):
        return True
    lp = p.lower()
    if lp == "/overlays" or lp.startswith("/overlays/") or lp.startswith("/system volume information"):
        return True
    n = lp[1:]
    if "/" in n:
        return False
    return n in BOOT or n.startswith(BOOTP) or n.endswith((".dtb", ".elf", ".dat"))


def kept(p):
    return p.lower() in KEPT


def disk(p):
    return os.path.join(ROOT, p.lstrip("/"))


NAMES = [("sc.acid", "ACID", 120), ("sc.wavepad", "WAVEPAD", 300), ("sc.metal", "METAL", 150),
         ("sc.snare", "SNARE", 80), ("sc.pluck", "PLUCK", 60), ("sc.grain", "GRAIN", 400)]
# the current project's slots as core/user/um.cpp keeps them (map + notes, rmp_resolve.h): U1 from the project's
# sounds, U2 saved, U3 needs a machine not installed (unknown fingerprint), U4 inherited, U5 explicitly empty
SLOTS = {1: {"id": "sc.grain", "ver": "1.0.0", "src": "sounds", "updated": True},
         2: {"id": "sc.acid", "ver": "1.0.0", "src": "saved"},
         3: {"id": "", "ver": "", "src": "none", "unknown": True, "want_fp": 4711, "want_name": "WOBBLE"},
         4: {"id": "sc.metal", "ver": "1.0.0", "src": "inherited"},
         5: {"id": "", "ver": "", "src": "empty"}}


def um_cmd(c):
    """um=assign:<n>:<id> | um=clear:<n> | um=put:<n>:<id|->:<a.b.c>[:<name>] | um=empty, as um.cpp's command()."""
    ids = {i: (nm, "1.0.0") for i, nm, _ in NAMES}
    m = re.match(r"um=assign:([1-5]):(\S+)$", c)
    if m and m.group(2) in ids:
        SLOTS[int(m.group(1))] = {"id": m.group(2), "ver": "1.0.0", "src": "saved"}
    m = re.match(r"um=clear:([1-5])$", c)
    if m:
        SLOTS[int(m.group(1))] = {"id": "", "ver": "", "src": "empty"}
    m = re.match(r"um=put:([1-5]):([^:]+):(\d+\.\d+\.\d+)(?::(.{0,10}))?$", c)
    if m:
        n, id_ = int(m.group(1)), m.group(2)
        SLOTS[n] = ({"id": "", "ver": "", "src": "empty"} if id_ == "-" else
                    {"id": id_, "ver": m.group(3), "src": "imported", "want_name": m.group(4) or ""})
    if c == "um=empty":
        for n in SLOTS:
            SLOTS[n] = {"id": "", "ver": "", "src": "empty"}


def um():
    names = NAMES
    a = []
    for i, (id_, nm, cpu) in enumerate(names):
        played = nm in ("GRAIN", "ACID")
        a.append({"t": "plugin", "id": id_, "ver": "1.0.0", "size": 9000 + i * 1000, "api": 2, "image": 8000, "bss": 512,
                  "state": 4096, "cpu": cpu, "ns": 1830 + 900 * i if played else 0, "blocks": 52000 if played else 0,
                  "name": nm, "slots": [1] if nm == "GRAIN" else [2] if nm == "ACID" else [], "fit": True, "why": "",
                  "file": "/Plugins/%s.rmp" % id_.replace(".", "_"), "target": "a64", "fp": 1000 + i, "status": "ok"})
    fps = {id_: 1000 + i for i, (id_, _, _) in enumerate(names)}
    nms = {id_: nm for id_, nm, _ in names}
    for n in range(1, 6):
        x = SLOTS[n]
        have = x["id"] in nms
        st = "READY" if have else "MISSING" if x["id"] else "EMPTY"
        a.append({"t": "slot", "slot": n, "id": x["id"], "ver": x["ver"], "state": st,
                  "name": nms.get(x["id"], x["id"].split(".")[-1].upper()[:10] if x["id"] else ""),
                  "fp": fps.get(x["id"], 0) if have else 0, "ns": 6330 if n == 1 else 0, "blocks": 52000 if n == 1 else 0,
                  "src": x["src"], "updated": x.get("updated", False), "collision": False, "unknown": x.get("unknown", False),
                  "mixed": False, "want_fp": x.get("want_fp", fps.get(x["id"], 0)), "want_n": 0, "want_id": x["id"],
                  "want_ver": x["ver"], "want_name": x.get("want_name", ""),
                  "presets": 2 if have else 0, "pre_ok": True, "pre_ext": ""})
    a.append({"t": "plugins", "prints": "rytm", "resolves": 1, "fresh": False, "n": 6, "project": "AR PERC PATCH", "arena": [8 << 20, 60000], "state_limit": 65536,
              "block_ns": 250000, "listed": 6, "gen": 3, "dir": "/Plugins", "scanning": False})
    a += [{"t": "mem", "k": "code", "size": 8 << 20, "used": 60000}, {"t": "mem", "k": "state", "size": 1 << 20, "used": 65536},
          {"t": "mem", "k": "sd", "size": 32 << 30, "used": 2 << 30}]
    return a


# the pool and JUNO / FM lists (core/user/lists.h's /lists and its pool= juno= fm= commands, a sketch)
LISTS = {"project": "AR PERC PATCH", "card": True, "sram": [256 << 20, 3 << 20],
         "pool": [{"slot": 1, "path": "Drums/kick.wav", "name": "kick.wav", "ok": True},
                  {"slot": 3, "path": "gone.wav", "name": "gone.wav", "ok": False}], "npool": 3,
         "juno": {"total": 2, "list": [{"slot": 0, "name": "Brass", "used": True}, {"slot": 1, "name": "Strings", "used": True}]},
         "fm": {"total": 96, "rom": True, "list": []},
         "voices": [{"v": v, "m": ["JUNO", "FM", "DSPS", "U1"][v] if v < 4 else "PLAITS", "pat": 1 if v < 2 else -1} for v in range(8)]}


def lists_cmd(c):
    m = re.match(r"pool=(\d+):(.*)$", c)
    if m:
        slot, path = int(m.group(1)), m.group(2)
        L = [e for e in LISTS["pool"] if e["slot"] != slot]
        if path and path != "-":
            L.append({"slot": slot, "path": path, "name": path.split("/")[-1], "ok": os.path.exists(disk("/Samples/" + path))})
        LISTS["pool"] = sorted(L, key=lambda e: e["slot"])
        LISTS["npool"] = max([e["slot"] for e in L] or [0])
    m = re.match(r"(juno|fm)=(ren|del|swap|save):(\d+)(?::(.*))?$", c)
    if m:
        k, op, a, b = m.group(1), m.group(2), int(m.group(3)), m.group(4)
        L = LISTS[k]["list"]
        if op == "ren" and a < len(L):
            L[a]["name"] = b
        elif op == "del" and a < len(L):
            L[a].update(name="", used=False)
        elif op == "swap" and b is not None and max(a, int(b)) < len(L):
            L[a]["name"], L[int(b)]["name"] = L[int(b)]["name"], L[a]["name"]
        elif op == "save":
            L.append({"slot": len(L), "name": b or "saved", "used": True, "found": True})
        while L and not L[-1]["used"]:
            L.pop()
        LISTS[k]["total"] = len(L) or (96 if k == "fm" else 0)
        if k == "fm":
            LISTS["fm"]["rom"] = not L


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, body, ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def code(self, st, msg):
        self.send("%d %s\n" % (st, msg))

    def do_GET(self):
        self.route()

    def do_POST(self):
        self.route()

    def route(self):
        u = urlsplit(self.path)
        # MOCK_REFUSE=0.3: that share of the requests refused as the Pi's connection cap does (web.cpp CreateWorker):
        # half answered 503 + Retry-After, half closed unanswered (the browser's "NetworkError")
        if REFUSE and random.random() < REFUSE and u.path != "/":
            REFUSED[0] += 1
            if random.random() < 0.5:
                self.close_connection = True
                self.wfile.write(b"HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 9\r\n\r\n503 busy\n")
            else:
                self.close_connection = True
                try: self.connection.shutdown(2)
                except OSError: pass
            return
        q = {k: v[0] for k, v in parse_qs(u.query, keep_blank_values=True).items()}
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        p = u.path
        if time.time() < WIFI["rebootUntil"] and p != "/":       # a join's restart: the Pi is away
            self.close_connection = True
            self.wfile.write(b"HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 9\r\n\r\n503 busy\n")
            return
        if p.startswith("/wifi/"):
            op = p[6:]
            if op == "status":
                return self.send(json.dumps(wifi_status(self.headers.get("Host"))), "application/json")
            if op == "scan":
                if not WIFI["chip"]:
                    return self.send(json.dumps({"ok": False, "code": 409, "error": "Wi-Fi is off: turn it on to scan"}), "application/json")
                WIFI.update(scans=WIFI["scans"] + 1, scanAt=time.time() + 2, scanEnd=time.time() + 2)
                log("wifi: scan: %d networks in 2100 ms (their names on the Wi-Fi settings only)" % len(FAKE_NETS))
                return self.send(json.dumps({"ok": True}), "application/json")
            if op in ("join", "forget", "power"):
                if not same_origin(self.headers):
                    return self.send(json.dumps({"ok": False, "code": 403, "error": "refused: not from this page (the same-origin check)"}), "application/json")
                form = {k: v[0] for k, v in parse_qs(body.decode(), keep_blank_values=True).items()}   # the body only
                e = wifi_post(op, form)
                if e is None:
                    return self.send_error(400)
                return self.send(json.dumps({"ok": True} if e == "" else {"ok": False, "code": 400, "error": e}), "application/json")
            return self.send_error(404)
        if p in ("/", "/index.html"):           # as web.cpp: gzip, no-cache; ?raw plain
            if "raw" in u.query:
                return self.send(page(), "text/html; charset=utf-8")
            pg = page()
            body = gzip.compress(pg if isinstance(pg, bytes) else pg.encode(), 9, mtime=0)
            self.send_response(200)
            for k, v in (("Content-Type", "text/html; charset=utf-8"), ("Content-Encoding", "gzip"),
                         ("Vary", "Accept-Encoding"), ("Cache-Control", "no-cache"), ("Content-Length", str(len(body)))):
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)
            return
        if p == "/status":
            return self.send(json.dumps({
                "host": "sidecar", "uptime": int(time.time() - T0), "web": {"requests": 1, "maxus": 900, "commands": 0, "refused": REFUSED[0], "idle": 0, "open": 1},
                "power": POWER,
                "ip": "169.254.61.60", "link": "usb", "gadget": "sidecar", "sidecar": True, "mac": STATE["mac"], "streaming": True,
                "rytmMidi": True, "temp": 57, "out": STATE["out"], "notes": STATE["notes"], "drone": STATE["drone"], "fifoTarget": STATE["fifo"], "underruns": 0,
                "machines": STATE["machines"],
                "fx": [{"type": "PLATE", "post": 0, "send": [0, 20, 64, 127, 0, 0, 0, 90, 0, 0, 10, 0]},
                       {"type": "OFF", "post": 1, "send": [0] * 12}],
                "version": "0.1.0", "commit": "abc1234", "dirty": False, "build": "Oct 17 2026 12:00:00", "circle": "Circle 50.0", "screensaver": STATE["screensaver"],
                "brightness": STATE["brightness"], "display": True, "blank": False, "rec": rec_status(),
                "fxin": fxin_status(), "lfo": lfo_status(),
                "wifi": {"mode": WIFI["mode"], "chip": WIFI["chip"], "stored": bool(WIFI["ssid"]), "on": WIFI["on"],
                         "connected": WIFI["join"] == "up", "rejoins": 0, "scanning": False, "scans": WIFI["scans"],
                         "networks": len(FAKE_NETS) if WIFI["scans"] else 0, "join": WIFI["join"]}}), "application/json")
        if p == "/log":
            log("engine: engine: fifo min 78 max 96 target 96 under 0 | blocks 4000 max 29 us | cpu c1 0.3 c2 1.1 c3 1.6 % | pass max 30 us")
            if int(time.time()) % 10 == 0:
                log("display: 10 s: 30 frames, flush max 4 ms")
                log("touch: 10 s: 0 events")
            text = "".join(LOG)
            since = int(q.get("since", "0") or 0)
            data = text.encode()[since:]
            return self.send(("%d %d\n" % (since, since + len(data))).encode() + data)
        if p == "/cmd":
            c = parse_qs(body.decode()).get("c", [q.get("c", "")])[0]
            log("web: command :%s" % c)
            um_cmd(c)
            lists_cmd(c)
            if c.startswith("rec="):
                rec_cmd(c[4:])
            m = re.match(r"fxin=([01?])$", c)
            if m:
                if m.group(1) != "?":
                    FXIN["want"] = int(m.group(1))
                if FXIN["rytm"] >= 0 and m.group(1) != "?":
                    FXIN["rytm"] = FXIN["want"]
            m = re.match(r"(screensaver|brightness)=(\d+)$", c)
            if m:
                STATE[m.group(1)] = int(m.group(2))
            state_cmd(c)
            return self.send("ok\n")
        if p == "/um":
            return self.send(json.dumps(um()), "application/json")
        if p == "/lists":
            return self.send(json.dumps(LISTS), "application/json")
        if p.startswith("/fs/"):
            return self.fs(p[4:], q, body)
        self.send_error(404)

    def fs(self, op, q, body):
        path = q.get("path", "") or "/"
        if op == "ls":
            if not path_ok(path) or not os.path.isdir(disk(path)):
                return self.code(404, "no such folder")
            es = []
            for nm in sorted(os.listdir(disk(path))):
                full = (path if path != "/" else "") + "/" + nm
                d = os.path.isdir(disk(full))
                es.append({"n": nm, "d": int(d), "s": 0 if d else os.path.getsize(disk(full)),
                           "ro": 1 if read_only(full) else 2 if kept(full) else 0})
            return self.send(json.dumps({"path": path, "free": 30 << 30, "total": 32 << 30, "more": 0, "entries": es}),
                             "application/json")
        if not path_ok(path):
            return self.code(400, "bad path")
        if op == "get":
            off, ln = int(q.get("off", 0)), int(q.get("len", 65536))
            with open(disk(path), "rb") as f:
                f.seek(off)
                return self.send(f.read(min(ln, 65536)), "application/octet-stream")
        if op == "mkdir":
            if read_only(path):
                return self.code(400, "bad path")
            if q.get("p") == "1":
                acc, made = "", 0
                for part in path.strip("/").split("/"):
                    acc += "/" + part
                    if os.path.isfile(disk(acc)):
                        return self.code(409, "a file of that name is in the way")
                    if not os.path.isdir(disk(acc)):
                        os.mkdir(disk(acc))
                        made += 1
                return self.code(200, "made" if made else "exists")
            if os.path.exists(disk(path)):
                return self.code(409, "exists")
            os.mkdir(disk(path))
            return self.code(200, "made")
        if op == "del":
            if read_only(path):
                return self.code(403, "a boot file: read-only here")
            if kept(path):
                return self.code(403, "kept: the Sidecar's own folder (its contents can go)")
            try:
                os.rmdir(disk(path)) if os.path.isdir(disk(path)) else os.remove(disk(path))
            except OSError:
                return self.code(409, "the folder is not empty")
            return self.code(200, "deleted")
        if op == "mv":
            to = q.get("to", "")
            if not path_ok(to):
                return self.code(400, "bad path")
            if read_only(path) or read_only(to):
                return self.code(403, "a boot file: read-only here")
            if kept(path):
                return self.code(403, "kept: the Sidecar's own folder cannot be renamed")
            if os.path.exists(disk(to)):
                return self.code(409, "the new name exists")
            os.rename(disk(path), disk(to))
            return self.code(200, "renamed")
        if op == "put":
            if read_only(path):
                return self.code(403, "a boot file: read-only here (sdfiles installs it)")
            m = re.search(rb"\r\n\r\n", body)
            b = re.search(rb"^--(.+?)\r\n", body)
            data = body[m.end():body.rindex(b"\r\n--" + b.group(1))] if m and b else b""
            off, total, crc = int(q.get("off", 0)), int(q.get("total", 0)), int(q.get("crc", 0))
            if off == 0:
                if not os.path.isdir(os.path.dirname(disk(path))):
                    return self.code(404, "its folder does not exist")
                UP[path] = [0, 0]
                open(disk(path) + ".part", "wb").close()
            u = UP.get(path)
            if not u or u[0] != off:
                return self.code(409, "expected %d" % (u[0] if u else 0))
            with open(disk(path) + ".part", "r+b") as f:
                f.seek(off)
                f.write(data)
            u[0] += len(data)
            u[1] = zlib.crc32(data, u[1])
            time.sleep(0.05)   # a card's pace, so the progress shows
            if u[0] < total:
                return self.code(200, "ok %d" % u[0])
            del UP[path]
            if u[1] != crc:
                os.remove(disk(path) + ".part")
                return self.code(400, "CRC of the upload wrong: not saved (try again)")
            os.replace(disk(path) + ".part", disk(path))
            return self.code(200, "done %d" % total)
        self.send_error(404)


if __name__ == "__main__":
    seed()
    print("http://127.0.0.1:%d/  card %s" % (PORT, ROOT), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
