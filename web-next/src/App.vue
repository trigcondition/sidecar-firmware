<!-- web-next/src/App.vue - the frame: a skip link, the header (the Docked mark, the SIDECAR wordmark, the connection
     in words, the signal tide's slot), the page tabs, the page. The design ("OLED Studio") drops in through the tokens
     (src/styles/tokens.css); this pass is plumbing with the accessibility floor in place. -->
<script setup lang="ts">
import { computed } from "vue";
import Logo from "./components/Logo.vue";
import SignalTide from "./components/SignalTide.vue";
import { routes } from "./router";
import { provideSidecar } from "./lib/useSidecar";
import { usePrefs } from "./lib/usePrefs";

usePrefs();
const { state, paired, levels } = provideSidecar();
const tabs = routes.filter((r) => r.name);
const masterLevel = computed(() => (levels.value ? Math.max(...levels.value.master) : 0));
const connText = computed(() =>
  state.value === "open" ? (paired.value ? "Connected, paired" : "Connected, not paired") : state.value === "connecting" ? "Connecting…" : state.value === "incompatible" ? "Incompatible" : "Offline",
);
</script>

<template>
  <a class="skip pill" href="#main">Skip to the page</a>
  <header class="top">
    <div class="brand">
      <Logo :size="40" animated />
      <span class="wordmark">SIDECAR</span>
    </div>
    <p class="conn" role="status" aria-live="polite">
      <span class="dot" :class="state === 'open' ? 'on' : 'off'" aria-hidden="true">{{ state === "open" ? "●" : "○" }}</span>
      {{ connText }}
    </p>
    <div class="tide-slot"><SignalTide :level="masterLevel" /></div>
  </header>
  <nav aria-label="Pages" class="tabs">
    <router-link v-for="r in tabs" :key="String(r.name)" :to="r.path" class="pill">{{ r.meta?.title }}</router-link>
  </nav>
  <main id="main">
    <router-view />
  </main>
</template>

<style scoped>
.top {
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: 0.75rem 1rem;
  padding: 1rem;
}
.brand { display: flex; align-items: center; gap: 0.75rem; }
.wordmark {
  font-family: var(--font-ui);
  font-weight: 700;
  letter-spacing: 0.22em;
  padding: 0.375rem 0.875rem;
  border-radius: var(--radius-pill);
  background: var(--card);
  border: 1px solid var(--border);
}
.conn { justify-self: end; margin: 0; font-size: var(--text-min); color: var(--label); }
.dot.on { color: var(--accent); }
.tide-slot { grid-column: 1 / -1; }
.tabs { display: flex; flex-wrap: wrap; gap: 0.5rem; padding: 0 1rem 1rem; }
main { max-width: 60rem; margin: 0 auto; padding: 0 1rem 2rem; }
</style>
