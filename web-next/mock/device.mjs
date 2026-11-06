// web-next/mock/device.mjs - the mock Sidecar's state: a /status in the real shape (pi/engine/kernel.cpp WebStatus, the
// members the app reads; web-next/PARITY.md), the console commands that change it, the stats lines the real engine logs
// once a second (the Home tiles read them), and a sequencer view (pi/engine/seqview_json.h's shape).

import { createFs } from "./fs.mjs";

const MACHINES = ["plaits", "juno", "daisy", "dsps", "dd", "fm"];
export const TRACKS = ["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"];

export function createDevice() {
  const started = Date.now();
  const d = {
    commands: 0,
    notes: true,
    out: "tracks",
    mac: true,
    fxin: { want: 0, state: "off", rytm: 0 },
    fifoTarget: 96,
    usbMode: { set: "class", from: "card", runs: "class", modes: ["class", "overbridge"], shown: "sidecar", boxUp: true, board: 4 },
    machines: ["plaits", "juno", "daisy", "dsps", "fm", "dd", "daisy", "plaits"],
    drone: -1,
    screensaver: 15,
    brightness: 80,
    wifi: { on: false, ssid: "", networks: [{ ssid: "Studio", rssi: -48, ch: 6, band: "2.4", secured: true }, { ssid: "Guest", rssi: -71, ch: 36, band: "5", secured: false }], join: { state: "none", reason: "", left: 0 } },
    bridge: { on: false, os: false, data: false },
    pairClients: 1,
    sdtest: { state: "idle", phase: "", progress: 0, have: false },
    log: [],
    logTotal: 0,
    seq: seqView(),
    playStart: Date.now(),
    transport: 1,
    // the mixer (core/fx/insert.cpp json "ins": V1..V8, B1..B3; out routes; the master "mfx")
    ins: {
      kit: 1,
      route3: 0,
      c: ["V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3"].map((n, i) => ({
        n,
        s: [{ t: ["FILTER", "EQ", "DRIVE", "OFF", "OFF", "FILTER", "EQ", "OFF", "OFF", "OFF", "OFF"][i], p: [80, 20, 0, 0], l: [80, 20, 0, 0] }, { t: i === 0 ? "COMP" : "OFF", p: [50, 60, 40, 20], l: [50, 60, 40, 20] }],
        peq: [[0, 0, 80, 70], [20, 0, 60, 80], [0, 0, 64, 64], [0, 30, 70, 50], [0, 0, 64, 64], [10, 40, 50, 64], [30, 0, 64, 80], [0, 0, 64, 64], [40, 60, 64, 64], [30, 70, 64, 64], [0, 0, 64, 64]][i],
        ...(i < 8 ? { send: [i === 1 ? 50 : 0, i === 6 ? 30 : 0, 0] } : {}),
      })),
      out: Array.from({ length: 8 }, (_, i) => ({ route: i === 1 ? "post" : "trk", lev: 100 - i * 4, pan: 64, wid: 64, dec: 0, open: 0 })),
      mfx: { byp: 0, src: "ret", lev: 100, trim: 0, gr: 1.5, on: 1, s: [
        { t: "COMP", p: [50, 60, 40, 20], l: ["THR", "RAT", "SPD", "GAIN"], v: ["-12dB", "4:1", "40", "+2dB"], mix: 127, pair: 0, send: 0 },
        { t: "OFF", p: [0, 0, 0, 0], l: ["", "", "", ""], v: ["", "", "", ""], mix: 127, pair: 0, send: 0 },
        { t: "OFF", p: [0, 0, 0, 0], l: ["", "", "", ""], v: ["", "", "", ""], mix: 127, pair: 0, send: 0 },
        { t: "OFF", p: [0, 0, 0, 0], l: ["", "", "", ""], v: ["", "", "", ""], mix: 127, pair: 0, send: 0 },
      ], peq: [0, 0, 64, 64], rep: 1, onmain: 0 },
    },
    fs: createFs(),
    // the recorder (recorder.h / rec_pi.cpp's /status "rec"), playback (rec_play.h) and the samplers (core/samp)
    rec: { state: 0, arm: false, ready: true, frames: 0, startedAt: 0, take: "jam_2026-11-06_1840", prefix: "", vol: "SD", volOk: 1, freeKnown: 1, freeMB: 28120, totalMB: 30436,
      bits: 24, ch: [0, 3, 4], speedKBps: 21_000, speedMeasured: 1, drops: 0, silence: 0, outShort: 0, error: "", abandoned: 0, writes: 0, cardKBps: 0, needKBps: 0, worstWriteMs: 0, ringMs: 4000, ringMax: 0, ringSlots: 64, bytes: 0 },
    play: { state: 0, take: "", ch: "", posMs: 0, lenMs: 0, level: 0, startedAt: 0, error: "" },
    samp: { state: 0, ms: 0, bpm: 0, wav: "", nSlices: 0, error: "", startedAt: 0 },
    asamp: { state: 0, done: 0, total: 0, label: "", sfz: "", written: 0, loops: 0, error: "", startedAt: 0 },
    lfo: { n: 8, kit: 1, l: Array.from({ length: 8 }, (_, i) => ({ tgt: ["V2", "I1", "OFF", "OFF", "OFF", "OFF", "OFF", "OFF"][i], par: i === 1 ? 8 : 3, pn: i === 1 ? "EQ HPF" : "P4", np: i === 1 ? 12 : 16, wav: i === 1 ? 7 : 0, rate: i === 1 ? 8 : 60, rn: "", sync: i === 1 ? 1 : 0, trig: 0, dep: i < 2 ? 40 : 0, base: 64, des: "403" + "503".repeat(3) + "6f1".repeat(4) + "302".repeat(8), ph: 0 })) },
    // user machines (pi/engine/tests/mock_web.py's sketch of /um)
    plugins: [["sc.grain", "GRAIN", 400], ["sc.acid", "ACID", 120], ["sc.metal", "METAL", 90], ["sc.wobble", "WOBBLE", 200]],
    slots: { 1: { id: "sc.grain", src: "sounds" }, 2: { id: "sc.acid", src: "saved" }, 3: { id: "", src: "none", unknown: true, want_name: "WOBB" }, 4: { id: "sc.metal", src: "inherited" }, 5: { id: "", src: "empty" } },
    lists: {
      juno: { total: 3, list: [{ slot: 0, name: "Brass", used: true, found: true }, { slot: 1, name: "Strings", used: true, found: true }, { slot: 2, name: "Gone pad", used: true, found: false }] },
      fm: { total: 96, rom: true, list: [] },
    },
    fx: [{ type: "RINGS", post: 1, send: [0, 40, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0] }, { type: "CLOUD", post: 0, send: [0, 0, 0, 30, 0, 0, 0, 60, 0, 0, 0, 0] }],
  };

  const up = () => Math.floor((Date.now() - started) / 1000);

  d.status = (web = { requests: 0, open: 0 }) => ({
    host: "sidecar-mock",
    ip: "169.254.20.230",
    uptime: up(),
    web: { requests: web.requests, maxus: 812, commands: d.commands, refused: 0, idle: 0, open: web.open },
    boxes: [{ id: "rytm", name: "Analog Rytm MKII", n: "Rytm", role: "in use", on: true, feat: 511, tracks: 12, voices: 8 }],
    link: "usb",
    gadget: "sidecar",
    sidecar: true,
    mac: d.mac,
    usbHost: true,
    streaming: true,
    rytmMidi: true,
    temp: 58 + Math.round(3 * Math.sin(up() / 20)),
    tempWarn: false,
    clockLow: 0,
    out: d.out,
    notes: d.notes,
    drone: d.drone,
    fifoTarget: d.fifoTarget,
    underruns: 0,
    restartUnderruns: 0,
    streams: 1,
    usbMode: d.usbMode,
    cpu: { setting: "max", mhz: 1800, budgetUs: 340 },
    machines: d.machines,
    src: TRACKS.map((t, i) => ({ m: i < 8 ? d.machines[i].toUpperCase() : "RYTM", vb: i })).concat([{ m: "RYTM", vb: 0 }, { m: "RYTM", vb: 0 }, { m: "RYTM", vb: 0 }, { m: "RYTM", vb: 0 }]),
    version: "0.4.2",
    commit: "mock000",
    dirty: false,
    build: "Nov  6 2026 12:00:00",
    circle: "50.0.1",
    screensaver: d.screensaver,
    brightness: d.brightness,
    touchtest: 0,
    display: true,
    blank: false,
    power: { known: true, warn: "", maxCurrent: 3000, uvNow: false, uvBoot: false, throttledNow: false, cappedNow: false, note: "" },
    wifi: { mode: "sta", chip: "ok", stored: !!d.wifi.ssid, on: d.wifi.on, connected: d.wifi.on && !!d.wifi.ssid, networks: d.wifi.networks.length, join: d.wifi.join.state },
    rec: d.recStatus(),
    play: d.playStatus(),
    samp: { ...d.samp, ms: d.samp.state === 2 ? Date.now() - d.samp.startedAt : d.samp.ms },
    asamp: d.asampStatus(),
    fxin: d.fxin,
    stor: { vols: [{ name: "SD", state: "mounted", fs: "exFAT", freeMB: 28120, totalMB: 30436, link: "", model: "", pulls: 0 }, { name: "USB", state: "mounted", fs: "exFAT", freeMB: 51200, totalMB: 61440, link: "USB 3", model: "SanDisk", pulls: 0 }], exfat: true, bench: { state: "idle" } },
    sdtest: d.sdtest,
    library: { state: "ready", files: 1834, bytes: 2.1e9, hashed: 1834, pending: 0, pct: 100, dups: 3, missing: 0 },
    pair: { gate: "on", rng: true, clients: d.pairClients, sessions: 1, locked: false },
    ins: d.ins,
    lfo: { ...d.lfo, l: d.lfo.l.map((l, i) => ({ ...l, rn: l.sync ? ["1/16", "1/8", "1/4", "1BAR"][i % 4] : "", ph: l.tgt === "OFF" ? 0 : Math.floor(((Date.now() - started) / (800 + 300 * i)) * 1000) % 1000 })) },
    fx: d.fx,
    midibridge: { on: d.bridge.on, state: d.bridge.on ? "waiting" : "off", port: 7320, osUpgrade: d.bridge.os, dataWrites: d.bridge.data, toRytm: 0, fromRytm: 0, drops: 0 },
  });

  // the engine's once-a-second lines, in the real format (pi/engine/kernel.cpp LogStats, ob_usb, sidecar_gadget)
  d.tick = () => {
    const j = () => (Math.random() * 3).toFixed(1);
    d.addLog(`engine: engine: fifo min ${80 + Math.round(Math.random() * 10)} max ${110 + Math.round(Math.random() * 10)} target ${d.fifoTarget} under 0 (restart 0, streams 1) | blocks 3000 max 210 us | cpu c1 ${(40 + +j()).toFixed(1)} c2 ${(32 + +j()).toFixed(1)} c3 ${(18 + +j()).toFixed(1)} % | pass max 260 us | midi 0 B (lost 0) sysex 0 notes 0 | to-host sent 0 B 0 msgs fail 0 dropped 0 | peak 512/1000 | ${58} C`);
    d.addLog(`rytmob: 1000ms IN 1000/s gap 0 lost 0 late 0`);
    d.addLog(`sidecar: sidecar alt 1/1 · late 0/0/0`);
  };
  d.addLog = (line) => {
    const t = ((Date.now() - started) / 1000).toFixed(3);
    const text = `${t} ${line}\n`;
    d.log.push({ at: d.logTotal, text });
    d.logTotal += text.length;
    while (d.log.length > 400) d.log.shift();
  };
  d.logSince = (since, max = 8192) => {
    const first = d.log.length ? d.log[0].at : d.logTotal;
    let from = since < first || since > d.logTotal ? first : since;
    let text = "";
    for (const e of d.log) if (e.at >= from) text += e.text;
    if (text.length > max) { from += text.length - max; text = text.slice(text.length - max); }
    return { from, to: d.logTotal, gap: since < first, text };
  };

  // console commands (the subset the app sends; everything is logged as the real console does)
  d.command = (c) => {
    d.commands++;
    d.addLog(`web: command :${c.startsWith("wifi=join") ? "wifi=join:..." : c} (ws)`);
    let m;
    if (c === "notes=1" || c === "notes=0") d.notes = c.endsWith("1");
    else if (c === "out=main" || c === "out=tracks") d.out = c.slice(4);
    else if (c === "mac=on" || c === "mac=off") d.mac = c.endsWith("on");
    else if ((m = /^fxin=([01])$/.exec(c))) d.fxin = { want: +m[1], state: m[1] === "1" ? "on" : "off", rytm: +m[1] };
    else if ((m = /^fifo=(\d+)$/.exec(c))) d.fifoTarget = Math.max(24, Math.min(480, +m[1]));
    else if ((m = /^usbmode=(class|overbridge|cmdline)$/.exec(c))) d.usbMode = { ...d.usbMode, set: m[1] === "cmdline" ? "class" : m[1], runs: m[1] === "overbridge" ? "overbridge" : "class", shown: m[1] === "overbridge" ? "box" : "sidecar" };
    else if ((m = /^notemach=(\d):(\w+)$/.exec(c)) && MACHINES.includes(m[2])) d.machines[+m[1]] = m[2];
    else if ((m = /^drone=(\d|all|off)$/.exec(c))) d.drone = m[1] === "off" ? -1 : m[1] === "all" ? 8 : +m[1];
    else if ((m = /^screensaver=(\d+)$/.exec(c))) d.screensaver = +m[1];
    else if ((m = /^brightness=(\d+)$/.exec(c))) d.brightness = Math.max(5, Math.min(100, +m[1]));
    else if (c === "sdtest=start!") d.sdtest = { state: "done", phase: "", progress: 100, have: true, verdict: "fine", writeMBps: 21.4, readMBps: 88.1, rand32Iops: 2100, rand32P99Ms: 3.1 };
    else if (c === "version") d.addLog("version: Sidecar 0.4.2 (mock000)");
    else if ((m = /^seqed=show:(\d+)$/.exec(c))) d.seq.track = +m[1];
    else if ((m = /^seqed=add:(\d+):(\d+):(\d+):(-?\d+):(\d+):(\d+)$/.exec(c))) { d.seq.notes.push([+m[2], +m[4], +m[3], +m[5], +m[6], 0]); d.seq.gen++; d.seq.undo++; }
    else if ((m = /^seqed=del:(\d+):(\d+):(\d+)$/.exec(c))) { d.seq.notes = d.seq.notes.filter((n) => !(n[0] === +m[2] && n[2] === +m[3])); d.seq.gen++; d.seq.undo++; }
    else if ((m = /^seqed=vel:(\d+):(\d+):(\d+):(\d+)$/.exec(c))) { for (const n of d.seq.notes) if (n[0] === +m[2] && n[2] === +m[3]) n[3] = +m[4]; d.seq.gen++; }
    else if ((m = /^seqed=len:(\d+):(\d+):(\d+):(\d+)$/.exec(c))) { for (const n of d.seq.notes) if (n[0] === +m[2] && n[2] === +m[3]) n[4] = +m[4]; d.seq.gen++; }
    else if ((m = /^seqed=move:(\d+):(\d+):(\d+):(\d+):(\d+):(-?\d+)$/.exec(c))) { for (const n of d.seq.notes) if (n[0] === +m[2] && n[2] === +m[3]) { n[0] = +m[4]; n[2] = +m[5]; n[1] = +m[6]; } d.seq.gen++; }
    else if ((m = /^seq=swing:(\d+):(\d+)$/.exec(c))) { d.seq.swing = +m[2]; d.seq.gen++; }
    else if (/^ins=/i.test(c)) d.mixer(c);
    else if (/^(rec|play|samp|asamp)=/.test(c)) d.recCmd(c);
    else if ((m = /^lfo=([1-8]):off$/.exec(c))) d.lfo.l[+m[1] - 1].tgt = "OFF";
    else if ((m = /^lfo=([1-8]):tgt:(\w+)$/.exec(c))) d.lfo.l[+m[1] - 1].tgt = m[2].toUpperCase();
    else if ((m = /^lfo=([1-8]):(par|wav|sync|trig|rate|dep):(-?\d+)$/.exec(c))) d.lfo.l[+m[1] - 1][m[2]] = +m[3];
    else if ((m = /^um=assign:([1-5]):(\S+)$/.exec(c)) && d.plugins.some((p) => p[0] === m[2])) d.slots[+m[1]] = { id: m[2], src: "saved" };
    else if ((m = /^um=clear:([1-5])$/.exec(c))) d.slots[+m[1]] = { id: "", src: "empty" };
    else if ((m = /^(juno|fm)=(ren|del|swap|save):(\d+)(?::(.*))?$/.exec(c))) d.listCmd(m[1], m[2], +m[3], m[4]);
    else if (/^(out=V\d:|mfx=)/i.test(c)) d.mixer(c);
  };

  // ins= / out= / mfx= (core/fx/insert.cpp, out.cpp, master.cpp kHelp)
  const chainOf = (id) => ["V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3"].indexOf(id.toUpperCase());
  const EQK = ["hpf", "lpf", "low", "high"];
  d.mixer = (c) => {
    let m;
    if ((m = /^ins=(\w+):([AB]):type:(\w+)$/i.exec(c))) d.ins.c[chainOf(m[1])].s[m[2].toUpperCase() === "A" ? 0 : 1].t = m[3].toUpperCase();
    else if ((m = /^ins=(\w+):([AB]):p([1-4]):(\d+)$/i.exec(c))) d.ins.c[chainOf(m[1])].s[m[2].toUpperCase() === "A" ? 0 : 1].p[+m[3] - 1] = +m[4];
    else if ((m = /^ins=(\w+):eq:off$/i.exec(c))) d.ins.c[chainOf(m[1])].peq = [0, 0, 64, 64];
    else if ((m = /^ins=(\w+):eq:(hpf|lpf|low|high):(\d+)$/i.exec(c))) d.ins.c[chainOf(m[1])].peq[EQK.indexOf(m[2])] = +m[3];
    else if ((m = /^ins=(V\d):send:([1-3]):(\d+)$/i.exec(c))) d.ins.c[chainOf(m[1])].send[+m[2] - 1] = +m[3];
    else if ((m = /^out=V(\d):(lev|pan|wid):(\d+)$/i.exec(c))) d.ins.out[+m[1] - 1][m[2]] = +m[3];
    else if ((m = /^out=V(\d):(trk|pre|post)$/i.exec(c))) d.ins.out[+m[1] - 1].route = m[2];
    else if ((m = /^mfx=lev:(\d+)$/.exec(c))) d.ins.mfx.lev = +m[1];
    else if ((m = /^mfx=byp:([01])$/.exec(c))) d.ins.mfx.byp = +m[1];
    else if ((m = /^mfx=src:(ret|mix)$/.exec(c))) d.ins.mfx.src = m[1];
    else if ((m = /^mfx=trim:(-?\d+)$/.exec(c))) d.ins.mfx.trim = +m[1];
    else if ((m = /^mfx=([1-4]):type:(\w+)$/i.exec(c))) d.ins.mfx.s[+m[1] - 1].t = m[2].toUpperCase();
    else if ((m = /^mfx=([1-4]):p([1-4]):(\d+)$/i.exec(c))) d.ins.mfx.s[+m[1] - 1].p[+m[2] - 1] = +m[3];
    else if ((m = /^mfx=([1-4]):mix:(\d+)$/i.exec(c))) d.ins.mfx.s[+m[1] - 1].mix = +m[2];
    else if ((m = /^mfx=eq:(hpf|lpf|low|high):(\d+)$/i.exec(c))) d.ins.mfx.peq[EQK.indexOf(m[1])] = +m[2];
    else if (c === "mfx=eq:off") d.ins.mfx.peq = [0, 0, 64, 64];
  };

  // ---- the recorder, playback and the samplers
  const STEREO = new Set([0, 21, 22, 23, 24]);
  const CHN = ["MASTER", "MAIN_L", "MAIN_R", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB", "IN_L", "IN_R", "V_BD", "V_BT", "V_SD", "V_LT", "V_CH-OH", "V_MT-HT", "V_CY-CB", "V_RS-CP", "FX1", "FX2", "PRE", "POST"];
  const stamp = () => { const t = new Date(); return `${t.getFullYear()}-${String(t.getMonth() + 1).padStart(2, "0")}-${String(t.getDate()).padStart(2, "0")}_${String(t.getHours()).padStart(2, "0")}${String(t.getMinutes()).padStart(2, "0")}`; };
  d.recStatus = () => {
    const r = d.rec, live = r.state === 2;
    const frames = live ? Math.floor(((Date.now() - r.startedAt) / 1000) * 48000) : r.frames;
    const need = r.ch.reduce((a, id) => a + (STEREO.has(id) ? 2 : 1), 0) * (r.bits / 8) * 48000 / 1024;
    const peak = Array.from({ length: 25 }, (_, id) => (r.ch.includes(id) && r.state ? Math.round(300 + 500 * Math.random()) : 0));
    return { ...r, frames, peak, needKBps: Math.round(need), writes: live ? Math.floor(frames / 48000) * 12 : r.writes, cardKBps: live ? 19_800 : r.cardKBps, worstWriteMs: live ? 41 : r.worstWriteMs, bytes: live ? Math.round((frames / 48000) * need) : r.bytes, ringMax: live ? 6 : 0 };
  };
  d.playStatus = () => {
    const p = d.play;
    if (p.state !== 2) return p;
    const pos = Date.now() - p.startedAt;
    if (pos >= p.lenMs) { p.state = 0; return p; }
    return { ...p, posMs: pos, level: 60 + Math.round(30 * Math.random()) };
  };
  d.asampStatus = () => {
    const a = d.asamp;
    if (a.state === 1) {
      a.done = Math.min(a.total, Math.floor((Date.now() - a.startedAt) / 700));
      a.label = `C${2 + Math.floor(a.done / 3)} v100`;
      if (a.done >= a.total) Object.assign(a, { state: 3, sfz: "fm_0.sfz", written: a.total, loops: 0, label: "" });
    }
    return a;
  };
  d.recCmd = (c) => {
    const r = d.rec;
    let m;
    if ((m = /^rec=ch:(none|[\d,]+)$/.exec(c)) && r.state === 0) r.ch = m[1] === "none" ? [] : m[1].split(",").map(Number).filter((x) => x >= 0 && x < 25);
    else if ((m = /^rec=bits:(16|24)$/.exec(c)) && r.state === 0) r.bits = +m[1];
    else if ((m = /^rec=name:([A-Za-z0-9_-]{0,24})$/.exec(c)) && r.state === 0) r.prefix = m[1];
    else if ((m = /^rec=vol:(\w+)$/.exec(c)) && r.state === 0) { r.vol = m[1]; r.volOk = d.status().stor.vols.some((v) => v.name === m[1] && v.state === "mounted") ? 1 : 0; }
    else if (c === "rec=start" && r.state === 0 && r.ch.length) Object.assign(r, { state: 2, arm: false, startedAt: Date.now(), take: `${r.prefix || "SHARED_SET"}_${stamp()}` });
    else if (c === "rec=arm" && r.state === 0 && r.ch.length) Object.assign(r, { state: 1, arm: true });
    else if (c === "rec=stop") {
      if (r.state === 2) {
        r.frames = Math.floor(((Date.now() - r.startedAt) / 1000) * 48000);
        d.fs.addTake(r.vol === "SD" ? "/Recordings" : `/${r.vol}/Recordings`, r.take, r.ch.map((id) => CHN[id]));
      }
      r.state = 0;
    } else if ((m = /^play=start:(?:(\w+):)?([^/]+)\/(\d+)$/.exec(c)) && r.state !== 2) {
      const base = m[1] ? `/${m[1]}/Recordings` : "/Recordings";
      const f = [...d.fs.files.keys()].find((k) => k.startsWith(`${base}/${m[2]}/${String(m[3]).padStart(2, "0")}_`));
      if (f) Object.assign(d.play, { state: 2, take: (m[1] ? `${m[1]}:` : "") + m[2], ch: f.slice(f.lastIndexOf("/") + 4, -4), lenMs: 30_000, posMs: 0, startedAt: Date.now(), error: "" });
    } else if (c === "play=stop") d.play.state = 0;
    else if (c === "samp=arm") Object.assign(d.samp, { state: 2, startedAt: Date.now(), error: "" });
    else if (c === "samp=stop" && d.samp.state === 2) Object.assign(d.samp, { state: 5, ms: Date.now() - d.samp.startedAt, bpm: 120, wav: "SHARED SET/samp_001.wav", nSlices: 8 });
    else if (c === "asamp=start") Object.assign(d.asamp, { state: 1, done: 0, total: 15, startedAt: Date.now(), sfz: "", error: "" });
    else if (c === "asamp=cancel" && d.asamp.state === 1) d.asamp.state = 5;
  };

  d.um = () => {
    const a = d.plugins.map(([id, name, cpu], i) => ({ t: "plugin", id, ver: "1.0.0", size: 9000 + i * 1000, name, cpu, fit: true, why: "", file: `/Plugins/${id.replace(".", "_")}.rmp`, slots: Object.entries(d.slots).filter(([, s]) => s.id === id).map(([n]) => +n), status: "ok" }));
    for (let n = 1; n <= 5; n++) {
      const s = d.slots[n], p = d.plugins.find((x) => x[0] === s.id);
      a.push({ t: "slot", slot: n, id: s.id, ver: s.id ? "1.0.0" : "", state: p ? "READY" : s.id || s.unknown ? "MISSING" : "EMPTY", name: p ? p[1] : s.want_name ?? "", src: s.src, unknown: !!s.unknown, want_name: s.want_name ?? "", presets: p ? 2 : 0 });
    }
    a.push({ t: "plugins", n: d.plugins.length, project: "SHARED SET", dir: "/Plugins", scanning: false });
    a.push({ t: "mem", k: "code", size: 8 << 20, used: 60000 }, { t: "mem", k: "state", size: 1 << 20, used: 65536 });
    return a;
  };
  d.listCmd = (k, op, a, b) => {
    const L = d.lists[k].list;
    if (op === "ren" && L[a]) L[a].name = b ?? L[a].name;
    else if (op === "del" && L[a]) L.splice(a, 1);
    else if (op === "swap" && b !== undefined && L[a] && L[+b]) [L[a].name, L[+b].name] = [L[+b].name, L[a].name];
    else if (op === "save") L.push({ slot: L.length, name: b || "saved", used: true, found: true });
    L.forEach((x, i) => (x.slot = i));
    d.lists[k].total = L.length || (k === "fm" ? 96 : 0);
    if (k === "fm") d.lists.fm.rom = !L.length;
  };

  // a few ordinary lines, so the log view has more than the once-a-second ones (generic, as a boot writes them)
  for (const l of ["==== rytm-pi engine ==== Sidecar 0.4.2 (mock000), CPU 1800 MHz", "sidecar: USB-C class compliant, 12 x 12 + MIDI",
    "rytmob: Analog Rytm MKII attached, Overbridge streaming", "web: listening on sidecar-mock.local", "um: 4 packages in /Plugins, U1..U5 resolved",
    "seq: pattern A04, seq tracks T9 T10", "lib: 1834 files indexed, 3 duplicates"]) d.addLog(l);

  d.seqpos = () => {
    const stepMs = 60000 / 120 / 4;
    const pos = d.transport === 1 ? Math.floor(((Date.now() - d.playStart) / stepMs) * 1000) % (d.seq.length * 1000) : -1;
    return { pos, track: d.seq.track, transport: d.transport, pattern: d.seq.pattern, bpm10: d.seq.bpm10 };
  };
  return d;
}

// a sequencer view in pi/engine/seqview_json.h's shape: track 9 (T10) a poly track with a little chord pattern
function seqView() {
  const notes = [];
  for (const [s, chord] of [[0, [60, 64, 67]], [4, [62, 65, 69]], [8, [64, 67, 71]], [12, [60, 64, 67]], [16, [57, 60, 64]], [24, [55, 59, 62]]])
    for (const p of chord) notes.push([s, 0, p, 100 - (p % 7) * 4, 96 * 3, 0]);   // [step, micro, pitch, vel, len, flags]
  notes.push([14, 24, 72, 80, 48, 0]);
  return {
    valid: true, track: 9, tracks: [8, 9, -1, -1], pattern: 3, project: "SHARED SET", machine: "JUNO",
    length: 32, pages: 2, defaults: { note: 60, vel: 100, len: 96 }, transport: 1, bpm10: 1200, undo: 0, redo: 0, gen: 1,
    chords: ["OFF", "MAJ", "MIN", "SUS2", "SUS4", "MAJ7", "MIN7", "DOM7", "DIM", "AUG", "ADD9", "6", "MIN6", "9", "MIN9", "POW"],
    swing: 50, poly: { voices: 0, alloc: 0, unison: 0, info: "JUNO 4 NOTES" }, speed: 0, dir: 0, key: 0, scale: 0, qnt: 0,
    rnd: [0, 1, 0], seed: 0, seedFixed: false, editPage: -1, noteEdit: { on: false, step: -1, pitch: -1, sel: 0 },
    notes, steps: [[4, 2, 0, 0, 0, 0, 0, 0, 0, 0], [12, 0, 1, 2, 3, 0, 0, 0, 0, 0]],
  };
}
