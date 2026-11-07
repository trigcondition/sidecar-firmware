<!-- Seq › tabs: a few views of one panel (the ARIA tabs pattern: arrows, Home and End inside the list, Tab on into the
     panel). The current tab is bold and underlined (a shape, not only a colour). Slots: "before" and "after" sit on
     the tab row (a title, actions), the default slot is the current tab's panel. -->
<script setup lang="ts" generic="T extends string">
import { nextTick, ref, useId } from "vue";
import { tabMove } from "../../lib/sequi";

const props = defineProps<{ label: string; tabs: { id: T; text: string }[]; modelValue: T }>();
const emit = defineEmits<{ "update:modelValue": [T] }>();
const base = useId();
const list = ref<HTMLElement | null>(null);

function key(e: KeyboardEvent) {
  const i = props.tabs.findIndex((t) => t.id === props.modelValue);
  const j = tabMove(i, e.key, props.tabs.length);
  if (j === null) return;
  e.preventDefault();
  emit("update:modelValue", props.tabs[j].id);
  nextTick(() => list.value?.querySelector<HTMLElement>('[aria-selected="true"]')?.focus());
}
</script>

<template>
  <div class="tabrow">
    <slot name="before" />
    <div ref="list" role="tablist" :aria-label="label" class="tablist" @keydown="key">
      <button
        v-for="t in tabs"
        :id="`${base}-${t.id}`"
        :key="t.id"
        type="button"
        role="tab"
        class="tab"
        :aria-selected="t.id === modelValue"
        :aria-controls="`${base}-panel`"
        :tabindex="t.id === modelValue ? 0 : -1"
        @click="emit('update:modelValue', t.id)"
      >
        {{ t.text }}
      </button>
    </div>
    <slot name="after" />
  </div>
  <div :id="`${base}-panel`" role="tabpanel" class="tabpanel" :aria-labelledby="`${base}-${modelValue}`">
    <slot />
  </div>
</template>

<style scoped>
.tabrow { display: flex; flex-wrap: wrap; align-items: center; gap: 0.25rem 1rem; border-bottom: 1px solid var(--border); }
.tablist { display: flex; gap: 0 1.25rem; min-width: 0; }
.tab {
  min-width: 0; padding: 0 0.125rem; border: 0; border-radius: 0; background: transparent; color: var(--label);
  border-bottom: 3px solid transparent; margin-bottom: -1px;
}
.tab:hover { color: var(--text); }
.tab[aria-selected="true"] { color: var(--text); font-weight: 700; border-bottom-color: var(--accent); }
.tabpanel { padding-top: 0.875rem; }
</style>
