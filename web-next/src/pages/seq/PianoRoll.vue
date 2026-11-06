<!-- web-next/src/pages/seq/PianoRoll.vue - the shown seq track's notes over a window of steps and pitches, the play head,
     a velocity lane. Pointer: a tap on a note selects it; on an empty cell it adds a note while drawing is on.
     Keyboard (the roll takes focus): arrows move the cursor, Enter adds or selects, Delete removes the selected note,
     Shift+arrows move it, + / - its velocity, [ / ] its length; the cell under the cursor is spoken. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { SUB, isBlack, noteAt, noteName, type SeqNote } from "../../lib/seq";

const props = defineProps<{
  notes: SeqNote[];
  first: number; // the first step shown
  count: number; // steps shown
  top: number; // the top pitch shown
  rows: number;
  play: number | null; // the play head in steps
  draw: boolean;
  selected: SeqNote | null;
  length: number; // the track's length in steps
  rowRem?: number; // a row's height (1.5 rem; 2.75 for the large cells: 44 px targets)
}>();
const emit = defineEmits<{
  select: [SeqNote | null];
  add: [step: number, pitch: number];
  remove: [SeqNote];
  nudge: [SeqNote, dStep: number, dPitch: number];
  vel: [SeqNote, delta: number];
  len: [SeqNote, delta: number];
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
const isSel = (n: SeqNote) => !!props.selected && n.step === props.selected.step && n.pitch === props.selected.pitch;

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
  if (!n) return `${where}: empty${props.draw ? "" : " (drawing is off)"}`;
  return `${where}: note${isSel(n) ? ", selected" : ""}, velocity ${n.vel}, ${+(n.len / SUB).toFixed(2)} steps${n.step !== c.step ? `, from step ${n.step + 1}` : ""}`;
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
  if (k === "Enter" || k === " ") {
    handled();
    const n = under.value;
    if (n) emit("select", isSel(n) ? null : n);
    else if (props.draw) emit("add", cur.value.step, cur.value.pitch);
    return;
  }
  if ((k === "Delete" || k === "Backspace") && sel) return handled(), emit("remove", sel);
  if ((k === "+" || k === "=") && sel) return handled(), emit("vel", sel, 8);
  if (k === "-" && sel) return handled(), emit("vel", sel, -8);
  if (k === "]" && sel) return handled(), emit("len", sel, SUB / 4);
  if (k === "[" && sel) return handled(), emit("len", sel, -SUB / 4);
  if (k === "Escape" && sel) return handled(), emit("select", null);
}

function tap(e: PointerEvent) {
  const svg = e.currentTarget as SVGSVGElement;
  const r = svg.getBoundingClientRect();
  const step = props.first + Math.floor(((e.clientX - r.left) / r.width) * props.count);
  const pitch = props.top - Math.floor(((e.clientY - r.top) / r.height) * props.rows);
  if (step < 0 || step >= props.length) return;
  cur.value = { step, pitch };
  const n = noteAt(props.notes, step, pitch);
  if (n) emit("select", isSel(n) ? null : n);
  else if (props.draw) emit("add", step, pitch);
  else emit("select", null);
}
</script>

<template>
  <div
    class="roll"
    tabindex="0"
    role="application"
    aria-roledescription="piano roll"
    aria-label="Notes. Arrows move, Enter adds or selects, Delete removes, Shift and arrows move the selected note, plus and minus its velocity, brackets its length."
    @keydown="key"
    @focus="focused = true"
    @blur="focused = false"
  >
    <div class="keys" aria-hidden="true">
      <div v-for="p in pitches" :key="p" class="key" :class="{ black: isBlack(p), c: p % 12 === 0 }" :style="{ height: `${rowRem ?? 1.5}rem` }">{{ p % 12 === 0 || p === top ? noteName(p) : "" }}</div>
    </div>
    <div class="grid">
      <svg :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="none" :style="{ height: `${rows * (rowRem ?? 1.5)}rem` }" @pointerdown="tap">
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
.roll { display: grid; grid-template-columns: 3rem 1fr; gap: 0 0.25rem; border-radius: var(--radius-control); }
.roll:focus-visible { outline: 3px solid var(--focus); outline-offset: 2px; }
.keys { display: flex; flex-direction: column; }
.key { height: 1.5rem; font-size: var(--text-min); color: var(--label); display: flex; align-items: center; justify-content: flex-end; padding-right: 0.25rem; border-bottom: 1px solid var(--border); }
.key.black { background: color-mix(in srgb, var(--card) 70%, var(--bg)); }
.key.c { color: var(--text); font-weight: 700; }
.grid { min-width: 0; display: flex; flex-direction: column; }
.grid svg { width: 100%; display: block; touch-action: manipulation; }
.row-w { fill: var(--card); }
.row-b { fill: color-mix(in srgb, var(--card) 60%, var(--bg)); }
.tick { stroke: var(--border); stroke-width: 1; }
.beat { stroke: var(--control-border); stroke-width: 1; }
.note { fill: var(--accent); }
.note.sel { fill: var(--text); stroke: var(--focus); stroke-width: 3; opacity: 1 !important; }
.note.muted { fill: var(--control-border); }
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
