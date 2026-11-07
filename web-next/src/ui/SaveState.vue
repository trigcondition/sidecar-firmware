<!-- web-next/src/ui/SaveState.vue - what happened to a setting (lib/pending.ts), said in words for everyone. -->
<script setup lang="ts">
import type { PendState } from "../lib/pending";
defineProps<{ state: PendState; msg?: string }>();
</script>

<template>
  <span class="save" role="status" aria-live="polite">
    <template v-if="state === 'pending'">sending…</template>
    <template v-else-if="state === 'saved'"><span aria-hidden="true">✓ </span>saved</template>
    <span v-else-if="state === 'failed'" class="warm"><span aria-hidden="true">⚠ </span>{{ msg }}</span>
  </span>
</template>

<style scoped>
.save { font-size: var(--text-min); color: var(--label); min-height: 1.25rem; }
</style>
