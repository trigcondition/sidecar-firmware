<!-- web-next/src/pages/Voices.vue - the voices (machines, drone), the eight LFOs, the user machines, the presets. -->
<script setup lang="ts">
import { notify } from "../lib/notify";
import { computed } from "vue";
import Card from "../ui/Card.vue";
import LfoCard from "./voices/LfoCard.vue";
import MachinesCard from "./voices/MachinesCard.vue";
import PresetsCard from "./voices/PresetsCard.vue";
import VoiceGrid from "./voices/VoiceGrid.vue";
import VoiceView from "./voices/VoiceView.vue";
import { coalescer } from "../lib/coalesce";
import type { Lfos } from "../lib/lfo";
import { useSidecar } from "../lib/useSidecar";
import { has } from "../lib/box";

const { status, cmd, paired } = useSidecar();
const lfos = computed(() => (status.value as Record<string, any> | null)?.lfo as Lfos | undefined);
const latest = coalescer(cmd, (e) => notify.error(e));
function send(c: string) {
  void latest(c);
}
</script>

<template>
  <h1 class="title" tabindex="-1">Voices</h1>
  <VoiceView />
  <VoiceGrid />
  <Card label="Modulation">
    <p v-if="!lfos">This build reports no LFOs.</p>
    <template v-else>
      <p class="small">Eight LFOs, saved with kit {{ lfos.kit }}. The shape shows the phase now.</p>
      <div class="lfos">
        <LfoCard v-for="(l, i) in lfos.l" :key="i" :n="i + 1" :lfo="l" :disabled="!paired" @cmd="send" />
      </div>
    </template>
  </Card>
  <div class="grid">
    <MachinesCard v-if="has(status, 'userslots')" />
    <PresetsCard />
  </div>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.lfos { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 18rem), 1fr)); gap: 0.75rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 24rem), 1fr)); gap: 0 1.25rem; align-items: start; }
</style>
