<!-- web-next/src/ui/Spark.vue - a small line of the last minute's samples (0..max). Drawn, not animated: it moves only
     when a new sample comes (once a second), so reduced motion has nothing to stop. The picture is hidden from screen
     readers; label says the same in words (lowest, highest, now). -->
<script setup lang="ts">
import { computed } from "vue";
import { sparkPoints } from "../lib/health";

const p = withDefaults(defineProps<{ values: number[]; max?: number; label: string; unit?: string }>(), { max: 100, unit: " %" });
const W = 120, H = 28;
const pts = computed(() => sparkPoints(p.values, W, H, p.max));
const words = computed(() => {
  const v = p.values;
  if (!v.length) return `${p.label}: no samples yet`;
  const r = (x: number) => Math.round(x);
  return `${p.label}, last ${v.length} s: lowest ${r(Math.min(...v))}${p.unit}, highest ${r(Math.max(...v))}${p.unit}, now ${r(v[v.length - 1])}${p.unit}`;
});
</script>

<template>
  <span class="spark">
    <svg :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="none" aria-hidden="true" focusable="false">
      <line :x1="0" :x2="W" :y1="H - 0.5" :y2="H - 0.5" class="base" />
      <polygon v-if="values.length > 1" :points="`${pts} ${W},${H} 0,${H}`" class="area" />
      <polyline :points="pts" class="line" />
    </svg>
    <span class="visually-hidden">{{ words }}</span>
  </span>
</template>

<style scoped>
.spark { position: relative; display: block; }
svg { display: block; width: 100%; height: 2.25rem; overflow: visible; }
.area { fill: var(--accent-tint); stroke: none; }
.base { stroke: var(--border); stroke-width: 1; vector-effect: non-scaling-stroke; }
.line { fill: none; stroke: var(--accent); stroke-width: 1.5; stroke-linejoin: round; vector-effect: non-scaling-stroke; }
</style>
