<!-- web-next/src/App.vue - the frame, as the approved sketch: the signal tide band with the SIDECAR wordmark pill and a
     status pill (pattern, tempo, play state, connection) laid over it, the page tabs as pills (the current one
     filled), the page. A skip link first. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Logo from "./components/Logo.vue";
import SignalTide from "./components/SignalTide.vue";
import ConfirmDialog from "./ui/ConfirmDialog.vue";
import { routes } from "./router";
import { provideSidecar } from "./lib/useSidecar";
import { usePrefs } from "./lib/usePrefs";
import { transportText } from "./lib/status";
import { WEB_BUILD, buildNote } from "./lib/build";

usePrefs();
const { state, paired, levels, status, seqpos, hello } = provideSidecar();
const tabs = routes.filter((r) => r.name);
const masterLevel = computed(() => (levels.value ? Math.max(...levels.value.master) : 0));
const conn = computed(() =>
  state.value === "open" ? (paired.value ? "Live" : "Live, not paired") : state.value === "connecting" ? "Connecting…" : state.value === "incompatible" ? "Incompatible" : "Offline",
);
const transport = computed(() => transportText(seqpos.value));
// the page against the Sidecar's release (lib/build.ts): a note until it is put away (this tab only)
const buildMsg = computed(() => buildNote(WEB_BUILD, hello.value?.version));
const buildSeen = ref("");
// the supply (/status "power".warn, pi/engine/power_state.h): on every page, Wi-Fi and the page suffer first
const powerWarn = computed(() => {
  const w = (status.value as { power?: { warn?: string } } | null)?.power?.warn;
  return w ? w.charAt(0).toUpperCase() + w.slice(1) : "";
});
</script>

<template>
  <a class="skip pill" href="#main">Skip to the page</a>
  <header class="top">
    <SignalTide :level="masterLevel" />
    <div class="over">
      <span class="brand">
        <Logo :size="40" animated />
        <span class="wordmark">SIDECAR</span>
      </span>
      <p class="state" role="status" aria-live="polite">
        <span v-if="transport">{{ transport }}</span>
        <span class="lamp" :class="{ on: state === 'open' }" aria-hidden="true" />
        <span>{{ conn }}</span>
      </p>
    </div>
  </header>
  <nav aria-label="Pages" class="tabs">
    <router-link v-for="r in tabs" :key="String(r.name)" :to="r.path" class="tab">{{ r.meta?.title }}</router-link>
  </nav>
  <p v-if="powerWarn" class="banner" role="alert"><span aria-hidden="true">⚠ </span>{{ powerWarn }} (Wi-Fi and the web page suffer first). <router-link :to="{ path: '/', query: { show: 'power' } }">Details</router-link></p>
  <div v-if="buildMsg && buildSeen !== buildMsg" class="banner build" role="status">
    <span>{{ buildMsg }}</span>
    <button type="button" @click="buildSeen = buildMsg">Got it</button>
  </div>
  <main id="main">
    <router-view />
  </main>
  <ConfirmDialog />
</template>

<style scoped>
.top { position: relative; background: var(--tide-bg); overflow: hidden; }
.over {
  position: absolute; inset: 0 0 auto 0; padding: 1.125rem 1rem;
  display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 0.75rem;
}
.brand {
  display: inline-flex; align-items: center; gap: 0.75rem; padding: 0.375rem 1rem 0.375rem 0.625rem;
  background: color-mix(in srgb, var(--tide-bg) 82%, transparent); border-radius: var(--radius-pill);
}
.wordmark { font-weight: 700; letter-spacing: 0.22em; font-size: clamp(1.25rem, 4vw, 1.875rem); }
.state {
  margin: 0; display: inline-flex; align-items: center; gap: 0.625rem; padding: 0.375rem 0.75rem;
  border-radius: var(--radius-pill); background: color-mix(in srgb, var(--bg) 82%, transparent); font-size: 0.9375rem;
}
.lamp { width: 0.625rem; height: 0.625rem; border-radius: 50%; border: 2px solid var(--control-border); }
.lamp.on { background: var(--accent); border-color: var(--accent); box-shadow: var(--accent-glow); }
.tabs { display: flex; flex-wrap: wrap; gap: 0.375rem 0.625rem; padding: 0 1rem 0.375rem; margin-top: -0.375rem; position: relative; }
.tab {
  display: inline-flex; align-items: center; min-height: var(--target); padding: 0.625rem 1.125rem;
  border-radius: var(--radius-pill); color: var(--text); text-decoration: none; border: var(--control-border-width) solid transparent;
}
.tab:hover { border-color: var(--control-border); }
/* the current page: filled (a shape change, not only a colour), and aria-current for screen readers */
.tab[aria-current="page"] { background: var(--text); color: var(--bg); font-weight: 700; }
/* phones: the tabs in one row that scrolls sideways (inside itself: the page never scrolls sideways) */
@media (max-width: 40rem) {
  .tabs { flex-wrap: nowrap; overflow-x: auto; scrollbar-width: thin; padding-bottom: 0.5rem; }
  .tab { flex: none; padding: 0.625rem 0.875rem; }
  .over { padding: 0.75rem; }
}
.banner { margin: 0.5rem 1rem 0; padding: 0.75rem 1rem; border: 2px solid var(--warm); border-radius: var(--radius-control); color: var(--warm); }
.banner a { color: var(--warm); font-weight: 700; }
.banner.build { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 0.5rem 1rem; color: var(--text); }
main { max-width: 80rem; margin: 0 auto; padding: 1rem 1rem 2rem; }
</style>
