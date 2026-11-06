<!-- web-next/src/ui/Tile.vue - one status tile: a label, the value as a big number, its details. A warning is said in
     words and with a mark, never by colour alone. -->
<script setup lang="ts">
import type { Tile } from "../lib/tiles";
defineProps<{ tile: Tile }>();
</script>

<template>
  <div class="tile" :class="`tone-${tile.tone || 'plain'}`">
    <div class="label">{{ tile.name }}</div>
    <div class="value num">
      <span v-if="tile.tone === 'bad'" class="mark" aria-hidden="true">⚠ </span>{{ tile.value }}
      <span v-if="tile.tone === 'bad'" class="visually-hidden">(a warning)</span>
    </div>
    <ul class="detail">
      <li v-for="(d, i) in tile.detail" :key="i">{{ d }}</li>
    </ul>
  </div>
</template>

<style scoped>
.tile { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.75rem; min-width: 0; display: flex; flex-direction: column; gap: 0.25rem; }
.value { font-size: 1.5rem; line-height: 1.1; overflow-wrap: anywhere; }
.tone-good .value { color: var(--accent); }
.tone-bad { border-color: var(--warm); border-width: 2px; }
.tone-bad .value, .mark { color: var(--warm); }
.detail { list-style: none; margin: 0; padding: 0; font-size: var(--text-min); color: var(--label); display: flex; flex-wrap: wrap; gap: 0 0.75rem; }
</style>
