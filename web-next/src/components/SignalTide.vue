<!-- web-next/src/components/SignalTide.vue - the header's slot for Sidecar's signature "signal tide": a band of
     halftone dots whose column sizes follow the master level (the "levels" topic, docs/web-ws.md), an orange layer
     offset ~2 px at 50 %, a torn bottom edge. This pass only holds the place and wires the data: plain dots, no
     polish. Decorative (aria-hidden; the level is shown as a number elsewhere); static under reduced motion (it
     only redraws on data, no animation of its own); a plain level line in the high-contrast theme. -->
<script setup lang="ts">
import { computed } from "vue";

const props = withDefaults(defineProps<{ level?: number; columns?: number }>(), { level: 0, columns: 48 });
const W = 480, H = 40, ROWS = 4;
const cols = computed(() => {
  const l = Math.max(0, Math.min(1, props.level));
  const out: { x: number; r: number }[] = [];
  for (let i = 0; i < props.columns; i++) {
    const wave = 0.6 + 0.4 * Math.sin(i * 0.45);           // a placeholder shape until the real design lands
    out.push({ x: (i + 0.5) * (W / props.columns), r: 0.4 + 3.2 * l * wave });
  }
  return out;
});
</script>

<template>
  <svg class="tide" :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="none" aria-hidden="true" focusable="false">
    <g class="layer warm" transform="translate(2 2)">
      <template v-for="(c, i) in cols" :key="'w' + i">
        <circle v-for="row in ROWS" :key="row" :cx="c.x" :cy="row * (H / (ROWS + 1))" :r="c.r * (1 - row / (ROWS + 2))" />
      </template>
    </g>
    <g class="layer main">
      <template v-for="(c, i) in cols" :key="'m' + i">
        <circle v-for="row in ROWS" :key="row" :cx="c.x" :cy="row * (H / (ROWS + 1))" :r="c.r * (1 - row / (ROWS + 2))" />
      </template>
    </g>
    <line class="plain" x1="0" :x2="W * Math.max(0, Math.min(1, level))" :y1="H / 2" :y2="H / 2" />
  </svg>
</template>

<style scoped>
.tide { display: block; width: 100%; height: 2.5rem; }
.main { fill: var(--tide-dots); }
.warm { fill: var(--tide-offset); opacity: 0.5; }
.plain { display: none; stroke: var(--text); stroke-width: 4; }
:root[data-theme="contrast"] .tide .layer { display: none; }
:root[data-theme="contrast"] .tide .plain { display: inline; }
</style>
