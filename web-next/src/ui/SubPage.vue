<!-- web-next/src/ui/SubPage.vue - a page made of sections (router.ts: a route with children): its title, the sections as
     tabs under it (each its own URL, /library/pool), the chosen one below. A section the Sidecar's build lacks
     (meta.needs, lib/box.ts) has no tab; opened by its URL it says so. -->
<script setup lang="ts">
import { computed } from "vue";
import { useRoute } from "vue-router";
import { has, type Feat } from "../lib/box";
import { useSidecar } from "../lib/useSidecar";

const route = useRoute();
const { status } = useSidecar();
const parent = computed(() => route.matched[0]);
const tabs = computed(() =>
  (parent.value?.children ?? [])
    .filter((c) => c.meta?.title && (!c.meta.needs || has(status.value, c.meta.needs as Feat)))
    .map((c) => ({ to: `${parent.value.path}/${c.path}`, title: String(c.meta!.title) })),
);
const missing = computed(() => {
  const n = route.meta.needs as Feat | undefined;
  return !!n && !!status.value && !has(status.value, n);
});
</script>

<template>
  <h1 class="visually-hidden" tabindex="-1">{{ parent?.meta.title }}</h1>
  <nav class="sub" :aria-label="`${parent?.meta.title} sections`">
    <router-link v-for="t in tabs" :key="t.to" :to="t.to" class="subtab">{{ t.title }}</router-link>
  </nav>
  <p v-if="missing" class="card">This Sidecar's build has no {{ route.meta.title }}.</p>
  <router-view v-else />
</template>

<style scoped>
/* no visible title: the main tab shows the page; the h1 stays for screen readers and the focus after a page change */
/* the sections: text tabs on a rule, the current one underlined and bold (a shape change, not only colour); they
   scroll sideways inside themselves on a phone */
.sub {
  display: flex; gap: 0.25rem 1.5rem; margin: 0.25rem 0 0.75rem; border-bottom: 1px solid var(--border);
  overflow-x: auto; scrollbar-width: thin;
}
.subtab {
  flex: none; display: inline-flex; align-items: center; min-height: var(--target); padding: 0 0.125rem;
  color: var(--label); text-decoration: none; border-bottom: 3px solid transparent; margin-bottom: -1px;
}
.subtab:hover { color: var(--text); }
.subtab[aria-current="page"] { color: var(--text); font-weight: 700; border-bottom-color: var(--accent); }
</style>
