<!-- web-next/src/components/SignalTide.vue - Sidecar's signature header band: halftone dots, one column per slice of
     the master level (the newest on the right), an orange layer offset ~2 px at 30 %, a torn bottom edge.
     Decorative (aria-hidden: the level is a number on Home and Mix). prefers-reduced-motion: a still band, not moving.
     High-contrast theme: a plain level line instead of dots. -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from "vue";

const props = withDefaults(defineProps<{ level?: number }>(), { level: 0 });
const W = 1280, H = 124, COLS = 107, ROWS = 9, PITCH = 12;

const history = ref<number[]>(Array.from({ length: COLS }, () => 0));
const still = ref(false);
let mq: MediaQueryList | null = null;
const onMq = () => (still.value = !!mq?.matches);
onMounted(() => {
  mq = typeof matchMedia === "function" ? matchMedia("(prefers-reduced-motion: reduce)") : null;
  onMq();
  mq?.addEventListener?.("change", onMq);
});
onUnmounted(() => mq?.removeEventListener?.("change", onMq));

// one new slice per ~80 ms (the levels come at ~25 Hz; ~900 dots redrawn 12 times a second is plenty), the loudest
// level since the last slice so no transient is lost; a still band under reduced motion
let peak = 0, at = 0;
watch(
  () => props.level,
  (l) => {
    if (still.value) return;
    peak = Math.max(peak, l);
    const now = performance.now();
    if (now - at < 80) return;
    at = now;
    history.value = [...history.value.slice(1), Math.max(0, Math.min(1, peak))];
    peak = 0;
  },
);

// a fixed gentle shape (the sketch's) for the still band and as a floor under silence
const shape = (x: number) => 0.55 + 0.22 * Math.sin(x / 70) + 0.16 * Math.sin(x / 23 + 1) + 0.12 * Math.sin(x / 9.5 + 2);

interface TideDot { x: number; y: number; r: number }
const dots = computed<TideDot[]>(() => {
  const out: TideDot[] = [];
  for (let c = 0; c < COLS; c++) {
    const x = c * PITCH + 6;
    const w = still.value ? shape(x) * 0.8 : 0.12 + 0.95 * history.value[c] * (0.75 + 0.25 * shape(x));
    for (let r = 0; r < ROWS; r++) {
      const v = Math.max(0, Math.min(1, w + 0.1 - r * 0.15));
      if (v < 0.1) continue;
      out.push({ x, y: r * PITCH + 6, r: +(v * 3.8).toFixed(2) });
    }
  }
  return out;
});

let torn = `M0 ${H} L0 102`;
for (let x = 0; x <= W; x += 8) torn += ` L${x} ${(102 + 3 * Math.sin(x / 90) + 1.5 * Math.sin(x / 31 + 1)).toFixed(1)}`;
torn += ` L${W} ${H} Z`;

const lineX = computed(() => W * Math.max(0, Math.min(1, props.level)));
</script>

<template>
  <svg class="tide" :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="xMidYMid slice" aria-hidden="true" focusable="false">
    <g class="layer warm">
      <circle v-for="(d, i) in dots" :key="'w' + i" :cx="d.x + 1.6" :cy="d.y + 1.2" :r="d.r * 0.7" />
    </g>
    <g class="layer main">
      <circle v-for="(d, i) in dots" :key="'m' + i" :cx="d.x" :cy="d.y" :r="d.r" />
    </g>
    <path class="torn" :d="torn" />
    <line class="plain" x1="0" :x2="lineX" y1="62" y2="62" />
  </svg>
</template>

<style scoped>
.tide { display: block; width: 100%; height: 7.75rem; }
.main { fill: var(--tide-dots); }
.warm { fill: var(--tide-offset); opacity: 0.3; }
.torn { fill: var(--bg); }
.plain { display: none; stroke: var(--text); stroke-width: 6; }
:root[data-theme="contrast"] .tide .layer, :root[data-theme="contrast"] .tide .torn { display: none; }
:root[data-theme="contrast"] .tide .plain { display: inline; }
</style>
