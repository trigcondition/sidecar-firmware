<!-- web-next/src/ui/Meter.vue - a dotted level bar (decorative: the number next to it is the value). -->
<script setup lang="ts">
withDefaults(defineProps<{ level: number; height?: string; tone?: "accent" | "warm" }>(), { height: "10rem", tone: "accent" });
</script>

<template>
  <div class="meter" :style="{ height }" aria-hidden="true">
    <div class="fill" :class="`tone-${tone}`" :style="{ height: `${Math.round(Math.max(0, Math.min(1, level)) * 100)}%` }" />
  </div>
</template>

<style scoped>
.meter { position: relative; width: 0.5rem; display: flex; align-items: flex-end; }
.fill { width: 100%; background: repeating-linear-gradient(0deg, var(--accent) 0 3px, transparent 3px 5px); transition: height 60ms linear; }
.tone-warm { background: repeating-linear-gradient(0deg, var(--warm) 0 3px, transparent 3px 5px); }
:root[data-theme="contrast"] .fill { background: var(--text); }
</style>
