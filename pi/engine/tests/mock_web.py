#!/usr/bin/env python3
"""pi/engine/tests/mock_web.py - the Sidecar page (pi/engine/www/index.html) against a stand-in for web.cpp, to try
the page in a browser without the Pi: [MOCK_REFUSE=0.3] python3 pi/engine/tests/mock_web.py [PORT] [CARD_DIR] (default 8765, a temp
folder seeded with the boot files, /Samples, /Plugins, /.rytm and some macOS dot-files).

/wifi/* as wifi.cpp and web.cpp: fake networks (Fake-*), a join that "restarts" (503 for 3 s) then tries for a few
seconds: the password "wrong-pass" fails as wrong_password, an SSID not in the scan as not_found, else it is up; a
failed one reverts (the previous settings back). join/forget/power need the same-origin headers.

The pairing code (pi/engine/paircode.h) as web.cpp gates it: /pair/status, /pair, /pair/resume, /pair/forget,
/pair/revoke, /pair/show, the same list of gated endpoints, the token from the X-Sidecar-Auth header (or tok= in a
POST's body - never the query), the "401 not paired" answers with the X-Sidecar-Pair header, and the devices remembered in
<CARD_DIR>/.rytm/pair.txt so a restart of this mock is a reboot of the Pi. The code is random per run and written to
<CARD_DIR>/sidecar-paircode.txt, as the engine does when no touchscreen is attached (there is none here); a test reads
it from there. MOCK_PAIR=0 is cmdline.txt paircode=0 (the gate off), MOCK_PAIR=<8 digits> a fixed code.
MOCK_PAIR_LOCK / MOCK_PAIR_GAP (seconds, default 60 and 1 as paircode.h) shorten the lockout for a test. The limiter
is per peer as paircode.h's (its global backstop is not mirrored); a test plays a second address with the mock-only
header X-Mock-Peer.
A sketch for the page's tests; make -C pi/engine/tests (test_paircode) is the reference.

/status, /log (with the periodic stats and display/touch lines), /um (six machines, two of them measured), /pool and
the pool=smp|wt|inst:... lines (the POOL, answers in the log), /cmd (logged) and /fs/* over CARD_DIR with fsweb.cpp's rules: the boot files and config.txt.bak read-only ("ro":1), the
kept folders /.rytm, /Samples, /Plugins ("ro":2), put via <path>.part with its CRC, mkdir?p=1. A sketch of the
rules for the page's tests, not their reference: make -C pi/engine/tests (test_fsweb) is."""
import gzip, json, math, os, re, struct, sys, tempfile, time, zlib
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
BUNDLE = {"job": "none", "phase": "IDLE", "done": 0, "total": 0, "msg": "", "key": "", "t": 0}
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
REC = {"state": 0, "arm": 0, "bits": 24, "prefix": "take", "take": "", "ch": [0], "t0": 0, "frames": 0, "drops": 0,
       "vol": "SD", "error": ""}   # vol: rec=vol: (a drive's takes in ROOT/<VOL>/Recordings: /USB/Recordings through /fs)


def rec_dir(vol):
    return os.path.join(ROOT, "Recordings") if vol == "SD" else os.path.join(ROOT, vol, "Recordings")


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
        if n.isdigit() and int(n) < 8 and name in ("plaits", "juno", "daisy", "dsps", "md", "fm"):
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


def not_cross_site(h):   # wifi_conf.h: lenient, for tools that send neither header
    return True if not h.get("Origin") and not h.get("Referer") else same_origin(h)


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
        vol = next((x for x in STOR["vols"] if x["name"] == REC["vol"]), None)
        if REC["vol"] != "SD" and not (vol and vol["state"] == "mounted"):   # never the SD instead
            REC["error"] = "%s drive not mounted" % REC["vol"]
            log("rec: rec=%s: %s" % (v, REC["error"]))
            return False
        REC.update(state=2 if v == "start" else 1, arm=int(v == "arm"), t0=time.time(), frames=0, error="")
        n = 1
        for d in {rec_dir("SD"), rec_dir(REC["vol"])}:   # one name on either volume
            os.makedirs(d, exist_ok=True)
            n = max([n] + [1 + int(m.group(1)) for m in (re.match(re.escape(REC["prefix"]) + r"_(\d+)$", x) for x in os.listdir(d)) if m])
        REC["take"] = "%s_%03d" % (REC["prefix"], n)
    elif v == "stop" and REC["state"]:
        if REC["state"] == 2:
            REC["frames"] = int((time.time() - REC["t0"]) * 48000)
            d = os.path.join(rec_dir(REC["vol"]), REC["take"])
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
    elif v.startswith("vol:"):
        if REC["state"] != 0 or v[4:].upper() not in ("SD", "USB", "USB2", "USB3", "NVME"):
            log("rec: rec=%s: refused" % v)
            return False
        REC["vol"] = v[4:].upper()
    log("rec: %s" % v)
    return True


# the volumes and the storage bench (stor_pi.cpp's /status "stor"): an SD card, a USB 3 stick; stor=bench:VOL runs a
# pretend bench (done after ~5 s), stor=eject:USB ejects the stick, stor=mount:USB brings it back
STOR = {"exfat": False, "vols": [
    {"name": "SD", "state": "mounted", "fs": "FAT32", "totalMB": 30436, "freeMB": 24310, "link": "SD HS 4-bit 50 MHz", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0},
    {"name": "USB", "state": "mounted", "fs": "FAT32", "totalMB": 61056, "freeMB": 58982, "link": "USB 3 SuperSpeed", "model": "0781:5581", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 41},
    {"name": "USB2", "state": "absent", "fs": "", "totalMB": 0, "freeMB": 0, "link": "", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0},
    {"name": "USB3", "state": "absent", "fs": "", "totalMB": 0, "freeMB": 0, "link": "", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0},
    {"name": "NVME", "state": "unavailable", "fs": "", "totalMB": 0, "freeMB": 0, "link": "", "model": "", "why": "", "err": 0, "busy": False, "pulls": 0, "mountMs": 0}],
    "bench": {"state": "idle", "progress": 0, "phase": "", "vol": "SD", "have": False}}
STOR_RUN = {"vol": None, "t0": 0}
STOR_TABLE = ("stor bench USB: 256 MB file, FAT32, USB 3 SuperSpeed, calls <= 64 KB\n"
              "(a pretend result from tests/mock_web.py)\nverify ok, 5 s\n")


def stor_command(c):
    v = c.split(":", 1)[1].rstrip("!") if ":" in c else "SD"
    vol = next((x for x in STOR["vols"] if x["name"] == v.split(":")[0].upper()), None)
    if c.startswith("stor=bench") and vol and vol["state"] == "mounted" and STOR["bench"]["state"] != "running":
        STOR_RUN.update(vol=vol["name"], t0=time.time())
    elif c == "stor=cancel" and STOR["bench"]["state"] == "running":
        STOR_RUN["vol"] = None
        STOR["bench"].update(state="cancelled", phase="cancelled", progress=0)
    elif c.startswith("stor=eject:") and vol and vol["name"] != "SD" and vol["state"] == "mounted":
        vol.update(state="ejected", fs="", totalMB=0, freeMB=0)
    elif c.startswith("stor=mount:") and vol and vol["state"] == "ejected":
        vol.update(state="mounted", fs="FAT32", totalMB=61056, freeMB=58982)


def stor_status():
    if STOR_RUN["vol"]:
        p = int((time.time() - STOR_RUN["t0"]) * 20)
        if p >= 100:
            STOR["bench"].update(state="done", progress=100, phase="done", vol=STOR_RUN["vol"], have=True, table=STOR_TABLE)
            STOR_RUN["vol"] = None
        else:
            STOR["bench"].update(state="running", progress=p, phase="sequential write", vol=STOR_RUN["vol"])
    return STOR


def rec_status():
    if REC["state"] == 1 and time.time() - REC["t0"] > 4:   # armed: "PLAY" after 4 s
        REC.update(state=2, t0=time.time())
    fr = int((time.time() - REC["t0"]) * 48000) if REC["state"] == 2 else REC["frames"]
    samp = sum(2 if i in (0, 21, 22) else 1 for i in REC["ch"])
    need = samp * REC["bits"] // 8 * 48000 // 1024
    import random
    take = REC["take"] if REC["vol"] == "SD" or not REC["take"] else REC["vol"] + ":" + REC["take"]   # the id
    return {"state": REC["state"], "arm": REC["arm"], "ready": 1, "bits": REC["bits"], "prefix": REC["prefix"], "take": take,
            "vol": REC["vol"], "abandoned": 0, "freeKnown": 1,
            "volOk": int(REC["vol"] == "SD" or any(x["name"] == REC["vol"] and x["state"] == "mounted" for x in STOR["vols"])),
            "ch": REC["ch"], "frames": fr, "bytes": need * fr // 48000, "drops": 0, "silence": 0, "outShort": 0,
            "cardKBps": 21000 if fr else 0, "needKBps": need, "worstWriteMs": 38 if fr else 0, "writes": fr // 20000,
            "ringMs": 11000, "ringUsed": 3, "ringMax": 40, "ringSlots": 65536, "freeMB": 28000, "totalMB": 30436, "error": REC["error"],
            "speedKBps": 5120, "speedMeasured": 1, "loadPct": need * 100 // 5120, "warnPct": 70,   # rec_pi.cpp Status (5d)
            "peak": [random.randint(100, 995) if REC["state"] and i in REC["ch"] else 0 for i in range(23)]}


def log(m):
    t = time.time() - T0
    LOG.append("%02d:%02d:%05.2f %s\n" % (t // 3600, t // 60 % 60, t % 60, m))


def seed():
    for d in ("Samples/Kit", "Samples/Loops", "MULTISAMPLE/Piano", "WAVETABLES", "Plugins", ".rytm/slots", "overlays", ".Spotlight-V100", ".fseventsd"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    for f, b in (("config.txt", b"arm_64bit=1\n"), ("config.txt.bak", b"arm_64bit=1\n"), ("config.txt.prev", b"x\n"),
                 ("cmdline.txt", b"x\n"), ("kernel8-rpi4.img", b"\0" * 1000), ("._config.txt", b"\0"),
                 ("Samples/Kit/kick.wav", b"\0" * 4000), ("Samples/Kit/snare.wav", b"\0" * 8000), ("Samples/Kit/tom.wav", b"\0" * 6000),
                 ("Samples/Loops/loop 2.wav", b"\0" * 9000), ("Samples/Loops/loop 1.wav", b"\0" * 9000),
                 ("Samples/Kit/hat open.wav", b"\0" * 3000), ("Samples/Kit/clap.aif", b"\0" * 5000),
                 ("MULTISAMPLE/Piano/Piano.sfz", b"<region> sample=c3.wav\n"), ("MULTISAMPLE/Piano/c3.wav", b"\0" * 2000), ("Plugins/sc_acid.rmp", b"\0" * 9000)):
        p = os.path.join(ROOT, f)
        if not os.path.exists(p):
            open(p, "wb").write(b)


# fsweb.cpp's reachable(): /fs/* is content, 7310 is firmware, /.rytm is neither. Deny by default.
FS_CONTENT = ("/Bundles", "/CHOP", "/DX7", "/GRANULAR", "/JUNO", "/MACHINES", "/MULTISAMPLE", "/NAM", "/PRESETS",
              "/Plugins", "/Recordings", "/Samples", "/WAVETABLES")


def fs_reachable(p):
    if not path_ok(p):
        return False
    if p == "/":
        return True
    m = re.match(r"^/(USB|USB2|USB3|NVME)(/.*)?$", p)   # a drive: its /Recordings only (fsweb.cpp drive_of)
    if m:
        r = (m.group(2) or "/").lower()
        return r == "/recordings" or r.startswith("/recordings/")
    low = p.lower()
    return any(low == c.lower() or low.startswith(c.lower() + "/") for c in FS_CONTENT)


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


# the project's POOL (core/lib/pools.h; GET /pool as poolweb.cpp, pool=smp|wt|inst:... as pools::command, its answer
# logged as cores.cpp does): a sketch for the Pool tab and the Files tab's +SMP / +WT / +INST / Fill bank, not their
# reference (core/tests/test_pools.cpp and tests/test_poolweb.cpp are). MOCK_PLAYING=1 or :mock=playing:1: the Rytm plays.
POOL_BANKS = "ABCDEFGH"
POOL_WARN = "NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT"
POOL_INPUTS = ["IN", "MAIN", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB"]
POOL_PLAY = [os.environ.get("MOCK_PLAYING") == "1"]


def pool_row(c):
    return "%s%03d" % (POOL_BANKS[c >> 7 & 7], c & 127)


def pool_code(t):
    m = re.match(r"([A-Ha-h])(\d{3})$", t or "")
    return (POOL_BANKS.index(m.group(1).upper()) * 128 + int(m.group(2))) if m and int(m.group(2)) < 128 else None


def pool_e(t, name, state="EXACT", path="", used=0, users=0, size=0, members=0):
    return {"type": t, "name": name, "state": state, "path": path, "used": used, "users": users, "size": size, "members": members}


def pool_seed():
    smp = {7 * 128 + i + 1: pool_e("input", n, "INPUT") for i, n in enumerate(POOL_INPUTS)}
    smp[7 * 128 + 11] = pool_e("builtin", "PAD", "BUILT-IN")
    smp[7 * 128 + 12] = pool_e("builtin", "BELLS", "BUILT-IN")
    smp[1] = pool_e("sample", "kick", "EXACT", "/Samples/Kit/kick.wav", 37, 2, 4000)
    smp[2] = pool_e("sample", "snare", "MOVED", "/Samples/Kit/snare.wav", 12, 0, 88244)
    smp[3] = pool_e("sample", "gone", "MISSING", "/Samples/old/gone.wav", 3, 0, 52000)
    smp[4] = pool_e("sample", "hat open", "CHANGED", "/Samples/Kit/hat open.wav", 0, 0, 30200)
    smp[5] = pool_e("sample", "riser", "UNVERIFIED", "/Samples/riser.wav", 0, 0, 1900000)
    smp[2 * 128 + 1] = pool_e("sample", "vox chop", "EXACT", "/Samples/vox chop.wav", 0, 0, 240000)
    wt = {i + 1: pool_e("builtin", n, "BUILT-IN") for i, n in enumerate(["BASIC", "PWM", "VOCAL", "ORGAN", "FM", "SYNC"])}
    wt[128 + 1] = pool_e("sample", "glass", "EXACT", "/WAVETABLES/glass.wav", 4, 0, 524288)
    inst = {1: pool_e("instrument", "Piano", "EXACT", "/MULTISAMPLE/Piano/Piano.sfz", 5, 1, 2100, 24)}
    return {"smp": smp, "wt": wt, "inst": inst}


POOL = pool_seed()
POOL_UNDO = {}
POOL_RAM = {"budget": 1 << 30, "used": 214 << 20, "tables": 6 << 20, "entries": 9, "pct": 20} if os.environ.get("MOCK_RAM") == "1" else \
    {"budget": 0, "used": 0, "tables": 0, "entries": 0, "pct": 0}


def pool_json(q):
    k = q.get("l") if q.get("l") in POOL else "smp"
    b = q.get("b", "0")
    b = int(b) if b.isdigit() and int(b) < 8 else POOL_BANKS.index(b.upper()) if len(b) == 1 and b.upper() in POOL_BANKS else 0
    L = POOL[k]
    rows = [[c & 127, e["type"], e["name"], e["state"], e["path"], e["used"], e["users"], e["size"], e["members"]]
            for c, e in sorted(L.items()) if c >> 7 == b]
    st = {"project": LISTS["project"], "ready": True}
    for t, l in POOL.items():
        st[t] = {"n": len(l), "locked": False, "ro": False, "why": ""}
    d = {"st": st, "ram": POOL_RAM, "bank": {"list": k, "bank": b, "locked": False, "count": len(L), "rows": rows}}
    if q.get("u") == "1":
        d["unused"] = [c for c, e in sorted(L.items()) if not e["used"] and not e["users"]]
        d["warn"] = POOL_WARN
    return d


def pool_exts(k):
    return (".sfz",) if k == "inst" else (".wav", ".aif", ".aiff")


def pool_cmd(c):
    m = re.match(r"pool=(smp|wt|inst):(.*)$", c)
    if not m:
        m2 = re.match(r"mock=playing:([01])$", c)
        if m2:
            POOL_PLAY[0] = m2.group(1) == "1"
        return
    k, a = m.group(1), m.group(2)
    L = POOL[k]
    ans = lambda row, what, res: log("cores: pool: %s %s %s: %s" % (k, row, what, res))

    def find(path):
        return next((c for c, e in L.items() if e["path"] and e["path"].lower() == path.lower()), None)

    def entry(path):
        n = os.path.basename(path)
        return pool_e("instrument" if k == "inst" else "sample", n.rsplit(".", 1)[0], "EXACT", path, 0, 0,
                      os.path.getsize(disk(path)), 12 if k == "inst" else 0)

    def guard(cd, conf):
        e = L.get(cd)
        if not e or not (e["used"] or e["users"]):
            return "SET"
        if POOL_PLAY[0] and conf != "force":
            return "PLAYING: STOP FIRST"
        return "SET" if conf in ("yes", "force") else "IN USE: REPLACE?"

    m = re.match(r"(set|replace):([A-Ha-h]\d{3}):(.*?)(?::(yes|force))?$", a)
    if m:
        cd, path, conf = pool_code(m.group(2)), m.group(3), m.group(4) or ""
        row = m.group(2).upper()
        if cd is None:
            return ans("-", path, "NO SUCH ROW")
        if not cd & 127:
            return ans(row, path, "000 IS OFF")
        if not path.lower().endswith(pool_exts(k)):
            return ans(row, path, "NOT FOR THIS POOL")
        if not os.path.isfile(disk(path)):
            return ans(row, path, "NO SUCH FILE")
        if L.get(cd, {}).get("type") in ("input", "builtin"):
            return ans(row, path, "BUILT-IN")
        ex = find(path)
        if ex is not None and ex != cd:
            return ans(pool_row(ex), path, "IN POOL")
        g = guard(cd, conf)
        if g != "SET":
            return ans(row, path, g)
        POOL_UNDO[k] = (cd, L.get(cd))
        old = L.get(cd) or {}
        L[cd] = dict(entry(path), used=old.get("used", 0), users=old.get("users", 0))   # the used set is the row's
        return ans(row, path, "READING" if k == "inst" else "SET")
    m = re.match(r"add:([A-Ha-h]):(.*)$", a)
    if m:
        b, path = POOL_BANKS.index(m.group(1).upper()), m.group(2)
        if not path.lower().endswith(pool_exts(k)):
            return ans("-", path, "NOT FOR THIS POOL")
        if not os.path.isfile(disk(path)):
            return ans("-", path, "NO SUCH FILE")
        ex = find(path)
        if ex is not None:
            return ans(pool_row(ex), path, "IN POOL")
        free = next((b * 128 + r for r in range(1, 128) if b * 128 + r not in L), None)
        if free is None:
            return ans("-", path, "BANK FULL")
        L[free] = entry(path)
        return ans("-" if k == "inst" else pool_row(free), path, "READING" if k == "inst" else "SET")
    m = re.match(r"(?:unload|clear):(.*)$", a)
    if m:
        done, last = 0, ""
        for part in m.group(1).split(","):
            r, _, conf = part.partition(":")
            cd = pool_code(r)
            if cd is None:
                break
            res = "000 IS OFF" if not cd & 127 else "EMPTY" if cd not in L else guard(cd, conf)
            if res == "SET":
                POOL_UNDO[k] = (cd, L.pop(cd))
                done += 1
            else:
                last = res
        return log("cores: pool: %s: %d unloaded%s" % (k, done, ", refused: " + last if last else ""))
    m = re.match(r"accept:([A-Ha-h]\d{3})$", a)
    if m:
        cd = pool_code(m.group(1))
        if cd in L and L[cd]["state"] == "CHANGED":
            L[cd]["state"] = "EXACT"
        return ans(m.group(1).upper(), L.get(cd, {}).get("state", "EXACT"), "SET")
    m = re.match(r"fill:([A-Ha-h]):(.*)$", a)
    if m:
        b, d = POOL_BANKS.index(m.group(1).upper()), m.group(2)
        if not os.path.isdir(disk(d)):
            return log("cores: pool: %s bank %s: cannot fill from %s" % (k, m.group(1).upper(), d))
        for n in sorted(os.listdir(disk(d))):
            p = d.rstrip("/") + "/" + n
            if n[0] == "." or not n.lower().endswith(pool_exts(k)) or find(p) is not None:
                continue
            free = next((b * 128 + r for r in range(1, 128) if b * 128 + r not in L), None)
            if free is None:
                break
            L[free] = entry(p)
        return log("cores: pool: %s bank %s: filling from %s" % (k, m.group(1).upper(), d))
    if a == "undo":
        u = POOL_UNDO.pop(k, None)
        if u:
            if u[1] is None:
                L.pop(u[0], None)
            else:
                L[u[0]] = u[1]
        return log("cores: pool: %s: %s" % (k, "undone" if u else "nothing to undo"))
    m = re.match(r"shown:([A-Ha-h])$", a)
    if m:
        return log("cores: pool: %s: bank %s shown" % (k, m.group(1).upper()))
    if a == "unused":
        un = [pool_row(c) for c, e in sorted(L.items()) if not e["used"] and not e["users"]]
        return log("cores: pool: %s: %d unused (%s): %s" % (k, len(un), POOL_WARN, " ".join(un)))
    log("cores: pool: %s: set|replace|add|clear|unload|fill|input|builtin|unused|list|undo" % k)


# ---- the pairing code (pi/engine/paircode.h; the reference is pi/engine/tests/test_paircode.cpp)
import hashlib, secrets
PAIR_PATH = os.path.join(ROOT, ".rytm", "pair.txt")
# The device shows the code on the Rytm's screen and on the touchscreen and writes it nowhere. This mock has neither
# screen, so MOCK_PAIR_OUT names a file to write it to - a stand-in for the screen, outside the card, for a test to
# read. Nothing on the card ever holds it (page_pair.test.mjs checks that).
PAIR_OUT = os.environ.get("MOCK_PAIR_OUT", "")
PAIR_LOCK = float(os.environ.get("MOCK_PAIR_LOCK", "60"))
PAIR_GAP = float(os.environ.get("MOCK_PAIR_GAP", "1"))
PAIR_MAX_FAILS, PAIR_CLIENTS, PAIR_SESSIONS = 5, 16, 16
PAIR_SHOW_GAP = 10          # paircode.h kShowGapS (fixed)
# web.cpp's Open(): the whole open set, and nothing else. Everything else needs the pairing code, so a new endpoint
# is gated unless someone deliberately adds it here (page_pair.test.mjs reads this list out of web.cpp and checks the
# two agree, and that nothing else in web.cpp answers an unpaired client).
PAIR_OPEN = {"/", "/index.html", "/bundle-sw.js", "/health", "/status", "/log", "/pair", "/pair/status", "/pair/resume",
             "/pair/show"}
NOT_PAIRED = ("not paired: enter the pairing code shown on the Rytm's screen and on the Sidecar's screen")
_mp = os.environ.get("MOCK_PAIR", "")
PAIR = {"mode": "off" if _mp == "0" else "fixed" if re.fullmatch(r"\d{8}", _mp) else "on",
        "code": _mp if re.fullmatch(r"\d{8}", _mp) else "".join(secrets.choice("0123456789") for _ in range(8)),
        "clients": [], "sessions": {}, "peers": {}, "unpaired": 0, "show_at": 0.0, "show_gap": PAIR_SHOW_GAP}


def pair_load():
    try:
        text = open(PAIR_PATH, encoding="utf-8").read()
    except OSError:
        return
    for l in text.splitlines():
        if not l.startswith("c="):
            continue
        f = l[2:].split(" ", 2)
        if len(f) == 3 and re.fullmatch(r"[0-9a-f]{8}", f[0]) and re.fullmatch(r"[0-9a-f]{64}", f[1]) \
           and len(PAIR["clients"]) < PAIR_CLIENTS:
            PAIR["clients"].append({"id": f[0], "hash": f[1], "name": f[2]})


def pair_save():
    os.makedirs(os.path.dirname(PAIR_PATH), exist_ok=True)
    body = ("# Sidecar paired devices (docs/pi-engine.md \"Pairing code\"). The code itself is never\n"
            "# stored here: these are SHA-256 of each device's own secret. Delete this file to unpair all.\nv=1\n")
    for c in PAIR["clients"]:
        body += "c=%s %s %s\n" % (c["id"], c["hash"], c["name"])
    open(PAIR_PATH + ".part", "w", encoding="utf-8").write(body)      # as wifi.txt is written
    if os.path.exists(PAIR_PATH):
        os.replace(PAIR_PATH, PAIR_PATH + ".prev")
    os.replace(PAIR_PATH + ".part", PAIR_PATH)


def pair_peer(peer):
    return PAIR["peers"].setdefault(peer, {"fails": 0, "locks": 0, "lock_until": 0.0, "last_try": 0.0})


def pair_lock_left(peer):
    now, e = time.time(), PAIR["peers"].get(peer)
    if not e:
        return 0
    if e["lock_until"] > now:
        return int(e["lock_until"] - now) + 1
    if e["last_try"] and now - e["last_try"] < PAIR_GAP:
        return 1
    return 0


def pair_fail(peer):
    e = pair_peer(peer)
    e["last_try"] = time.time()
    e["fails"] += 1
    if e["fails"] >= PAIR_MAX_FAILS:
        e["fails"] = 0
        e["lock_until"] = time.time() + PAIR_LOCK * (2 ** min(e["locks"], 4))
        e["locks"] = min(e["locks"] + 1, 4)


def pair_clear(peer):          # a correct code or a resume from this address: its slate, nobody else's
    PAIR["peers"].pop(peer, None)


def pair_session(cid=""):
    if len(PAIR["sessions"]) >= PAIR_SESSIONS:
        del PAIR["sessions"][min(PAIR["sessions"], key=lambda t: PAIR["sessions"][t]["seen"])]
    tok = secrets.token_hex(16)
    PAIR["sessions"][tok] = {"id": cid, "seen": time.time()}
    return tok


def pair_check(tok):
    if PAIR["mode"] == "off":
        return True
    s = PAIR["sessions"].get(tok or "")
    if not s:
        return False
    s["seen"] = time.time()
    return True


def pair_token(h, q, form):
    """web.cpp's Token(): the X-Sidecar-Auth header, else tok= in a POST's body (never the query string).
    MOCK_PAIR_NOHDR=1 is a kernel built without pi/circle/circle-http-auth.patch: the header never reaches it, so
    pairing succeeds and then nothing stays paired (hardware, 2026-10-25)."""
    hdr = "" if os.environ.get("MOCK_PAIR_NOHDR") else (h.get("X-Sidecar-Auth") or "")
    return hdr or form.get("tok") or ""


def pair_do(op, form, tok, peer=""):
    g = PAIR
    if op == "":
        if g["mode"] == "off":
            return {"ok": False, "code": 403, "wait": 0, "error": "the pairing code is off on this card (cmdline.txt paircode=0)"}
        code = form.get("code", "")
        if not re.fullmatch(r"\d{8}", code):      # not a guess: never counted (paircode.h)
            return {"ok": False, "code": 403, "wait": pair_lock_left(peer), "error": "the pairing code is 8 digits"}
        if pair_lock_left(peer):
            return {"ok": False, "code": 403, "wait": pair_lock_left(peer),
                    "error": "too many wrong codes: pairing is locked for a while (the code on the screen still works after it)"}
        if not secrets.compare_digest(code, g["code"]):
            pair_fail(peer)
            return {"ok": False, "code": 403, "wait": pair_lock_left(peer),
                    "error": "that is not the pairing code on the Sidecar's screen"}
        pair_clear(peer)
        PAIR["show_at"], PAIR["show_gap"] = 0.0, PAIR_SHOW_GAP
        name = "".join(c if 0x20 <= ord(c) <= 0x7e else "?" for c in form.get("name", "")[:24]).strip() or "a client"
        remember = form.get("remember", "1") != "0"
        out = {"ok": True, "token": pair_session()}
        if remember:
            sec = secrets.token_hex(16)
            cid = secrets.token_hex(4)
            if len(g["clients"]) >= PAIR_CLIENTS:   # paircode.cpp victim(): a browser before sdboot.py
                browsers = [c for c in g["clients"] if c["name"] != "sdboot.py"]
                g["clients"].remove(min(browsers or g["clients"], key=lambda c: c.get("used", 0)))
            g["clients"].append({"id": cid, "hash": hashlib.sha256(bytes.fromhex(sec)).hexdigest(), "name": name,
                                 "used": time.time()})
            pair_save()
            PAIR["sessions"][out["token"]]["id"] = cid
            out["client"], out["secret"] = cid, sec
        log("pair: a device paired with the code (\"%s\"): %d paired" % (name, len(g["clients"])))
        return out
    if op == "resume":
        # Deliberately outside the code's lockout, in both directions (paircode.h): a 128-bit secret is not
        # guessable, so rate-limiting it protects nothing, and counting it let a browser with a stale entry lock
        # its own owner out - and out of `sdboot.py install`, which shares the limiter.
        if g["mode"] == "off":
            return {"ok": False, "code": 403, "wait": 0, "error": "the pairing code is off on this card (cmdline.txt paircode=0)"}
        stale = {"ok": False, "code": 403, "wait": 0,
                 "error": "the saved pairing is no longer valid on this Sidecar: the code is needed again"}
        cid, sec = form.get("client", ""), form.get("secret", "")
        if not re.fullmatch(r"[0-9a-fA-F]{8}", cid) or not re.fullmatch(r"[0-9a-fA-F]{32}", sec):
            return stale
        want = hashlib.sha256(bytes.fromhex(sec)).hexdigest()
        hit = [c for c in g["clients"] if c["id"] == cid.lower() and secrets.compare_digest(c["hash"], want)]
        if not hit:
            return stale
        hit[0]["used"] = time.time()
        pair_clear(peer)            # the owner is here: this address's lockout goes (nobody else's)
        return {"ok": True, "token": pair_session(cid.lower())}
    if op in ("forget", "revoke"):
        if not pair_check(tok):
            return None
        if op == "revoke":
            g["clients"], g["sessions"] = [], {}
        else:
            cid = g["sessions"].get(tok, {}).get("id", "")
            g["clients"] = [c for c in g["clients"] if c["id"] != cid]
            g["sessions"] = {t: v for t, v in g["sessions"].items() if t != tok and (not cid or v["id"] != cid)}
        pair_save()
        log("pair: %s (%d paired devices)" % ("every device unpaired" if op == "revoke" else "a device unpaired itself",
                                              len(g["clients"])))
        return {"ok": True}
    return None


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

    # ---- project bundles (bundleweb.cpp): a sketch for trying the Project tab, not its reference (test_bundleweb is).
    # An export walks HASH -> DONE in a few seconds; an import of anything shows a fixed summary.
    def bundle(self, p, q, body):
        f = {k: v[0] for k, v in parse_qs(body.decode(errors="replace")).items()}
        j = lambda o: self.send(json.dumps(o), "application/json")
        B = BUNDLE
        if B["t"] and B["phase"] not in ("READY", "DONE", "FAILED", "IDLE", "SERVE"):
            el = time.time() - B["t"]
            B["done"] = min(B["total"], int(B["total"] * el / 4))
            if el > 4: B.update(phase="DONE" if B["job"] != "import" else "DONE", msg="done")
        if p == "/bundle/projects":
            return j([{"key": "DEMO PERC", "rytm": "DEMO PERC", "used": 3, "date": "2026-10-22T14:03:00Z", "imported": False, "current": True, "files": 16, "bytes": 772},
                      {"key": "A_B", "rytm": "A/B", "used": 2, "date": "", "imported": True, "current": False, "files": 9, "bytes": 5120},
                      {"key": "LIVE SET", "rytm": "LIVE SET", "used": 1, "date": "", "imported": False, "current": False, "files": 30, "bytes": 81920}])
        if p == "/bundle/list":
            return j([{"name": "DEMO PERC 20261022-1403.sidecar.zip", "size": 2178082, "date": "2026-10-22 14:03", "incomplete": False}])
        if p == "/bundle/zip":
            off = int(q.get("off", 0))
            body = b"PK\x03\x04" + bytes(1020)
            return self.send(body[off:off + int(q.get("len", 65536))], "application/octet-stream")
        if p == "/bundle/status":
            return j(dict(ok=True, key=B["key"], file="", name="", bytes=0, seq=int(B["t"] * 10), clock="browser", waiting="", **{k: B[k] for k in ("job", "phase", "done", "total", "msg")}))
        if p == "/bundle/plan" and os.environ.get("MOCK_SLOTS") == "1":
            # slot mode (a card keyed by the Rytm's slots, core/bundle/import.cpp): the user's 0.3.0 bundle, no Rytm file;
            # the Rytm's list names 047 as the bundle; slot 1 the current one; MOCK_SLOTS=1
            sl = [["", 0, 0] for _ in range(128)]
            sl[0], sl[2], sl[46], sl[47] = ["LIVE SET", 30, 1], ["A/B", 9, 0], ["SDCR01R 03", 86, 0], ["JAM 1021", 0, 0]
            return j({"bundle": "/Bundles/incoming/x.sidecar.zip", "from": {"key": "@047", "rytm_name": "SDCR01R 03", "created": "", "sidecar": "0.3.0", "board": "pi4"},
                      "name": "SDCR01R 03", "key": "@047", "clash": True, "mode": "new", "slot": 47, "rytm_list": True, "slots": sl,
                      "new_bytes": 29000, "new_sample_bytes": 0, "reused_bytes": 105000000,
                      "files": {"write": 0, "reuse": 86, "relocated": 0}, "totals": {"samples": 79, "songs": 0, "patterns": 16, "bytes": 105029000},
                      "relocated": [], "machines": [{"slot": i + 1, "id": n, "ver": "1.0.0", "name": n, "action": "reuse", "to": "", "code": True}
                                                    for i, n in enumerate(["sc.snare", "mdkit.md", "MD-PI", "sc.acid", "MD-EFM"])],
                      "missing": [], "warnings": [], "rytm": None,
                      "step": "This bundle holds only the Sidecar's side, no Rytm project file. Slot 047 on the Rytm holds SDCR01R 03: load it on the Rytm.",
                      "phase": B["phase"], "msg": ""})
        if p == "/bundle/plan":
            return j({"bundle": "/Bundles/incoming/x.sidecar.zip", "from": {"key": "DEMO PERC", "rytm_name": "DEMO PERC", "created": "", "sidecar": "0.3.0", "board": "pi4"},
                      "name": "DEMO PERC 2", "key": "DEMO PERC 2", "clash": True, "mode": "new", "new_bytes": 838, "new_sample_bytes": 0, "reused_bytes": 2093036,
                      "files": {"write": 0, "reuse": 22, "relocated": 0}, "totals": {"samples": 21, "songs": 1, "patterns": 2, "bytes": 2094000},
                      "relocated": [], "machines": [{"slot": 1, "id": "sc.grain", "ver": "1.0.0", "name": "GRAIN", "action": "install", "to": "", "code": True}],
                      "missing": ["/Samples/Drums/missing.wav"], "warnings": [], "rytm": {"file": "rytm/DEMO PERC.arprj", "to": "/Bundles/Rytm/DEMO PERC 2.arprj", "size": 79},
                      "step": "Send DEMO PERC.arprj to the Rytm with Elektron Transfer, then save it on the Rytm as DEMO PERC 2.", "phase": B["phase"], "msg": ""})
        if p in ("/bundle/export", "/bundle/verify", "/bundle/import", "/bundle/delete"):
            if not same_origin(self.headers):
                return j({"ok": False, "error": "refused: not from this page (the same-origin check)"})
            job = {"/bundle/export": "export", "/bundle/verify": "import", "/bundle/import": "import", "/bundle/delete": "delete"}[p]
            B.update(job=job, key=f.get("key", "DEMO PERC"), phase="READY" if p == "/bundle/verify" else "HASH", done=0, total=2 << 20, t=time.time(), msg="working")
            return j({"ok": True, "phase": B["phase"]})
        if p in ("/bundle/cancel", "/bundle/finish"):
            B.update(phase="IDLE", job="none", t=0, msg="")
            return j({"ok": True})
        if p == "/time":
            return j({"ok": True})
        if p.startswith("/usb/"):
            return j({"present": False})
        if p == "/bundle-sw.js":
            return self.send(open(os.path.join(HERE, "..", "www", "bundle-sw.js")).read(), "application/javascript")
        return self.code(404, "no such endpoint")

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
        form = {k: v[0] for k, v in parse_qs(body.decode(errors="replace"), keep_blank_values=True).items()}
        tok = pair_token(self.headers, q, form)
        peer = self.headers.get("X-Mock-Peer") or self.client_address[0]   # paircode.h: the limiter is per address
        if p not in PAIR_OPEN and not pair_check(tok):       # web.cpp: HTTP 200, the code in the body
            PAIR["unpaired"] += 1
            js = not (p.startswith("/fs/") or p == "/cmd")       # web.cpp's WantsText()
            b = (json.dumps({"ok": False, "code": 401, "pair": True, "wait": 0, "error": NOT_PAIRED})
                 if js else "401 %s\n" % NOT_PAIRED).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json" if js else "text/plain; charset=utf-8")
            self.send_header("X-Sidecar-Pair", "required")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        if p == "/pair" or p.startswith("/pair/"):
            op = p[6:] if len(p) > 5 else ""
            j = lambda o: self.send(json.dumps(o), "application/json")
            if op == "status":
                # hdr: this request carried X-Sidecar-Auth (a kernel without circle-http-auth.patch never sees it,
                # which looks exactly like "pairing does not stick"). MOCK_PAIR_NOHDR=1 plays that kernel.
                hdr = bool(self.headers.get("X-Sidecar-Auth")) and not os.environ.get("MOCK_PAIR_NOHDR")
                return j({"gate": PAIR["mode"], "paired": pair_check(tok), "clients": len(PAIR["clients"]),
                          "sessions": len(PAIR["sessions"]), "wait": pair_lock_left(peer), "digits": 8,
                          "hdr": hdr, "rng": True, "host": "sidecar"})
            if op == "show":        # web.cpp: open, lenient same-origin, rate-limited; the code goes to the Rytm only
                if not same_origin(self.headers) and (self.headers.get("Origin") or self.headers.get("Referer")):
                    return self.send_error(400)
                shown, wait = False, 0
                if PAIR["mode"] != "off" and not pair_check(tok):
                    left = PAIR["show_gap"] - (time.time() - PAIR["show_at"]) if PAIR["show_at"] else 0
                    if left <= 0:               # paircode.h ShowGap: a fixed 10 s, reset by a pairing
                        PAIR["show_at"] = time.time()
                        log("pair: the code is on the Rytm's screen")
                        shown = True
                    else:
                        wait = math.ceil(left)  # ShowGap::wait_s: whole seconds left of the gap
                return j({"ok": True, "shown": shown, "wait": wait})
            if op in ("", "resume", "forget", "revoke"):
                if not same_origin(self.headers) and (op in ("forget", "revoke") or self.headers.get("Origin")
                                                      or self.headers.get("Referer")):
                    return j({"ok": False, "code": 403, "error": "refused: not from this page (the same-origin check)"})
                r = pair_do(op, form, tok, peer)
                if r is None:
                    return j({"ok": False, "code": 401, "pair": True, "error": NOT_PAIRED})
                return j(r)
            return self.send_error(404)
        if p.startswith("/bundle") or p == "/time" or p.startswith("/usb/"):
            return self.bundle(p, q, body)
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
        if p == "/health":
            return self.send(json.dumps({
                "alive": True, "version": "0.1.0", "commit": "abc1234", "dirty": False, "uptime": int(time.time() - T0),
                "temp": 57, "streaming": True, "rytmMidi": True, "display": True, "underruns": 0,
                "pair": PAIR["mode"], "power": POWER}), "application/json")
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
                "fxin": fxin_status(), "lfo": lfo_status(), "stor": stor_status(),
                "library": {"state": "HASHING", "files": 48213, "bytes": 19756849152, "hashed": 29410, "pending": 18803, "pct": 61,
                            "dups": 212, "dupBytes": 404750336, "missing": 0, "unreadable": 0, "recordings": 14,
                            "waiting": "PLAYING", "logLines": 412, "readOnly": False},
                "pair": {"gate": PAIR["mode"], "clients": len(PAIR["clients"]), "sessions": len(PAIR["sessions"]),
                         "rng": True, "locked": 0},
                "wifi": {"mode": WIFI["mode"], "chip": WIFI["chip"], "stored": bool(WIFI["ssid"]), "on": WIFI["on"],
                         "connected": WIFI["join"] == "up", "rejoins": 0, "scanning": False, "scans": WIFI["scans"],
                         "networks": len(FAKE_NETS) if WIFI["scans"] else 0, "join": WIFI["join"]}}), "application/json")
        if p == "/log":
            # the hardware pass 2026-10-23's figures (a busy pattern): the STATUS cards' widest values (fix1049)
            log("engine: engine: fifo min 78 max 96 target 96 under 0 | blocks 4000 max 22 us | cpu c1 8.1 c2 1.7 c3 10.4 % | pass max 1234 us")
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
            pool_cmd(c)
            if c.startswith("rec="):
                rec_cmd(c[4:])
            if c.startswith("stor"):
                stor_command(c)
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
        if p == "/pool":
            return self.send(json.dumps(pool_json(q)), "application/json")
        if p == "/lists":
            return self.send(json.dumps(LISTS), "application/json")
        if p.startswith("/fs/"):
            try:
                return self.fs(p[4:], q, body)
            except OSError as e:                 # the engine answers a code; this must not drop the connection
                return self.code(409, "cannot: %s" % e.__class__.__name__)
        self.send_error(404)

    def fs(self, op, q, body):
        # web.cpp: /fs/* had no origin check at all before 0.3.0, so a page on another site could reach it by DNS
        # rebinding. Reads strict (the page sends a Referer on its own GETs), the four writes lenient (a tool).
        if not (same_origin(self.headers) if op in ("ls", "get") else not_cross_site(self.headers)):
            return self.code(403, "refused: not from this page (the same-origin check)")
        path = q.get("path", "") or "/"
        to = q.get("to", "")
        if not fs_reachable(path) or (op == "mv" and not fs_reachable(to)):
            return self.code(404, "no such path (the file browser reaches the content folders only)")
        if op == "ls":
            if not path_ok(path) or not os.path.isdir(disk(path)):
                return self.code(404, "no such folder")
            es = []
            for nm in sorted(os.listdir(disk(path))):
                full = (path if path != "/" else "") + "/" + nm
                if not fs_reachable(full):      # the root shows the content folders, not the boot files or /.rytm
                    continue
                d = os.path.isdir(disk(full))
                es.append({"n": nm, "d": int(d), "s": 0 if d else os.path.getsize(disk(full)),
                           "t": time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(disk(full)))),   # fsweb.h
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
    pair_load()
    if PAIR_OUT and PAIR["mode"] != "off":
        open(PAIR_OUT, "w", encoding="utf-8").write("%s %s\n" % (PAIR["code"][:4], PAIR["code"][4:]))
    # this mock has no display, so it writes the code to the card's root as a screenless engine does. Nothing on the
    # network can read it: /fs/* does not reach the root's files at all, and every read needs the code anyway.
    recourse = os.path.join(ROOT, "sidecar-paircode.txt")
    if PAIR["mode"] != "off":
        open(recourse, "w", encoding="utf-8").write("Sidecar pairing code: %s %s\n" % (PAIR["code"][:4], PAIR["code"][4:]))
    elif os.path.exists(recourse):
        os.remove(recourse)
    print("pair: the gate is %s, %d device(s) remembered" % (PAIR["mode"], len(PAIR["clients"])), flush=True)
    print("http://127.0.0.1:%d/  card %s" % (PORT, ROOT), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
