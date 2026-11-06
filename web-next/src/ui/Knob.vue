<!-- web-next/src/ui/Knob.vue - a dial drawn over a native range input (the input takes the keys, the pointer and the
     screen reader; the drawing is decoration). Dots round the dial as on the touchscreen, the value as a number. -->
<script setup lang="ts">
import { computed, ref, useId, watch } from "vue";

const props = withDefaults(defineProps<{ label: string; modelValue: number; min?: number; max?: number; disabled?: boolean; format?: (v: number) => string }>(), {
  min: 0,
  max: 127,
  disabled: false,
  format: undefined,
});
const emit = defineEmits<{ "update:modelValue": [number]; commit: [number] }>();
const id = useId();
// its own value while it is turned (the Sidecar's answer comes later); the prop's again when it is let go
const v = ref(props.modelValue);
let turning = false;
watch(() => props.modelValue, (m) => { if (!turning) v.value = m; });
const frac = computed(() => (v.value - props.min) / Math.max(1, props.max - props.min));
const angle = computed(() => -135 + 270 * frac.value);
const text = computed(() => (props.format ? props.format(v.value) : String(v.value)));
function input(e: Event) {
  turning = true;
  v.value = Number((e.target as HTMLInputElement).value);
  emit("update:modelValue", v.value);
}
function change() {
  turning = false;
  emit("commit", v.value);
}
const ticks = Array.from({ length: 28 }, (_, i) => {
  const a = ((-135 + (270 * i) / 27 - 90) * Math.PI) / 180;
  return { x: 24 + 20 * Math.cos(a), y: 24 + 20 * Math.sin(a), f: i / 27 };
});
</script>

<template>
  <div class="knob">
    <div class="dial">
      <svg viewBox="0 0 48 48" aria-hidden="true">
        <circle v-for="(t, i) in ticks" :key="i" :cx="t.x" :cy="t.y" r="1.3" :class="t.f <= frac + 0.001 ? 'on' : 'off'" />
        <line x1="24" y1="24" x2="24" y2="9" :transform="`rotate(${angle} 24 24)`" />
      </svg>
      <input
        :id="id"
        type="range"
        :min="min"
        :max="max"
        :value="v"
        :disabled="disabled"
        :aria-valuetext="`${label} ${text}`"
        @input="input"
        @change="change"
      />
    </div>
    <label :for="id" class="label">{{ label }}</label>
    <span class="num" aria-hidden="true">{{ text }}</span>
  </div>
</template>

<style scoped>
.knob { display: flex; flex-direction: column; align-items: center; gap: 0.125rem; min-width: 4rem; }
.dial { position: relative; width: 3.5rem; height: 3.5rem; }
.dial svg { width: 100%; height: 100%; }
.dial .on { fill: var(--accent); }
.dial .off { fill: var(--control-border); }
.dial line { stroke: var(--text); stroke-width: 2.5; stroke-linecap: round; }
/* the real control: covers the dial, invisible, but it takes focus (the ring is drawn on the dial) */
.dial input { position: absolute; inset: 0; width: 100%; height: 100%; opacity: 0; margin: 0; cursor: ns-resize; min-height: 0; min-width: 0; }
.dial:has(input:focus-visible) { outline: 3px solid var(--focus); outline-offset: 2px; border-radius: 50%; }
.num { font-size: 1.125rem; }
.label { font-size: var(--text-min); }
</style>
