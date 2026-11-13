<!-- web-next/src/ui/DotFill.vue - the area under a curve as a grid of dots in the accent colour, brightest at the top
     (the Sidecar's dot-matrix look). Goes inside an <svg>: `d` is the curve; the area runs down to `bottom`. `step`
     and `r` are in the svg's own units, so each use picks them for its size on screen. -->
<script setup lang="ts">
import { computed, useId } from "vue";

const props = withDefaults(defineProps<{ d: string; width: number; bottom: number; step?: number; r?: number; muted?: boolean }>(), {
  step: 4,
  r: 1,
  muted: false,
});
const id = `df${useId().replace(/[^\w-]/g, "")}`;
const area = computed(() => `${props.d} L${props.width} ${props.bottom} L0 ${props.bottom} Z`);
</script>

<template>
  <defs>
    <pattern :id="`${id}p`" patternUnits="userSpaceOnUse" :width="step" :height="step">
      <circle :cx="step / 2" :cy="step / 2" :r="r" class="dot" :class="{ muted }" />
    </pattern>
    <linearGradient :id="`${id}g`" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="white" stop-opacity="1" />
      <stop offset="1" stop-color="white" stop-opacity="0.3" />
    </linearGradient>
    <mask :id="`${id}m`" maskContentUnits="objectBoundingBox">
      <rect width="1" height="1" :fill="`url(#${id}g)`" />
    </mask>
  </defs>
  <path :d="area" :fill="`url(#${id}p)`" :mask="`url(#${id}m)`" />
</template>

<style scoped>
.dot { fill: var(--accent); opacity: 0.85; }
.dot.muted { fill: var(--control-border); opacity: 0.5; }
</style>
