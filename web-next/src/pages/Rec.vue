<!-- web-next/src/pages/Rec.vue - the recorder, its takes, the sampler and the auto-sampler (the old page's Record tab). -->
<script setup lang="ts">
import RecorderCard from "./rec/RecorderCard.vue";
import SamplersCard from "./rec/SamplersCard.vue";
import TakesCard from "./rec/TakesCard.vue";
</script>

<template>
  <h1 class="title" tabindex="-1">Rec</h1>
  <RecorderCard />
  <div class="grid">
    <TakesCard />
    <div><SamplersCard /></div>
  </div>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 26rem), 1fr)); gap: 0 1.25rem; align-items: start; }
</style>
