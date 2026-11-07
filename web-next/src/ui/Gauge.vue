<!-- web-next/src/ui/Gauge.vue - a thin horizontal meter (role="meter": a screen reader hears its name and value). value
     between min and max; mark: a tick on the track (a target, a budget) with its name; zone: a tinted stretch from zone
     to max (the hot end); range: a band from range[0] to range[1] instead of a fill from min (the FIFO's low..high).
     Warn turns the fill coral; the words beside it say why, never the colour alone. No animation. -->
<script setup lang="ts">
import { computed } from "vue";

const p = withDefaults(
  defineProps<{
    label: string;
    value: number | null;
    min?: number;
    max: number;
    text?: string; // the spoken value ("41 percent"); the visible one is the page's
    mark?: number | null;
    markLabel?: string;
    zone?: number | null;
    range?: [number, number] | null;
    warn?: boolean;
  }>(),
  { min: 0, text: "", mark: null, markLabel: "", zone: null, range: null, warn: false },
);
const at = (v: number) => `${(Math.max(0, Math.min(1, (v - p.min) / (p.max - p.min))) * 100).toFixed(2)}%`;
const fill = computed(() => {
  if (p.value === null) return null;
  if (p.range) return { left: at(p.range[0]), width: `calc(${at(p.range[1])} - ${at(p.range[0])})` };
  return { left: "0%", width: at(p.value) };
});
const spoken = computed(() => (p.value === null ? "not known yet" : p.text || String(p.value)) + (p.mark !== null && p.markLabel ? `, ${p.markLabel} ${p.mark}` : ""));
</script>

<template>
  <div
    class="gauge"
    role="meter"
    :aria-label="label"
    :aria-valuemin="min"
    :aria-valuemax="max"
    :aria-valuenow="value ?? undefined"
    :aria-valuetext="spoken"
  >
    <span v-if="zone !== null" class="zone" :style="{ left: at(zone), right: '0' }" />
    <span v-if="fill" class="fill" :class="{ warn }" :style="fill" />
    <span v-if="mark !== null" class="mark" :style="{ left: at(mark) }" />
  </div>
</template>

<style scoped>
.gauge { position: relative; height: 0.5rem; border-radius: var(--radius-pill); background: var(--border); }
.zone { position: absolute; top: 0; bottom: 0; background: var(--warm-tint); border-left: 1px dashed var(--warm); }
.fill { position: absolute; top: 0; bottom: 0; border-radius: var(--radius-pill); background: var(--accent); min-width: 2px; }
.fill.warn { background: var(--warm); }
.mark { position: absolute; top: -0.25rem; bottom: -0.25rem; width: 2px; margin-left: -1px; background: var(--text); border-radius: 1px; }
:root[data-theme="contrast"] .gauge { background: transparent; outline: 1px solid var(--border); }
:root[data-theme="contrast"] .zone { background: transparent; }
</style>
