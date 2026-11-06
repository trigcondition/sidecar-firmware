<!-- web-next/src/pages/Settings.vue - the display settings that exist now: theme and text size (per device). -->
<script setup lang="ts">
import { nextTick } from "vue";
import { TEXT_SIZES, THEMES, usePrefs, type TextSize, type Theme } from "../lib/usePrefs";

const { theme, textSize } = usePrefs();
const themeName: Record<Theme, string> = { standard: "Standard", contrast: "High contrast" };

// arrow keys move within a radio group (the ARIA pattern), Tab leaves it
function arrows<T>(e: KeyboardEvent, list: T[], cur: T, set: (v: T) => void) {
  const d = e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : e.key === "ArrowLeft" || e.key === "ArrowUp" ? -1 : 0;
  if (!d) return;
  e.preventDefault();
  const group = e.currentTarget as HTMLElement; // (gone from the event once it has been handled)
  const next = list[(list.indexOf(cur) + d + list.length) % list.length];
  set(next);
  nextTick(() => group.querySelector<HTMLElement>('[aria-checked="true"]')?.focus());
}
</script>

<template>
  <section class="card" aria-labelledby="h">
    <h1 id="h" class="title" tabindex="-1">Settings</h1>

    <h2 id="h-theme" class="label">Theme</h2>
    <div
      role="radiogroup"
      aria-labelledby="h-theme"
      class="row"
      @keydown="arrows(($event as KeyboardEvent), THEMES, theme, (v: Theme) => (theme = v))"
    >
      <button
        v-for="t in THEMES"
        :key="t"
        type="button"
        role="radio"
        class="pill"
        :aria-checked="theme === t"
        :tabindex="theme === t ? 0 : -1"
        @click="theme = t"
      >
        {{ themeName[t] }}
      </button>
    </div>

    <h2 id="h-size" class="label">Text size</h2>
    <div
      role="radiogroup"
      aria-labelledby="h-size"
      class="row"
      @keydown="arrows(($event as KeyboardEvent), TEXT_SIZES, textSize, (v: TextSize) => (textSize = v))"
    >
      <button
        v-for="s in TEXT_SIZES"
        :key="s"
        type="button"
        role="radio"
        class="pill"
        :aria-checked="textSize === s"
        :tabindex="textSize === s ? 0 : -1"
        @click="textSize = s"
      >
        {{ s }}&nbsp;%
      </button>
    </div>
    <p class="small">Kept on this device. Later also per user on the Sidecar.</p>
  </section>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 1rem; }
.pill { background: var(--card); }
</style>
