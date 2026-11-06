<!-- Settings › Look: theme and text size, per device (lib/usePrefs.ts). -->
<script setup lang="ts">
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import { TEXT_SIZES, usePrefs, type TextSize, type Theme } from "../../lib/usePrefs";

const { theme, textSize } = usePrefs();
</script>

<template>
  <Card label="Look">
    <span class="label">Theme</span>
    <Segmented label="Theme" :options="[{ value: 'standard', text: 'Standard' }, { value: 'contrast', text: 'High contrast' }]" :model-value="theme" @update:model-value="(v: Theme) => (theme = v)" />
    <span class="label">Text size</span>
    <Segmented label="Text size" :options="TEXT_SIZES.map((s) => ({ value: s, text: `${s} %` }))" :model-value="textSize" @update:model-value="(v: TextSize) => (textSize = v)" />
    <p class="small">Kept on this device. Later also per user on the Sidecar.</p>
  </Card>
</template>
