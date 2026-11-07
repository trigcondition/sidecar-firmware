<!-- web-next/src/pages/seq/PianoRoll.vue - the shown seq track's notes over a window of steps and pitches, the play head,
     a velocity lane. Pointer: a tap on a note selects it; on an empty cell it adds a note while drawing is on; while
     drawing is on a note drags to another step or pitch, or by its end to another length (a ghost shows where it goes).
     Keyboard (the roll takes focus): arrows move the cursor, Enter adds or selects, Delete removes the selected note,
     Shift+arrows move it, + / - its velocity, [ / ] its length; the cell under the cursor is spoken.
     Several notes: in Select mode a drag on empty space draws a rectangle (a mouse or pen; a finger while Multi is
     on), Shift / Cmd / Ctrl and a click (or Shift+Enter) puts a note in or takes it out; a selected group drags,
     moves, deletes and changes velocity and length as one (Seq.vue sends a line per note). -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { SUB, dragResult, grabsEnd, isBlack, noteAt, noteName, type DragMode, type SeqNote } from "../../lib/seq";
import { groupDelta, inBox, sameKey, type RollPoint } from "../../lib/seqsel";

const props = defineProps<{
  notes: SeqNote[];
  first: number; // the first step shown
  count: number; // steps shown
  top: number; // the top pitch shown
  rows: number;
  play: number | null; // the play head in steps
  draw: boolean;
  selected: SeqNote | null; // the note last selected (the one the keys act on)
  group?: SeqNote[]; // every selected note (the selected one among them)
  multi?: boolean; // Select mode's Multi: taps put notes in or take them out, a finger draws the rectangle
  length: number; // the track's length in steps
  rowRem?: number; // a row's height (1.5 rem; 2.75 for the large cells: 44 px targets)
  cellRem?: number; // a step's least width (a phone: 2.75 rem, 44 px; the roll then scrolls sideways, the keys stay)
}>();
const emit = defineEmits<{
  select: [SeqNote | null];
  toggle: [SeqNote];
  pick: [notes: SeqNote[], add: boolean];
  add: [step: number, pitch: number];
  remove: [SeqNote];
  nudge: [SeqNote, dStep: number, dPitch: number];
  vel: [SeqNote, delta: number];
  len: [SeqNote, delta: number];
  lenTo: [SeqNote, len: number];
  scroll: [dPitch: number, dStep: number];
}>();

const CW = 40, RH = 24; // a cell in the SVG's units
const W = computed(() => props.count * CW);
const H = computed(() => props.rows * RH);
const pitches = computed(() => Array.from({ length: props.rows }, (_, i) => props.top - i));
const shown = computed(() =>
  props.notes.filter((n) => n.pitch <= props.top && n.pitch > props.top - props.rows && n.step * SUB + n.micro + n.len > props.first * SUB && n.step < props.first + props.count),
);
const x = (stepSub: number) => ((stepSub - props.first * SUB) / SUB) * CW;
const y = (p: number) => (props.top - p) * RH;
const group = computed(() => props.group ?? []);
const isSel = (n: SeqNote) => (!!props.selected && sameKey(n, props.selected)) || group.value.some((g) => sameKey(g, n));
// the only note selected: a tap on it again lets it go (as before); a tap on one of several keeps just that one
const isOnly = (n: SeqNote) => isSel(n) && group.value.length <= 1;
const inGroup = (n: SeqNote) => group.value.length > 1 && isSel(n);

// the keyboard cursor
const cur = ref({ step: props.first, pitch: props.top - Math.floor(props.rows / 2) });
const focused = ref(false);
watch(
  () => [props.first, props.count, props.top, props.rows],
  () => {
    cur.value.step = Math.max(props.first, Math.min(props.first + props.count - 1, cur.value.step));
    cur.value.pitch = Math.max(props.top - props.rows + 1, Math.min(props.top, cur.value.pitch));
  },
);
const under = computed(() => noteAt(props.notes, cur.value.step, cur.value.pitch));
const spoken = computed(() => {
  const c = cur.value, n = under.value;
  const where = `Step ${c.step + 1}, ${noteName(c.pitch)}`;
  const many = group.value.length > 1 ? `; ${group.value.length} notes selected` : "";
  if (!n) return `${where}: empty${props.draw ? "" : " (drawing is off)"}${many}`;
  return `${where}: note${isSel(n) ? ", selected" : ""}, velocity ${n.vel}, ${+(n.len / SUB).toFixed(2)} steps${n.step !== c.step ? `, from step ${n.step + 1}` : ""}${many}`;
});

// the keyboard cursor kept in sight when the roll scrolls sideways
const rollEl = ref<HTMLElement | null>(null);
watch(() => cur.value.step, (s) => {
  const r = rollEl.value, g = r?.querySelector<HTMLElement>(".grid");
  if (!r || !g || r.scrollWidth <= r.clientWidth) return;
  const w = g.offsetWidth / props.count, x0 = g.offsetLeft + (s - props.first) * w, keys = g.offsetLeft;
  if (x0 < r.scrollLeft + keys) r.scrollLeft = x0 - keys;
  else if (x0 + w > r.scrollLeft + r.clientWidth) r.scrollLeft = x0 + w - r.clientWidth;
});

function move(ds: number, dp: number) {
  const c = cur.value;
  let s = c.step + ds, p = c.pitch + dp;
  if (s < props.first || s >= props.first + props.count) emit("scroll", 0, ds);
  if (p > props.top || p <= props.top - props.rows) emit("scroll", dp, 0);
  s = Math.max(0, Math.min(props.length - 1, s));
  p = Math.max(0, Math.min(127, p));
  cur.value = { step: s, pitch: p };
}
function key(e: KeyboardEvent) {
  const sel = props.selected;
  const k = e.key;
  const handled = () => e.preventDefault();
  if (e.shiftKey && sel && k.startsWith("Arrow")) {
    handled();
    const ds = k === "ArrowLeft" ? -1 : k === "ArrowRight" ? 1 : 0, dp = k === "ArrowUp" ? 1 : k === "ArrowDown" ? -1 : 0;
    emit("nudge", sel, ds, dp);
    move(ds, dp);
    return;
  }
  if (k === "ArrowLeft") return handled(), move(-1, 0);
  if (k === "ArrowRight") return handled(), move(1, 0);
  if (k === "ArrowUp") return handled(), move(0, 1);
  if (k === "ArrowDown") return handled(), move(0, -1);
  if (k === "PageUp") return handled(), move(0, 12);
  if (k === "PageDown") return handled(), move(0, -12);
  if ((k === "Enter" || k === " ") && e.shiftKey) {
    handled();
    if (under.value) emit("toggle", under.value);
    return;
  }
  if (k === "Enter" || k === " ") {
    handled();
    const n = under.value;
    if (n) emit("select", isOnly(n) ? null : n);
    else if (props.draw) emit("add", cur.value.step, cur.value.pitch);
    return;
  }
  if ((k === "Delete" || k === "Backspace") && sel) return handled(), emit("remove", sel);
  if ((k === "+" || k === "=") && sel) return handled(), emit("vel", sel, 8);
  if (k === "-" && sel) return handled(), emit("vel", sel, -8);
  if (k === "]" && sel) return handled(), emit("len", sel, SUB / 4);
  if (k === "[" && sel) return handled(), emit("len", sel, -SUB / 4);
  if (k === "Escape" && (sel || group.value.length)) return handled(), emit("select", null);
}

// the pointer: a press on a note while drawing starts a drag (moved past a few pixels: a drag, else a tap); a press on
// one of several selected notes drags them all (either mode); in Select mode a press on empty space may draw a rectangle
interface Drag { n: SeqNote; mode: DragMode; group: boolean; x0: number; y0: number; cw: number; rh: number; moved: boolean; id: number }
let drag: Drag | null = null;
interface Ghost { step: number; micro: number; pitch: number; len: number }
const ghosts = ref<Ghost[]>([]);
interface Band { a: RollPoint; b: RollPoint; add: boolean; x0: number; y0: number; moved: boolean; id: number; step: number; pitch: number }
let bandDrag: Band | null = null;
const band = ref<{ a: RollPoint; b: RollPoint } | null>(null);
function pointOf(e: PointerEvent, svg: SVGSVGElement): RollPoint {
  const r = svg.getBoundingClientRect();
  return { s: props.first + ((e.clientX - r.left) / r.width) * props.count, p: props.top + 1 - ((e.clientY - r.top) / r.height) * props.rows };
}
// a group drag of (dx, dy) cells, cut short so every note stays inside the track and 0..127
const groupMove = (dx: number, dy: number) => groupDelta(group.value, Math.round(dx), -Math.round(dy), props.length);
function cellOf(e: PointerEvent, svg: SVGSVGElement) {
  const r = svg.getBoundingClientRect();
  const fx = ((e.clientX - r.left) / r.width) * props.count, fy = ((e.clientY - r.top) / r.height) * props.rows;
  return { r, step: props.first + Math.floor(fx), sub: Math.round((props.first + fx) * SUB), pitch: props.top - Math.floor(fy) };
}
function down(e: PointerEvent) {
  const svg = e.currentTarget as SVGSVGElement;
  const c = cellOf(e, svg);
  if (e.button !== 0) return tap(e);
  const hit = noteAt(props.notes, c.step, c.pitch);
  const add = e.shiftKey || e.metaKey || e.ctrlKey || !!props.multi;
  if (hit && add) {
    cur.value = { step: c.step, pitch: c.pitch };
    return emit("toggle", hit);
  }
  const grab = (n: SeqNote, mode: DragMode, g: boolean) => {
    drag = { n, mode, group: g, x0: e.clientX, y0: e.clientY, cw: c.r.width / props.count, rh: c.r.height / props.rows, moved: false, id: e.pointerId };
    svg.setPointerCapture?.(e.pointerId);
  };
  if (hit && inGroup(hit)) return grab(hit, "move", true);
  if (hit && props.draw) return grab(hit, grabsEnd(hit, c.sub) ? "len" : "move", false);
  // a finger on empty space scrolls the roll, unless Multi is on
  if (!hit && !props.draw && (e.pointerType !== "touch" || props.multi)) {
    const at = pointOf(e, svg);
    bandDrag = { a: at, b: at, add, x0: e.clientX, y0: e.clientY, moved: false, id: e.pointerId, step: c.step, pitch: c.pitch };
    svg.setPointerCapture?.(e.pointerId);
    return;
  }
  tap(e);
}
function moveTo(e: PointerEvent) {
  if (bandDrag && e.pointerId === bandDrag.id) {
    if (!bandDrag.moved && Math.hypot(e.clientX - bandDrag.x0, e.clientY - bandDrag.y0) < 6) return;
    bandDrag.moved = true;
    bandDrag.b = pointOf(e, e.currentTarget as SVGSVGElement);
    band.value = { a: bandDrag.a, b: bandDrag.b };
    return;
  }
  if (!drag || e.pointerId !== drag.id) return;
  const dx = e.clientX - drag.x0, dy = e.clientY - drag.y0;
  if (!drag.moved && Math.hypot(dx, dy) < 6) return;
  drag.moved = true;
  if (drag.group) {
    const d = groupMove(dx / drag.cw, dy / drag.rh);
    ghosts.value = group.value.map((n) => ({ step: n.step + d.ds, micro: n.micro, pitch: n.pitch + d.dp, len: n.len }));
    return;
  }
  const r = dragResult(drag.n, drag.mode, dx / drag.cw, dy / drag.rh, props.length), n = drag.n;
  ghosts.value = [!r ? { step: n.step, micro: n.micro, pitch: n.pitch, len: n.len }
    : "len" in r ? { step: n.step, micro: n.micro, pitch: n.pitch, len: r.len }
    : { step: n.step + r.ds, micro: n.micro, pitch: n.pitch + r.dp, len: n.len }];
}
function up(e: PointerEvent) {
  if (bandDrag && e.pointerId === bandDrag.id) {
    const b = bandDrag;
    bandDrag = null;
    band.value = null;
    if (b.moved) return emit("pick", inBox(props.notes, b.a, pointOf(e, e.currentTarget as SVGSVGElement)), b.add);
    // a click on empty space: the selection goes (kept while adding)
    if (b.step < 0 || b.step >= props.length) return;
    cur.value = { step: b.step, pitch: b.pitch };
    if (!b.add) emit("select", null);
    return;
  }
  if (!drag || e.pointerId !== drag.id) return;
  const d = drag;
  drag = null;
  ghosts.value = [];
  if (!d.moved) {
    cur.value = { step: d.n.step, pitch: d.n.pitch };
    return emit("select", isOnly(d.n) ? null : d.n);
  }
  if (d.group) {
    const g = groupMove((e.clientX - d.x0) / d.cw, (e.clientY - d.y0) / d.rh);
    if (!g.ds && !g.dp) return;
    emit("nudge", d.n, g.ds, g.dp);
    cur.value = { step: d.n.step + g.ds, pitch: d.n.pitch + g.dp };
    return;
  }
  const r = dragResult(d.n, d.mode, (e.clientX - d.x0) / d.cw, (e.clientY - d.y0) / d.rh, props.length);
  if (!r) return;
  if ("len" in r) emit("lenTo", d.n, r.len);
  else {
    emit("nudge", d.n, r.ds, r.dp);
    cur.value = { step: d.n.step + r.ds, pitch: d.n.pitch + r.dp };
  }
}
function cancel() {
  drag = null;
  ghosts.value = [];
  bandDrag = null;
  band.value = null;
}

function tap(e: PointerEvent) {
  const svg = e.currentTarget as SVGSVGElement;
  const r = svg.getBoundingClientRect();
  const step = props.first + Math.floor(((e.clientX - r.left) / r.width) * props.count);
  const pitch = props.top - Math.floor(((e.clientY - r.top) / r.height) * props.rows);
  if (step < 0 || step >= props.length) return;
  cur.value = { step, pitch };
  const n = noteAt(props.notes, step, pitch);
  if (n) emit("select", isOnly(n) ? null : n);
  else if (props.draw) emit("add", step, pitch);
  else emit("select", null);
}
</script>

<template>
  <div
    ref="rollEl"
    class="roll"
    tabindex="0"
    role="application"
    aria-roledescription="piano roll"
    aria-label="Notes. Arrows move, Enter adds or selects, Delete removes, Shift and arrows move the selected note, plus and minus its velocity, brackets its length. Shift and Enter puts the note under the cursor in the selection or takes it out."
    @keydown="key"
    @focus="focused = true"
    @blur="focused = false"
  >
    <div class="keys" aria-hidden="true">
      <div v-for="p in pitches" :key="p" class="key" :class="{ black: isBlack(p), c: p % 12 === 0 }" :style="{ height: `${rowRem ?? 1.5}rem` }">{{ p % 12 === 0 || p === top ? noteName(p) : "" }}</div>
    </div>
    <div class="grid" :style="cellRem ? { minWidth: `${count * cellRem}rem` } : undefined">
      <svg :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="none" :class="{ drawing: draw, grouped: group.length > 1, multi }" :style="{ height: `${rows * (rowRem ?? 1.5)}rem` }" @pointerdown="down" @pointermove="moveTo" @pointerup="up" @pointercancel="cancel">
        <rect v-for="(p, i) in pitches" :key="'r' + p" x="0" :y="i * RH" :width="W" :height="RH" :class="isBlack(p) ? 'row-b' : 'row-w'" />
        <line v-for="s in count + 1" :key="'s' + s" :x1="(s - 1) * CW" :x2="(s - 1) * CW" y1="0" :y2="H" :class="(first + s - 1) % 4 === 0 ? 'beat' : 'tick'" vector-effect="non-scaling-stroke" />
        <rect
          v-for="n in shown"
          :key="`${n.step}:${n.pitch}`"
          :x="x(n.step * SUB + n.micro)"
          :y="y(n.pitch) + 3"
          :width="Math.max(4, (n.len / SUB) * CW - 2)"
          :height="RH - 6"
          rx="3"
          :class="['note', { sel: isSel(n), muted: n.flags & 1 }]"
          :style="{ opacity: 0.45 + (0.55 * n.vel) / 127 }"
        />
        <rect
          v-for="(ghost, i) in ghosts"
          :key="'g' + i"
          :x="x(ghost.step * SUB + ghost.micro)"
          :y="y(ghost.pitch) + 3"
          :width="Math.max(4, (ghost.len / SUB) * CW - 2)"
          :height="RH - 6"
          rx="3"
          class="ghost"
          vector-effect="non-scaling-stroke"
        />
        <rect
          v-if="band"
          :x="(Math.min(band.a.s, band.b.s) - first) * CW"
          :y="(top + 1 - Math.max(band.a.p, band.b.p)) * RH"
          :width="Math.abs(band.a.s - band.b.s) * CW"
          :height="Math.abs(band.a.p - band.b.p) * RH"
          class="band"
          vector-effect="non-scaling-stroke"
        />
        <line v-if="play !== null && play >= first && play < first + count" :x1="x(play * SUB)" :x2="x(play * SUB)" y1="0" :y2="H" class="head" vector-effect="non-scaling-stroke" />
        <rect
          v-if="focused && cur.step >= first && cur.step < first + count"
          :x="(cur.step - first) * CW + 1"
          :y="y(cur.pitch) + 1"
          :width="CW - 2"
          :height="RH - 2"
          class="cursor"
          vector-effect="non-scaling-stroke"
        />
      </svg>
      <svg class="lane" :viewBox="`0 0 ${W} 60`" preserveAspectRatio="none" aria-hidden="true">
        <rect
          v-for="n in shown"
          :key="'v' + n.step + ':' + n.pitch"
          :x="x(n.step * SUB + n.micro) + 2"
          :y="60 - (n.vel / 127) * 56"
          width="8"
          :height="(n.vel / 127) * 56"
          :class="['vbar', { sel: isSel(n) }]"
        />
      </svg>
      <div class="steps" aria-hidden="true">
        <span v-for="s in count" :key="s" :class="{ beat: (first + s - 1) % 4 === 0 }">{{ (first + s - 1) % 4 === 0 ? first + s : "" }}</span>
      </div>
    </div>
    <p class="visually-hidden" aria-live="polite">{{ focused ? spoken : "" }}</p>
  </div>
</template>

<style scoped>
.roll { display: grid; grid-template-columns: 3rem 1fr; gap: 0 0.25rem; border-radius: var(--radius-control); overflow-x: auto; overscroll-behavior-x: contain; }
.roll:focus-visible { outline: 3px solid var(--focus); outline-offset: 2px; }
.keys { display: flex; flex-direction: column; position: sticky; left: 0; z-index: 1; background: var(--card); }
.key { height: 1.5rem; font-size: var(--text-min); color: var(--label); display: flex; align-items: center; justify-content: flex-end; padding-right: 0.25rem; border-bottom: 1px solid var(--border); }
.key.black { background: color-mix(in srgb, var(--card) 70%, var(--bg)); }
.key.c { color: var(--text); font-weight: 700; }
.grid { min-width: 0; display: flex; flex-direction: column; }
.grid svg { width: 100%; display: block; touch-action: pan-x pan-y; }
.row-w { fill: var(--card); }
.row-b { fill: color-mix(in srgb, var(--card) 60%, var(--bg)); }
.tick { stroke: var(--border); stroke-width: 1; }
.beat { stroke: var(--control-border); stroke-width: 1; }
.note { fill: var(--accent); }
.note.sel { fill: var(--text); stroke: var(--focus); stroke-width: 3; opacity: 1 !important; }
.note.muted { fill: var(--control-border); }
/* while drawing, a press on a note is the note's (a drag), not the page's scroll; so is a press on a selected group's
   note, and with Multi on every press is the roll's (taps put notes in, a drag draws the rectangle) */
.drawing .note, .grouped .note.sel { touch-action: none; cursor: grab; }
.grid svg.multi { touch-action: none; }
.band { fill: color-mix(in srgb, var(--focus) 15%, transparent); stroke: var(--focus); stroke-width: 2; stroke-dasharray: 4 3; pointer-events: none; }
.ghost { fill: none; stroke: var(--focus); stroke-width: 2; stroke-dasharray: 4 3; pointer-events: none; }
.head { stroke: var(--warm); stroke-width: 2; }
.cursor { fill: none; stroke: var(--focus); stroke-width: 3; }
.lane { height: 3rem; border-top: 1px solid var(--border); }
.vbar { fill: var(--accent); }
.vbar.sel { fill: var(--text); }
.steps { display: grid; grid-auto-flow: column; grid-auto-columns: 1fr; font-size: var(--text-min); color: var(--label); }
.steps .beat { color: var(--text); }
:root[data-theme="contrast"] .roll .note { fill: var(--text); }
:root[data-theme="contrast"] .roll .note.sel { fill: var(--accent); }
</style>
