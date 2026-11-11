<!-- web-next/src/ui/SpectrumDots.vue - a strip's live spectrum (the "spectrum" topic, src/lib/useSpectrum.ts) as columns
     of dots (DotFill's look in the second accent, coral, so it stands apart from the curve and its fill; brightest at the top), one column of whole dots per band. Goes inside
     an <svg>, under the curve it belongs to; decoration only (aria-hidden). It subscribes while it is mounted and redraws
     on its own, so the page around it does not redraw at the spectrum's rate (a select there would lose its pick). The
     axis: 20 Hz .. 20 kHz across `width`, log (as the EQ curve's). -->
<script setup lang="ts">
import { computed, useId, watch } from "vue";
import { columnsPath } from "../lib/spectrum";
import { useSpectrum } from "../lib/useSpectrum";

const props = withDefaults(defineProps<{ strip: string; width: number; bottom: number; step?: number; r?: number }>(), {
  step: 5,
  r: 1.5,
});
const emit = defineEmits<{ live: [boolean] }>(); // bands showing (or not): the page can quiet its own fill
const id = `sp${useId().replace(/[^\w-]/g, "")}`;
const { levels, hz } = useSpectrum(() => props.strip);
const col = computed(() => (levels.value.length ? props.width / levels.value.length : props.step));
const d = computed(() => columnsPath(levels.value, hz.value, props.width, props.bottom, props.step));
watch(() => levels.value.length > 0, (v) => emit("live", v), { immediate: true });
</script>

<template>
  <g aria-hidden="true">
    <defs>
      <pattern :id="`${id}p`" patternUnits="userSpaceOnUse" :width="col" :height="step">
        <circle :cx="col / 2" :cy="step / 2" :r="r" class="dot" />
      </pattern>
      <linearGradient :id="`${id}g`" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="white" stop-opacity="1" />
        <stop offset="1" stop-color="white" stop-opacity="0.25" />
      </linearGradient>
      <mask :id="`${id}m`" maskUnits="userSpaceOnUse" x="0" y="0" :width="width" :height="bottom">
        <rect :width="width" :height="bottom" :fill="`url(#${id}g)`" />
      </mask>
    </defs>
    <path v-if="d" :d="d" :fill="`url(#${id}p)`" :mask="`url(#${id}m)`" />
  </g>
</template>

<style scoped>
.dot { fill: var(--warm); }
</style>
