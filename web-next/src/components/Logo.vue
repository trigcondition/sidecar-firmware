<!-- web-next/src/components/Logo.vue - the "Docked" mark (geometry and colours in logo.ts). Decorative (the wordmark
     beside it carries the name). The dots' still colours grade from coral at the centre to lavender at the rim
     (--logo-1..4). animated: each dot breathes 1 -> 0.62 -> 1 over 4.8 s, delayed by its distance to the centre (a slow
     ripple outward), and drifts through all five UV colours in its own order, each fade 6-14 s, from its own point in
     the cycle (seeded per dot), so the disc shimmers rather than pulsing in step. CSS only: no per-frame script. Under
     prefers-reduced-motion both stop (tokens.css) and the graded colours stay; in high contrast every --logo colour is
     white. The disc stays still: the accent, or coral with disc="warm". -->
<script setup lang="ts">
import { computed } from "vue";
import { LOGO_COLOURS, LOGO_DISC, LOGO_REACH, LOGO_VIEWBOX, dotDrift, gradeColour, logoDots } from "./logo";

const props = withDefaults(defineProps<{ size?: number; animated?: boolean; disc?: "accent" | "warm" }>(), { size: 40, animated: false, disc: "accent" });
const dots = computed(() =>
  logoDots(props.size < 40).map((p, i) => {
    const d = dotDrift(i);
    return {
      ...p,
      cls: `g${gradeColour(p.d)} d${d.order}`,
      style: props.animated
        ? { "--rd": `${((p.d / LOGO_REACH) * 1.6).toFixed(3)}s`, "--dd": `${(d.fade * LOGO_COLOURS).toFixed(2)}s`, "--dp": `${-d.phase}s` }
        : undefined,
    };
  }),
);
</script>

<template>
  <svg
    class="logo"
    :class="{ breathe: animated }"
    :viewBox="LOGO_VIEWBOX"
    :width="(size * 46) / 40"
    :height="size"
    aria-hidden="true"
    focusable="false"
  >
    <circle v-for="(p, i) in dots" :key="i" :cx="p.x" :cy="p.y" :r="p.r" class="dot" :class="p.cls" :style="p.style" />
    <circle :cx="LOGO_DISC.cx" :cy="LOGO_DISC.cy" :r="LOGO_DISC.r" class="disc" :class="disc" />
  </svg>
</template>

<style scoped>
.g1 { fill: var(--logo-1); }
.g2 { fill: var(--logo-2); }
.g3 { fill: var(--logo-3); }
.g4 { fill: var(--logo-4); }
.disc { fill: var(--accent); }
.disc.warm { fill: var(--warm); }

.breathe .dot { animation-duration: 4.8s, var(--dd); animation-timing-function: ease-in-out; animation-iteration-count: infinite; animation-delay: var(--rd), var(--dp); }
.breathe .d0 { animation-name: breathe, drift0; }
.breathe .d1 { animation-name: breathe, drift1; }
.breathe .d2 { animation-name: breathe, drift2; }
.breathe .d3 { animation-name: breathe, drift3; }
@keyframes breathe {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.62; }
}
/* the orders of logo.ts DRIFT_ORDERS: each stop one colour, each step one fade */
@keyframes drift0 {
  0%, 100% { fill: var(--logo-1); } 20% { fill: var(--logo-2); } 40% { fill: var(--logo-3); } 60% { fill: var(--logo-4); } 80% { fill: var(--logo-5); }
}
@keyframes drift1 {
  0%, 100% { fill: var(--logo-3); } 20% { fill: var(--logo-5); } 40% { fill: var(--logo-1); } 60% { fill: var(--logo-4); } 80% { fill: var(--logo-2); }
}
@keyframes drift2 {
  0%, 100% { fill: var(--logo-2); } 20% { fill: var(--logo-4); } 40% { fill: var(--logo-1); } 60% { fill: var(--logo-5); } 80% { fill: var(--logo-3); }
}
@keyframes drift3 {
  0%, 100% { fill: var(--logo-4); } 20% { fill: var(--logo-1); } 40% { fill: var(--logo-3); } 60% { fill: var(--logo-2); } 80% { fill: var(--logo-5); }
}
</style>
