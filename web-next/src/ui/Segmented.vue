<!-- web-next/src/ui/Segmented.vue - one choice of a few (a radio group: arrow keys inside, Tab out). The chosen one is
     filled and carries a check mark, so it is never told by colour alone. -->
<script setup lang="ts" generic="T extends string | number | boolean">
import { nextTick, ref } from "vue";
import { useHeld } from "../lib/held";

const props = defineProps<{ label: string; options: { value: T; text: string; disabled?: boolean }[]; modelValue: T; disabled?: boolean }>();
const emit = defineEmits<{ "update:modelValue": [T] }>();
const group = ref<HTMLElement | null>(null);
// the choice shows at once; the Sidecar's answer confirms it (lib/held.ts)
const h = useHeld(() => props.modelValue);
const cur = h.shown;
function choose(v: T) {
  h.hold(v);
  emit("update:modelValue", v);
}

function key(e: KeyboardEvent) {
  const d = e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : e.key === "ArrowLeft" || e.key === "ArrowUp" ? -1 : 0;
  if (!d || props.disabled) return;
  e.preventDefault();
  const live = props.options.filter((o) => !o.disabled);
  if (!live.length) return;
  const i = live.findIndex((o) => o.value === cur.value);
  const next = live[(i + d + live.length) % live.length];
  choose(next.value);
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
      :aria-checked="o.value === cur"
      :tabindex="o.value === cur ? 0 : -1"
      :disabled="disabled || o.disabled"
      @click="choose(o.value)"
    >
      <span v-if="o.value === cur" aria-hidden="true">✓ </span>{{ o.text }}
    </button>
  </div>
</template>

<style scoped>
.seg { display: inline-flex; align-self: flex-start; max-width: 100%; flex-wrap: wrap; border: var(--control-border-width) solid var(--control-border); border-radius: var(--radius-control); overflow: hidden; }
.seg button { border: 0; border-radius: 0; background: transparent; }
.seg button[aria-checked="true"] { background: var(--text); color: var(--bg); font-weight: 700; }
</style>
