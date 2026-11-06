<!-- web-next/src/ui/Fader.vue - a value on a native range input (keyboard, screen readers and touch for free), its
     number always shown. vertical: a mixer fader. The value goes out on change (release), live on input. -->
<script setup lang="ts">
import { computed, useId } from "vue";

const props = withDefaults(
  defineProps<{ label: string; modelValue: number; min?: number; max?: number; step?: number; unit?: string; vertical?: boolean; hideLabel?: boolean; format?: (v: number) => string }>(),
  { min: 0, max: 127, step: 1, unit: "", vertical: false, hideLabel: false, format: undefined },
);
const emit = defineEmits<{ "update:modelValue": [number]; commit: [number] }>();
const id = useId();
const text = computed(() => (props.format ? props.format(props.modelValue) : `${props.modelValue}${props.unit ? " " + props.unit : ""}`));
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
      :value="modelValue"
      :aria-valuetext="`${label} ${text}`"
      @input="emit('update:modelValue', Number(($event.target as HTMLInputElement).value))"
      @change="emit('commit', Number(($event.target as HTMLInputElement).value))"
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
