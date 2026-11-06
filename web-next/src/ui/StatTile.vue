<!-- web-next/src/ui/StatTile.vue - one Home status tile: a label, at most one big figure (the "big" slot, or value +
     unit: Doto only here), the tile's own drawing (the default slot), one quiet detail line, and the rest behind
     "More". warn: a mark and the word, never colour alone. The tile is focusable by id (the banner's "Details"). -->
<script setup lang="ts">
withDefaults(defineProps<{ id: string; label: string; value?: string | null; unit?: string; detail?: string; more?: string[]; warn?: boolean }>(), {
  value: null, unit: "", detail: "", more: () => [], warn: false,
});
</script>

<template>
  <section :id="`tile-${id}`" class="stat" :class="{ warn }" :aria-labelledby="`tile-${id}-l`" tabindex="-1">
    <h3 :id="`tile-${id}-l`" class="label">{{ label }}</h3>
    <div class="head">
      <slot name="big">
        <span v-if="value !== null" class="big"><span class="num">{{ value }}</span><span v-if="unit" class="unit">{{ unit }}</span></span>
      </slot>
      <span v-if="warn" class="flag"><span aria-hidden="true">⚠</span> check</span>
    </div>
    <slot />
    <p v-if="detail" class="detail">{{ detail }}</p>
    <details v-if="more.length" class="more">
      <summary>More</summary>
      <ul>
        <li v-for="(m, i) in more" :key="i">{{ m }}</li>
      </ul>
    </details>
  </section>
</template>

<style scoped>
.stat {
  border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.75rem 0.875rem;
  display: flex; flex-direction: column; gap: 0.5rem; min-width: 0; background: var(--bg);
}
.stat.warn { border-color: var(--warm); }
.stat:focus { outline: 3px solid var(--focus); outline-offset: 2px; }
h3 { margin: 0; }
.head { display: flex; align-items: baseline; justify-content: space-between; gap: 0.5rem; min-height: 2.25rem; }
.big { display: inline-flex; align-items: baseline; gap: 0.25rem; }
.big .num { font-size: 2rem; line-height: 1; }
.unit { color: var(--label); font-variant-numeric: tabular-nums; }
.flag { color: var(--warm); font-size: var(--text-min); font-weight: 700; white-space: nowrap; }
.warn :deep(.big .num) { color: var(--warm); }
.detail { margin: auto 0 0; color: var(--label); font-size: var(--text-min); font-variant-numeric: tabular-nums; }
.more { font-size: var(--text-min); color: var(--label); }
.more summary {
  display: inline-flex; align-items: center; min-height: var(--target); cursor: pointer; color: var(--accent);
  list-style-position: inside;
}
.more ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.125rem; font-variant-numeric: tabular-nums; }
</style>
