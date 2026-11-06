<!-- web-next/src/ui/Segmented.vue - one choice of a few (a radio group: arrow keys inside, Tab out). The chosen one is
     filled and carries a check mark, so it is never told by colour alone. -->
<script setup lang="ts" generic="T extends string | number">
import { nextTick, ref } from "vue";

const props = defineProps<{ label: string; options: { value: T; text: string }[]; modelValue: T; disabled?: boolean }>();
const emit = defineEmits<{ "update:modelValue": [T] }>();
const group = ref<HTMLElement | null>(null);

function key(e: KeyboardEvent) {
  const d = e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : e.key === "ArrowLeft" || e.key === "ArrowUp" ? -1 : 0;
  if (!d || props.disabled) return;
  e.preventDefault();
  const i = props.options.findIndex((o) => o.value === props.modelValue);
  const next = props.options[(i + d + props.options.length) % props.options.length];
  emit("update:modelValue", next.value);
  nextTick(() => group.value?.querySelector<HTMLElement>('[aria-checked="true"]')?.focus());
}
</script>

<template>
  <div ref="group" role="radiogroup" :aria-label="label" class="seg" @keydown="key">
    <button
      v-for="o in options"
      :key="String(o.value)"
      type="button"
      role="radio"
      :aria-checked="o.value === modelValue"
      :tabindex="o.value === modelValue ? 0 : -1"
      :disabled="disabled"
      @click="emit('update:modelValue', o.value)"
    >
      <span v-if="o.value === modelValue" aria-hidden="true">✓ </span>{{ o.text }}
    </button>
  </div>
</template>

<style scoped>
.seg { display: inline-flex; flex-wrap: wrap; border: var(--control-border-width) solid var(--control-border); border-radius: var(--radius-control); overflow: hidden; }
.seg button { border: 0; border-radius: 0; background: transparent; }
.seg button[aria-checked="true"] { background: var(--text); color: var(--bg); font-weight: 700; }
</style>
