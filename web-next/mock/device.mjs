// web-next/mock/device.mjs - the mock Sidecar's state: a /status in the real shape (pi/engine/kernel.cpp WebStatus, the
// members the app reads; web-next/PARITY.md), the console commands that change it, the stats lines the real engine logs
// once a second (the Home tiles read them), and a sequencer view (pi/engine/seqview_json.h's shape).

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
    rec: { state: 0, arm: false, ready: true, frames: 0, freeMB: 28120, totalMB: 30436, freeKnown: true, vol: "SD", volOk: true, bits: 24, ch: [0, 1], peak: Array(25).fill(0) },
    fxin: d.fxin,
    stor: { vols: [{ name: "SD", state: "ok", fs: "exFAT", freeMB: 28120, totalMB: 30436, link: "", model: "", pulls: 0 }], exfat: true, bench: { state: "idle" } },
    sdtest: d.sdtest,
    library: { state: "ready", files: 1834, bytes: 2.1e9, hashed: 1834, pending: 0, pct: 100, dups: 3, missing: 0 },
    pair: { gate: "on", rng: true, clients: d.pairClients, sessions: 1, locked: false },
    ins: d.ins,
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
