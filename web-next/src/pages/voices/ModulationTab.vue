<!-- web-next/src/pages/voices/ModulationTab.vue - Voices > Modulation: the eight LFOs. -->
<script setup lang="ts">
import { computed } from "vue";
import Card from "../../ui/Card.vue";
import LfoCard from "./LfoCard.vue";
import type { Lfos } from "../../lib/lfo";
import { useSidecar } from "../../lib/useSidecar";
import { useSend } from "../../lib/useSend";

const { status, paired } = useSidecar();
const send = useSend();
const lfos = computed(() => (status.value as Record<string, any> | null)?.lfo as Lfos | undefined);
</script>

<template>
  <Card label="Modulation">
    <p v-if="!lfos">This build reports no LFOs.</p>
    <template v-else>
      <p class="small">Eight LFOs, saved with kit {{ lfos.kit }}. The shape shows the phase now.</p>
      <div class="lfos">
        <LfoCard v-for="(l, i) in lfos.l" :key="i" :n="i + 1" :lfo="l" :disabled="!paired" @cmd="send" />
      </div>
    </template>
  </Card>
</template>

<style scoped>
.lfos { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 18rem), 1fr)); gap: 0.75rem; }
</style>
