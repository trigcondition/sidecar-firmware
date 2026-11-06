<!-- web-next/src/pages/Library.vue - the POOL, the card's files and the projects (the old page's Pool, Files and Project
     tabs). -->
<script setup lang="ts">
import FilesCard from "./library/FilesCard.vue";
import PoolCard from "./library/PoolCard.vue";
import ProjectsCard from "./library/ProjectsCard.vue";
import { has } from "../lib/box";
import { useSidecar } from "../lib/useSidecar";

const { status } = useSidecar();
</script>

<template>
  <h1 class="title" tabindex="-1">Library</h1>
  <PoolCard v-if="has(status, 'pools')" />
  <div class="grid">
    <FilesCard />
    <ProjectsCard v-if="has(status, 'project')" />
  </div>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 30rem), 1fr)); gap: 0 1.25rem; align-items: start; }
</style>
