<!-- web-next/src/ui/Chip.vue - a toggle (aria-pressed): on = a check mark, a thicker outline and a tint. -->
<script setup lang="ts">
withDefaults(defineProps<{ pressed: boolean; tone?: "accent" | "warm" }>(), { tone: "accent" });
defineEmits<{ click: [MouseEvent] }>();
</script>

<template>
  <button type="button" class="chip" :class="`tone-${tone}`" :aria-pressed="pressed" @click="$emit('click', $event)">
    <span v-if="pressed" aria-hidden="true">✓ </span><slot />
  </button>
</template>

<style scoped>
.chip { border-radius: var(--radius-control); }
.chip[aria-pressed="true"] { border-width: 2px; font-weight: 700; }
.tone-accent[aria-pressed="true"] { border-color: var(--accent); background: var(--accent-tint); }
.tone-warm[aria-pressed="true"] { border-color: var(--warm); background: var(--warm-tint); }
</style>
