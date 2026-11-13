<!-- web-next/src/pages/voices/ModulationTab.vue - Voices > Modulation: the LFOs. The Pi's 26 are two a track (LFO FINAL:
     T1..T12 and FX, A and B; pt 1): one track's pair at a time, following the Rytm's selected track when it changes; the
     Mini's 4 all at once. "full": the Rytm link is full, the LFOs update slower. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import LfoCard from "./LfoCard.vue";
import Segmented from "../../ui/Segmented.vue";
import { LFO_FULL, LFO_TRACKS, lfoTrack, type Lfos } from "../../lib/lfo";
import { useSidecar } from "../../lib/useSidecar";
import { useSend } from "../../lib/useSend";

const { status, paired } = useSidecar();
const send = useSend();
const lfos = computed(() => (status.value as Record<string, any> | null)?.lfo as Lfos | undefined);
const track = ref(0);
watch(() => lfos.value?.trk, (t) => { if (t !== undefined && t >= 0 && t < LFO_TRACKS.length) track.value = t; }, { immediate: true });
const shown = computed(() => (lfos.value?.l ?? []).map((l, i) => ({ l, n: i + 1 })).filter((x) => !lfos.value?.pt || lfoTrack(x.n, 1) === track.value));
</script>

<template>
  <Card label="Modulation">
    <p v-if="!lfos">This build reports no LFOs.</p>
    <template v-else>
      <p v-if="lfos.full" class="small full" role="status">{{ LFO_FULL }}</p>
      <p class="small">{{ lfos.pt ? "Two LFOs a track (A any target, B the Sidecar's)" : `${lfos.n} LFOs` }}, saved with kit {{ lfos.kit }}. The shape shows the phase now.</p>
      <Segmented v-if="lfos.pt" label="Track" :options="LFO_TRACKS.map((t, i) => ({ value: i, text: t }))" :model-value="track" @update:model-value="(v: number) => (track = v)" />
      <div class="lfos">
        <LfoCard v-for="x in shown" :key="x.n" :n="x.n" :pt="lfos.pt" :lfo="x.l" :disabled="!paired" @cmd="send" />
      </div>
    </template>
  </Card>
</template>

<style scoped>
.lfos { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 18rem), 1fr)); gap: 0.75rem; }
.full { color: var(--bad, var(--warm)); font-weight: 600; }
</style>
