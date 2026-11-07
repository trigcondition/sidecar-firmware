<!-- Voices › LFO › the DESIGN wave's 16 steps, drawn: drag over the steps (mouse, pen or touch) with the brush's shape
     (HOLD, RAMP, SMTH: how a step goes on to the next), as the old page's canvas (web/lfo.js). Keyboard: Left/Right pick a
     step, Up/Down change it (Shift: by 1, else by 8), Home/End the ends. The drawing goes out when let go (one
     command, lfo=N:des:<hex>); it shows at once and holds until the Sidecar reports it (lib/held.ts). -->
<script setup lang="ts">
import { computed, ref } from "vue";
import { useHeld } from "../../lib/held";
import { designAt, designDecode, designEncode, dragStep, type DesignStep } from "../../lib/lfo";

const props = defineProps<{ des: string; brush: number; ph?: number; disabled?: boolean }>();
const emit = defineEmits<{ commit: [string] }>();
const W = 320, H = 96;

const held = useHeld(() => props.des);
const drawing = ref<DesignStep[] | null>(null);
const steps = computed<DesignStep[]>(() => drawing.value ?? designDecode(held.shown.value));
const path = computed(() => {
  const s = steps.value;
  let d = "";
  for (let x = 0; x <= W; x += 2) d += `${x ? " L" : "M"}${x} ${((0.5 - designAt(s, x / W) / 2) * (H - 4) + 2).toFixed(1)}`;
  return d;
});
const dots = computed(() => steps.value.map((st, k) => ({ x: ((k + 0.5) * W) / 16, y: (0.5 - st.v / 128) * H })));

const svg = ref<SVGSVGElement | null>(null);
function at(e: PointerEvent): [number, number] {
  const r = svg.value!.getBoundingClientRect();
  return [((e.clientX - r.left) / r.width) * W, ((e.clientY - r.top) / r.height) * H];
}
function down(e: PointerEvent) {
  if (props.disabled) return;
  (e.currentTarget as Element).setPointerCapture?.(e.pointerId);
  const s = designDecode(held.shown.value);
  dragStep(s, ...at(e), W, H, props.brush);
  drawing.value = s;
}
function move(e: PointerEvent) {
  if (!drawing.value) return;
  const s = [...drawing.value];
  dragStep(s, ...at(e), W, H, props.brush);
  drawing.value = s;
}
function up() {
  if (!drawing.value) return;
  send(drawing.value);
  drawing.value = null;
}
function send(s: DesignStep[]) {
  const hex = designEncode(s);
  held.hold(hex);
  emit("commit", hex);
}

// the keyboard: one step at a time, sent after a short pause so holding a key is one command
const cur = ref(0);
let keyTimer: ReturnType<typeof setTimeout> | undefined;
function key(e: KeyboardEvent) {
  if (props.disabled) return;
  const s = designDecode(held.shown.value);
  const by = e.shiftKey ? 1 : 8;
  if (e.key === "ArrowLeft") cur.value = Math.max(0, cur.value - 1);
  else if (e.key === "ArrowRight") cur.value = Math.min(15, cur.value + 1);
  else if (e.key === "Home") cur.value = 0;
  else if (e.key === "End") cur.value = 15;
  else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
    const k = cur.value;
    s[k] = { v: Math.max(-64, Math.min(63, s[k].v + (e.key === "ArrowUp" ? by : -by))), s: props.brush };
    held.hold(designEncode(s));
    clearTimeout(keyTimer);
    keyTimer = setTimeout(() => emit("commit", designEncode(s)), 350);
  } else return;
  e.preventDefault();
}
const valueText = computed(() => `Step ${cur.value + 1} of 16: ${steps.value[cur.value].v}`);
</script>

<template>
  <div class="pad">
    <svg
      ref="svg"
      :viewBox="`0 0 ${W} ${H}`"
      class="draw"
      :class="{ disabled }"
      role="slider"
      tabindex="0"
      aria-label="Design the wave: drag over the steps, or Left/Right and Up/Down"
      :aria-valuetext="valueText"
      :aria-valuenow="steps[cur].v"
      aria-valuemin="-64"
      aria-valuemax="63"
      :aria-disabled="disabled"
      @pointerdown="down"
      @pointermove="move"
      @pointerup="up"
      @pointercancel="up"
      @keydown="key"
    >
      <g class="grid">
        <line v-for="k in 15" :key="k" :x1="(k * W) / 16" :x2="(k * W) / 16" y1="0" :y2="H" />
        <line x1="0" :x2="W" :y1="H / 2" :y2="H / 2" class="mid" />
      </g>
      <rect class="cur" :x="(cur * W) / 16" y="0" :width="W / 16" :height="H" />
      <path :d="path" class="wave" />
      <circle v-for="(d, k) in dots" :key="k" :cx="d.x" :cy="d.y" r="3" class="dot" />
      <rect v-if="ph !== undefined" :x="(ph / 1000) * W" y="0" width="2" :height="H" class="ph" />
    </svg>
  </div>
</template>

<style scoped>
.pad { width: 100%; }
.draw { display: block; width: 100%; height: auto; aspect-ratio: 320 / 96; touch-action: none; cursor: crosshair; border: 1px solid var(--control-border); border-radius: var(--radius-control); background: var(--bg); }
.draw.disabled { cursor: not-allowed; opacity: 0.6; }
.draw:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.grid line { stroke: var(--border); stroke-width: 1; }
.grid .mid { stroke: var(--control-border); }
.cur { fill: var(--accent-tint); opacity: 0; }
.draw:focus-visible .cur { opacity: 1; }
.wave { fill: none; stroke: var(--accent); stroke-width: 2.5; }
.dot { fill: var(--text); }
.ph { fill: var(--warm); opacity: 0.7; }
</style>
