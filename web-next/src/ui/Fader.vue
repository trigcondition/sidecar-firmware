<!-- web-next/src/ui/Fader.vue - a value on a native range input (keyboard, screen readers and touch for free), its
     number always shown. vertical: a mixer fader. The value goes out on change (release), live on input. -->
<script setup lang="ts">
import { computed, useId } from "vue";
import { useHeld } from "../lib/held";

const props = withDefaults(
  defineProps<{ label: string; modelValue: number; min?: number; max?: number; step?: number; unit?: string; vertical?: boolean; hideLabel?: boolean; disabled?: boolean; format?: (v: number) => string }>(),
  { min: 0, max: 127, step: 1, unit: "", vertical: false, hideLabel: false, format: undefined },
);
const emit = defineEmits<{ "update:modelValue": [number]; commit: [number] }>();
const id = useId();
// the value moved to shows at once, until the Sidecar reports it (lib/held.ts)
const h = useHeld(() => props.modelValue);
const cur = h.shown;
const text = computed(() => (props.format ? props.format(cur.value) : `${cur.value}${props.unit ? " " + props.unit : ""}`));
function input(e: Event) {
  const v = Number((e.target as HTMLInputElement).value);
  h.hold(v);
  emit("update:modelValue", v);
}
function change(e: Event) {
  const v = Number((e.target as HTMLInputElement).value);
  h.hold(v);
  emit("commit", v);
}
</script>

<template>
  <div class="fader" :class="{ vertical }">
    <label :for="id" :class="{ 'visually-hidden': hideLabel }" class="label">{{ label }}</label>
    <input
      :id="id"
      type="range"
      :min="min"
      :max="max"
      :step="step"
      :value="cur"
      :disabled="disabled"
      :aria-valuetext="`${label} ${text}`"
      @input="input"
      @change="change"
    />
    <span class="num value" aria-hidden="true">{{ text }}</span>
  </div>
</template>

<style scoped>
.fader { display: flex; flex-direction: column; gap: 0.25rem; min-width: 0; }
.fader input { width: 100%; min-height: var(--target); border: 0; background: transparent; }
.value { font-size: 1.125rem; }
.vertical { align-items: center; }
.vertical input { writing-mode: vertical-lr; direction: rtl; width: var(--target); height: 10rem; min-height: 0; }
</style>
