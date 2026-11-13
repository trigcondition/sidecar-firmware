// web/lfo.js - the LFO tab of both web UIs (core/lfo.h, docs/lfo.md): the Sidecar Mini's console page (web/app.js, an ES
// module) and the Pi's page (pi/engine/www/index.html: build.sh and tests/mock_web.py put web/seg.js and this file in
// place of its include line, the exports dropped). The device's state is core/lfo.cpp's JSON - the Mini's
// @{"t":"lfo","lfo":{...}} answer to ':lfo', the Pi's /status "lfo" - and a change is one console line, lfo=N:KEY:VALUE
// (at most 63 characters: the Mini's console). Choice groups as every tab's (seg.js Pending: pending until the device's
// state shows it). The DESIGN wave's 16 steps are drawn on a canvas: drag over the steps (mouse or touch) with the
// brush's shape, or a preset. The pure parts are tested by node web/lfo.test.mjs.
// 0.5.0 (LFO FINAL, the owner): the Pi's 26 LFOs are two a track ("pt":1 - T1..T12 and FX, LFO 2t+1 the track's A, 2t+2
// its B): the tab shows one track's pair (the track bar picks it; it follows the Rytm's selected track, "trk", when that
// changes), and "full":1 shows LFO_FULL. The Mini ("pt":0): its 4 cards as before. 0.5.0 owner test (2026-11-13): no
// LFO's target list, A or B, has a Rytm target (RT1..RT12, RFX): an LFO on the Rytm made it slow.

export const LFO_WAVES = ["SINE", "TRI", "SAW+", "SAW-", "SQR", "S&H", "RND", "DESIGN"];
export const LFO_RETRIGS = ["FREE", "TRIG", "PLAY"];
export const LFO_SHAPES = ["HOLD", "RAMP", "SMTH"];
export const LFO_PRESETS = ["ramp", "tri", "steps", "rnd"];
// the Sidecar's voices and send FX (1026: the Rytm's own parameters over MIDI CC are no targets any more)
// fx-ins: I1..I8 the voices' INSERT chains, B1..B3 the FX buses' (core/fx/insert.h; params A P1..P4, B P1..P4, then
// ins1112's POST EQ HPF / LPF / LOW / HIGH, the stage after both slots);
// O1..O8 the voices' OUT route (core/fx/out.h, docs/STEREO.md: LEV PAN WID; the Pi only); 0.5.0: M1 M2 the master's
// chains, MFX its LEV / MIX1..4 (and the bus slots' send TYPE MIX B1A..B3B), S1..S12 a Rytm track's sends to FX1 / FX2, X1..X8 a voice's sends to the buses
export const LFO_TARGETS = ["OFF", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "FX1", "FX2",
  "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8", "B1", "B2", "B3", "O1", "O2", "O3", "O4", "O5", "O6", "O7", "O8",
  "M1", "M2", "MFX", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10", "S11", "S12",
  "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8",
  "RT1", "RT2", "RT3", "RT4", "RT5", "RT6", "RT7", "RT8", "RT9", "RT10", "RT11", "RT12", "RFX"];
// 0.5.0: RT1..RT12 / RFX the Rytm's own sound parameters (docs/RYTM-PARAMS.md; core/lfo.cpp kRtWords / kRfxWords)
// the Rytm applies a VEL offset to the trigs its sequencer plays only (rytm-param-mod M8): pads and live play keep theirs
export const VEL_NOTE = "the Rytm's sequenced trigs only: pads and live play keep their velocity";
const RT_PARAMS = ["SPD", "FAD", "SPH", "DEP", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "TUN", "FIN", "BR", "STA",
  "END", "LEV", "FENV", "FATK", "FDEC", "FSUS", "FREL", "FRQ", "RES", "ATK", "HLD", "DEC", "OVR", "VOL", "PAN", "ACC",
  "DEL", "REV", "VM1", "VM2", "VM3", "VM4", "AT1", "AT2", "AT3", "AT4", "VEL"];
const RFX_PARAMS = ["SPD", "FAD", "DEP", "TIM", "X", "WID", "FDB", "DHPF", "DLPF", "DREV", "DVOL", "DOV", "PRE", "RDEC",
  "RFRQ", "GAI", "RHPF", "RLPF", "RVOL", "AMT", "SYM", "THR", "MUP", "MIX", "CVOL"];   // (M0b: the compressor, appended)
export const LFO_FULL = "RYTM LINK FULL: LFOs update slower";
export const LFO_TRACKS = ["T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "T10", "T11", "T12", "FX"];
// LFO n (1-based) of a per-track set: its track (0..12) and slot (0 A, 1 B); the Mini's (pt 0): no track
export const lfoTrack = (n, pt) => (pt ? Math.floor((n - 1) / 2) : -1);
export const lfoSlot = (n, pt) => (pt ? (n - 1) % 2 : 0);
export const lfoTitle = (n, pt) => (pt ? `${LFO_TRACKS[lfoTrack(n, pt)]} LFO ${"AB"[lfoSlot(n, pt)]}` : `LFO ${n}`);
// the targets LFO n may have (core/lfo.cpp target_ok): never the Rytm's (RT1..RT12, RFX: the list's last; the owner's test)
const SIDECAR_TARGETS = LFO_TARGETS.filter((t) => t !== "RFX" && !/^RT\d/.test(t));
export const lfoTargets = (_n, _pt) => SIDECAR_TARGETS;
export const LFO_DIVS = ["1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2",
  "1BART", "1/2D", "1BAR", "1BARD", "2BAR", "4BAR", "8BAR"];

// the parameter list of a target; pn: the device's name for the current one (par), shown beside the number; pns (the
// Pi's /status, 2026-11): every param's name ("-" one the voice's machine / the FX's TYPE leaves unused), shown on
// every row: "P2 HRM", "P9 -"
export function lfoParams(tgt, par, pn, pns) {
  const t = LFO_TARGETS.indexOf(tgt);
  if (t <= 0) return [];
  const named = (base) => base.map((s, k) => {
    const n = Array.isArray(pns) && typeof pns[k] === "string" ? pns[k] : k === par ? pn : "";
    if (/^[A-Z]?-$/.test(n)) return `${s} -`;   // unused ("-"; an INSERT slot OFF: "A-")
    return n && n !== s ? `${s} ${n}` : s;
  });
  if (t < 9) return named(Array.from({ length: 16 }, (_, k) => `P${k + 1}`));
  if (tgt === "RFX") return named(RFX_PARAMS);   // (their own names: the device's equal them)
  if (tgt.startsWith("RT")) return named(RT_PARAMS);
  if (tgt[0] === "O") return ["LEV", "PAN", "WID"];
  if (tgt === "MFX") return ["LEV", "MIX1", "MIX2", "MIX3", "MIX4", "B1A", "B1B", "B2A", "B2B", "B3A", "B3B"];   // B1A: bus 1 slot A's MIX (a send TYPE)
  if (tgt[0] === "S") return ["F1", "F2"];
  if (tgt[0] === "X") return ["B1", "B2", "B3"];
  if (tgt[0] === "I" || tgt[0] === "B" || tgt[0] === "M")   // an INSERT chain: both slots, then the POST EQ (core/lfo.cpp params_of)
    return named(["A P1", "A P2", "A P3", "A P4", "B P1", "B P2", "B P3", "B P4", "EQ HPF", "EQ LPF", "EQ LOW", "EQ HIGH"]);
  return named(Array.from({ length: 4 }, (_, k) => `P${k + 2}`));
}
// the PAR list's rows as {k: the param, name}: a voice's are the params its machine names (core/lfo.cpp voice_params; the
// owner's test: SAMPLER names P9..P16 only, no "P1 -" rows), the stored one (par) kept; a machine naming none: all 16
export function lfoParamRows(tgt, par, pn, pns) {
  const rows = lfoParams(tgt, par, pn, pns).map((name, k) => ({ k, name }));
  if (!/^V\d$/.test(tgt)) return rows;
  const named = rows.filter((r) => !/ -$/.test(r.name) || r.k === par);
  return named.some((r) => !/ -$/.test(r.name)) ? named : rows;
}
export const lfoHz = (rate) => 0.02 * Math.pow(1000, Math.max(0, Math.min(127, rate)) / 127);
export function lfoRateText(sync, rate) {
  if (sync) return LFO_DIVS[Math.max(0, Math.min(LFO_DIVS.length - 1, rate))];
  const c = Math.round(lfoHz(rate) * 100);
  return c >= 1000 ? `${Math.round(c / 100)} Hz` : `${(c / 100).toFixed(2)} Hz`;
}

// the design as core/lfo.cpp sends it: per step the value + 64 (two hex digits) and the shape (one)
export function designDecode(hex) {
  const s = [];
  for (let k = 0; k < 16; k++) {
    const v = parseInt((hex || "").slice(3 * k, 3 * k + 2), 16), sh = parseInt((hex || "").slice(3 * k + 2, 3 * k + 3), 16);
    s.push({ v: Number.isFinite(v) ? Math.min(63, v - 64) : 0, s: Number.isFinite(sh) ? sh % 3 : 0 });
  }
  return s;
}
export const designEncode = (steps) => steps.map((x) => (Math.max(-64, Math.min(63, x.v)) + 64).toString(16).padStart(2, "0") + (x.s % 3).toString(16)).join("");
// the curve at phase ph (0..1), -1..1: core/lfo.cpp design_at
export function designAt(steps, ph) {
  const x = Math.min(0.99999, Math.max(0, ph)) * 16, k = Math.floor(x), f = x - k;
  const a = steps[k].v / 64, b = steps[(k + 1) % 16].v / 64;
  return steps[k].s === 1 ? a + (b - a) * f : steps[k].s === 2 ? a + (b - a) * f * f * (3 - 2 * f) : a;
}
// a drag point (x, y in a w x h box) sets its step's value and the brush's shape; returns the step
export function dragStep(steps, x, y, w, h, shape) {
  const k = Math.max(0, Math.min(15, Math.floor((x / w) * 16)));
  const v = Math.max(-64, Math.min(63, Math.round((0.5 - y / h) * 128)));
  steps[k] = { v, s: shape === undefined ? steps[k].s : shape };
  return k;
}
// ---- the DOM (both pages): a card per LFO in root
export function initLfo({ root, send, Pending, isOn = () => true }) {
  let state = null;            // the device's last JSON {n, pt, kit, full, trk, l: [...]}
  let track = 0, trkSeen;      // the track shown (pt); the device's selected track it followed last
  const drag = new Map();      // i -> steps being drawn (the device's state does not overwrite them)
  const brush = new Map();     // i -> the brush's shape
  const fb = (key, kind, text) => {
    const el = root.querySelector(`[data-lfofb="${key.split(".")[0]}"]`);
    if (el) { el.textContent = kind === "pending" ? "…" : text || ""; el.classList.toggle("err", kind === "err"); }
    paint();
  };
  const pend = new Pending(fb);
  const q = (line) => Promise.resolve(send(line)).catch(() => {});
  const seg = (i, key, names, vals) => `<div class="seg" role="radiogroup" aria-label="${key}" data-lfo="${i}" data-k="${key}" style="flex-wrap:wrap">` +
    names.map((n, j) => `<button type="button" role="radio" aria-checked="false" data-v="${vals ? vals[j] : j}">${n}</button>`).join("") + "</div>";
  const pt = () => (state && state.pt ? 1 : 0);
  const card = (i, p) => `<section class="lfo" data-i="${i}"${p && lfoTrack(i, p) !== track ? " hidden" : ""}><h2>${lfoTitle(i, p)} <span class="fb" data-lfofb="lfo${i}" role="status"></span></h2>
    <div class="row"><label>Target</label><select data-lfo="${i}" data-k="tgt">${lfoTargets(i, p).map((t) => `<option>${t}</option>`).join("")}</select>
      <select data-lfo="${i}" data-k="par"></select></div>
    <div class="row"><label>Wave</label>${seg(i, "wav", LFO_WAVES)}</div>
    <div class="row"><label>Rate</label>${seg(i, "sync", ["Free", "Clock"])} <input type="range" data-lfo="${i}" data-k="rate" min="0" max="127"> <span data-show="rate"></span></div>
    <div class="row"><label>Retrig</label>${seg(i, "trig", ["Free", "Trig", "Play"])}</div>
    <div class="row"><label>Depth</label><input type="range" data-lfo="${i}" data-k="dep" min="-64" max="63"> <span data-show="dep"></span></div>
    <div class="row"><label>Design</label><canvas data-lfo="${i}" width="320" height="96" style="touch-action:none;max-width:100%;border:1px solid var(--line,#888)"></canvas></div>
    <div class="row"><label>Brush</label>${seg(i, "brush", LFO_SHAPES)} ${LFO_PRESETS.map((p) => `<button type="button" data-lfo="${i}" data-pre="${p}">${p}</button>`).join(" ")}</div>
    </section>`;
  const cur = (i) => (state && state.l && state.l[i - 1]) || null;
  function draw(i) {
    const c = root.querySelector(`canvas[data-lfo="${i}"]`), o = cur(i);
    if (!c || !o) return;
    const g = c.getContext && c.getContext("2d");
    if (!g) return;
    const steps = drag.get(i) || designDecode(o.des), w = c.width, h = c.height;
    const css = getComputedStyle(c), fg = css.color || "#ddd";
    g.clearRect(0, 0, w, h);
    g.globalAlpha = 0.35; g.strokeStyle = fg; g.beginPath();
    for (let k = 1; k < 16; k++) { g.moveTo((k * w) / 16, 0); g.lineTo((k * w) / 16, h); }
    g.moveTo(0, h / 2); g.lineTo(w, h / 2); g.stroke();
    g.globalAlpha = 1; g.lineWidth = 2; g.beginPath();
    for (let x = 0; x <= w; x++) { const y = (0.5 - designAt(steps, x / w) / 2) * (h - 4) + 2; x ? g.lineTo(x, y) : g.moveTo(x, y); }
    g.stroke();
    if (o.wav === 7) { g.globalAlpha = 0.6; g.fillRect(((o.ph || 0) / 1000) * w, 0, 2, h); g.globalAlpha = 1; }
  }
  function paint() {
    if (!state) return;
    const full = root.querySelector("[data-lfofull]");
    if (full) full.hidden = !state.full;
    root.querySelectorAll('[data-k="lfotrk"] button').forEach((b) => b.setAttribute("aria-checked", String(+b.dataset.v === track)));
    for (let i = 1; i <= state.n; i++) {
      const o = cur(i), el = root.querySelector(`.lfo[data-i="${i}"]`);
      if (!o || !el) continue;
      const t = el.querySelector('[data-k="tgt"]'), p = el.querySelector('[data-k="par"]');
      if (document.activeElement !== t) t.value = o.tgt;
      const rows = lfoParamRows(o.tgt, o.par, o.pn, o.pns), names = rows.map((r) => `${r.k}:${r.name}`);
      if (p.dataset.list !== names.join()) { p.innerHTML = rows.map(({ k, name: n }) => `<option value="${k}"${n === "VEL" ? ` title="${VEL_NOTE}"` : ""}>${n === "VEL" ? "VEL (SEQ TRIGS)" : n}</option>`).join(""); p.dataset.list = names.join(); }
      if (document.activeElement !== p) p.value = String(o.par);
      p.disabled = !names.length;
      el.querySelectorAll(".seg[data-k]").forEach((g) => {
        const k = g.dataset.k, key = `lfo${i}.${k}`, val = k === "brush" ? String(brush.get(i) || 0) : String(o[k]);
        const want = pend.want(key);
        g.classList.toggle("pending", want !== undefined);
        g.querySelectorAll("button").forEach((b) => {
          b.setAttribute("aria-checked", String(b.dataset.v === val));
          b.classList.toggle("pend", want === b.dataset.v);
          b.disabled = !isOn();
        });
      });
      for (const k of ["rate", "dep"]) {
        const r = el.querySelector(`input[data-k="${k}"]`);
        if (k === "rate") r.max = o.sync ? String(LFO_DIVS.length - 1) : "127";
        if (document.activeElement !== r) r.value = String(o[k]);
        el.querySelector(`[data-show="${k}"]`).textContent = k === "rate" ? lfoRateText(o.sync, +r.value) : k === "dep" ? (+r.value > 0 ? "+" : "") + r.value : r.value;
      }
      draw(i);
    }
  }
  function showTrack(t) {   // the track's pair (pt): the other cards hidden
    track = t;
    root.querySelectorAll(".lfo[data-i]").forEach((el) => { el.hidden = lfoTrack(+el.dataset.i, pt()) !== track; });
    paint();
  }
  function build(n) {
    const p = pt();
    root.innerHTML = `<p class="lfo-full err" data-lfofull role="status" hidden>${LFO_FULL}</p>` +
      (p ? `<div class="row"><label>Track</label><div class="seg" role="radiogroup" aria-label="Track" data-k="lfotrk" style="flex-wrap:wrap">` +
        LFO_TRACKS.map((t, k) => `<button type="button" role="radio" aria-checked="${k === track}" data-v="${k}">${t}</button>`).join("") + "</div></div>" : "") +
      Array.from({ length: n }, (_, k) => card(k + 1, p)).join("");
    root.querySelectorAll("canvas[data-lfo]").forEach((c) => {
      const i = +c.dataset.lfo;
      const at = (e) => { const r = c.getBoundingClientRect(); return [((e.clientX - r.left) / r.width) * c.width, ((e.clientY - r.top) / r.height) * c.height]; };
      c.onpointerdown = (e) => {
        const o = cur(i);
        if (!o || !isOn()) return;
        c.setPointerCapture && c.setPointerCapture(e.pointerId);
        const s = designDecode(o.des);
        drag.set(i, s);
        dragStep(s, ...at(e), c.width, c.height, brush.get(i) || 0);
        draw(i);
      };
      c.onpointermove = (e) => { const s = drag.get(i); if (s) { dragStep(s, ...at(e), c.width, c.height, brush.get(i) || 0); draw(i); } };
      c.onpointerup = c.onpointercancel = () => {
        const s = drag.get(i);
        if (!s) return;
        const o = cur(i), hex = designEncode(s);
        if (o) o.des = hex;   // shown until the device's state confirms it
        drag.delete(i);
        q(`lfo=${i}:des:${hex}`);
        if (o && o.wav !== 7) { pend.start(`lfo${i}.wav`, "7"); q(`lfo=${i}:wav:7`); }
      };
    });
  }
  root.addEventListener("click", (e) => {
    const b = e.target.closest("button");
    if (!b) return;
    const g = b.closest(".seg[data-k]");
    if (g && g.dataset.k === "lfotrk") { showTrack(+b.dataset.v); return; }   // (a view choice: never a command)
    if (!isOn()) return;
    if (g) {
      const i = +g.dataset.lfo, k = g.dataset.k;
      if (k === "brush") { brush.set(i, +b.dataset.v); paint(); return; }
      pend.start(`lfo${i}.${k}`, b.dataset.v);
      q(`lfo=${i}:${k}:${b.dataset.v}`);
    } else if (b.dataset.pre) {
      const i = +b.dataset.lfo;
      pend.start(`lfo${i}.wav`, "7");
      q(`lfo=${i}:pre:${b.dataset.pre}`);
    }
  });
  root.addEventListener("change", (e) => {
    const t = e.target, i = +t.dataset.lfo, k = t.dataset.k;
    if (!i || !k) return;
    if (k === "tgt") q(t.value === "OFF" ? `lfo=${i}:off` : `lfo=${i}:tgt:${t.value.toLowerCase()}`);
    else q(`lfo=${i}:${k}:${t.value}`);
  });
  root.addEventListener("input", (e) => {   // the slider's label follows the thumb
    const t = e.target, el = t.closest && t.closest(".lfo");
    if (!el || t.type !== "range") return;
    const o = cur(+el.dataset.i) || {};
    const k = t.dataset.k;
    el.querySelector(`[data-show="${k}"]`).textContent = k === "rate" ? lfoRateText(o.sync, +t.value) : t.value;
  });
  return {
    show(lfo) {   // the device's state (the JSON's "lfo" object)
      if (!lfo || !lfo.l) return;
      const rebuild = !state || state.n !== lfo.n || !!state.pt !== !!lfo.pt;
      state = lfo;
      if (lfo.pt && lfo.trk !== undefined && lfo.trk !== trkSeen) {   // the Rytm selected another track: its pair
        trkSeen = lfo.trk;
        if (lfo.trk >= 0 && lfo.trk < LFO_TRACKS.length) track = lfo.trk;
        if (!rebuild) showTrack(track);
      }
      if (rebuild) build(lfo.n);
      lfo.l.forEach((o, k) => { for (const key of ["wav", "sync", "trig"]) pend.report(`lfo${k + 1}.${key}`, String(o[key]), ""); });
      paint();
    },
    state: () => state,
    track: () => track,
    showTrack,
  };
}
