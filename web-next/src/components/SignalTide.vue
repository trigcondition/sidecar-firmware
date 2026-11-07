<!-- web-next/src/components/SignalTide.vue - Sidecar's signature header band: halftone dots, one column per slice of
     the master level (the newest on the right), an orange layer offset ~2 px at 30 %, a torn bottom edge.
     Decorative (aria-hidden: the level is a number on Home and Mix). prefers-reduced-motion: a still band, not moving.
     Behind the dots a soft blurred UV glow, over them a few dashed "docked circles" (the logo's idea, the UV night
     design): both still drawings, so reduced motion changes nothing there. High-contrast theme: a plain level line
     instead of dots, no glow, no circles. -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, useId, watch } from "vue";

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

// Two speeds. Up and down: the whole band follows the level live (an 80 ms clock, quick to rise, ~0.4 s to fall).
// Sideways: the history moves one column only every 2.5 s (the loudest level in that time), so the band drifts very
// slowly instead of scrolling. On a clock, not on each new level: between hits the level sits at 0 and does not
// change. A still band under reduced motion, nothing while the tab is hidden.
const SLICE_MS = 2500;
let peak = 0, slicePeak = 0, sliceAt = 0;
const live = ref(0);
watch(() => props.level, (l) => { peak = Math.max(peak, l); });
let clock: ReturnType<typeof setInterval> | undefined;
onMounted(() => {
  clock = setInterval(() => {
    if (still.value || document.hidden) return;
    const v = toLift(peak);
    peak = 0;
    live.value = v > live.value ? v : live.value * 0.8 + v * 0.2;
    slicePeak = Math.max(slicePeak, v);
    const now = performance.now();
    if (now - sliceAt >= SLICE_MS) {
      sliceAt = now;
      history.value = [...history.value.slice(1), slicePeak];
      slicePeak = 0;
    }
  }, 80);
});
onUnmounted(() => clearInterval(clock));

// a peak (linear, 0..1) as how far it lifts the band: on a dB scale, -48 dB and below nothing, 0 dB all, so a mix
// around -20 dB moves it visibly (linear, it moved a few per cent)
function toLift(p: number): number {
  if (!(p > 0)) return 0;
  return Math.max(0, Math.min(1, (20 * Math.log10(p) + 48) / 48));
}

// a fixed gentle shape (the sketch's) for the still band and as a floor under silence
const shape = (x: number) => 0.55 + 0.22 * Math.sin(x / 70) + 0.16 * Math.sin(x / 23 + 1) + 0.12 * Math.sin(x / 9.5 + 2);

interface TideDot { x: number; y: number; r: number }
const dots = computed<TideDot[]>(() => {
  const out: TideDot[] = [];
  for (let c = 0; c < COLS; c++) {
    const x = c * PITCH + 6;
    // silence (or no Sidecar yet) shows the sketch's band; the level lifts it from there
    const w = still.value ? shape(x) : shape(x) * (0.85 + 0.6 * (0.45 * history.value[c] + 0.55 * live.value));
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

// the glow's ellipses and the docked circles (the sketch's places): a ring, a dashed ring round it, a dashed line out
// to a small docked circle
const blur = `uvblur-${useId()}`;
const GLOW = [[260, 70, 220, 46, 1], [760, 40, 200, 40, 2], [1120, 80, 240, 44, 3]];
const cells = [[430, 34, 15], [610, 78, 11], [905, 30, 18], [1210, 64, 13], [150, 88, 9]].map(([x, y, r], i) => {
  const a = 0.6 + i * 1.3;
  return { x, y, r, r2: r + 7, lx: Math.round(x + Math.cos(a) * (r + 26)), ly: Math.round(y + Math.sin(a) * (r + 14)), rs: Math.round(r * 0.45) };
});

const lineX = computed(() => W * Math.max(0, Math.min(1, props.level)));
</script>

<template>
  <svg class="tide" :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="xMidYMid slice" aria-hidden="true" focusable="false">
    <defs>
      <filter :id="blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="28" /></filter>
    </defs>
    <g class="glow" :filter="`url(#${blur})`">
      <ellipse v-for="g in GLOW" :key="g[0]" :cx="g[0]" :cy="g[1]" :rx="g[2]" :ry="g[3]" :class="`g${g[4]}`" />
    </g>
    <g class="layer warm">
      <circle v-for="(d, i) in dots" :key="'w' + i" :cx="d.x + 1.6" :cy="d.y + 1.2" :r="d.r * 0.7" />
    </g>
    <g class="layer main">
      <circle v-for="(d, i) in dots" :key="'m' + i" :cx="d.x" :cy="d.y" :r="d.r" />
    </g>
    <g class="cells">
      <g v-for="c in cells" :key="c.x">
        <circle :cx="c.x" :cy="c.y" :r="c.r" />
        <circle :cx="c.x" :cy="c.y" :r="c.r2" stroke-dasharray="3 4" />
        <line :x1="c.x" :y1="c.y" :x2="c.lx" :y2="c.ly" stroke-dasharray="2 3" />
        <circle :cx="c.lx" :cy="c.ly" :r="c.rs" />
      </g>
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
.glow { opacity: var(--glow-opacity); }
.g1 { fill: var(--glow-1); }
.g2 { fill: var(--glow-2); }
.g3 { fill: var(--glow-3); }
.cells { fill: none; stroke: var(--cells); stroke-width: 1; opacity: var(--cells-opacity); }
.plain { display: none; stroke: var(--text); stroke-width: 6; }
:root[data-theme="contrast"] .tide .layer, :root[data-theme="contrast"] .tide .torn,
:root[data-theme="contrast"] .tide .glow, :root[data-theme="contrast"] .tide .cells { display: none; }
:root[data-theme="contrast"] .tide .plain { display: inline; }
</style>
