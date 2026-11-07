<!-- web-next/src/ui/Card.vue - a section card with its label as a pill on the top edge (the approved sketch's
     "section label"). tone: the label's accent (blue, or warm for sidechain / warnings). -->
<script setup lang="ts">
import { useId } from "vue";
withDefaults(defineProps<{ label: string; tone?: "accent" | "warm" | "plain" }>(), { tone: "accent" });
const id = useId();
</script>

<template>
  <section class="ui-card card" :class="`tone-${tone}`" :aria-labelledby="id">
    <h2 :id="id" class="ui-card-label">{{ label }}</h2>
    <slot />
  </section>
</template>

<style scoped>
.ui-card { position: relative; padding-top: 1.5rem; margin-top: 1rem; display: flex; flex-direction: column; gap: 0.875rem; }
.ui-card-label {
  position: absolute; top: -0.875rem; left: 1rem; margin: 0;
  padding: 0.3rem 0.875rem 0.3rem 0.625rem; border-radius: var(--radius-pill);
  font-size: var(--label-size); letter-spacing: var(--label-tracking); text-transform: uppercase; font-weight: 700;
  background: var(--accent); color: var(--on-accent);
}
.tone-warm { border-color: var(--warm-border); }
.tone-warm .ui-card-label { background: var(--warm); }
.tone-plain .ui-card-label { background: var(--text); }
</style>
