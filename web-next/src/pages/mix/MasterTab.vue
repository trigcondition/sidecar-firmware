<!-- web-next/src/pages/mix/MasterTab.vue - Mix > Master FX: the master chain, its source, the stem trim, the limiter. -->
<script setup lang="ts">
import { computed } from "vue";
import Card from "../../ui/Card.vue";
import Fader from "../../ui/Fader.vue";
import Segmented from "../../ui/Segmented.vue";
import { mixCmd } from "../../lib/mix";
import { useSidecar } from "../../lib/useSidecar";
import { useSend } from "../../lib/useSend";

const { status, paired } = useSidecar();
const send = useSend();
const mfx = computed(() => (status.value as Record<string, any> | null)?.ins?.mfx as Record<string, any> | undefined);
</script>

<template>
  <p v-if="status && !mfx" class="card">This Sidecar's build reports no master FX.</p>
  <Card v-if="mfx" label="Master FX">
    <span class="label">Master chain</span>
    <Segmented label="Master chain" :options="[{ value: 0, text: 'On' }, { value: 1, text: 'Bypassed' }]" :model-value="mfx.byp" :disabled="!paired" @update:model-value="(v: number) => send(mixCmd.mfxBypass(v === 1))" />
    <span class="label">Source</span>
    <Segmented label="Master source" :options="[{ value: 'ret', text: 'FX returns' }, { value: 'mix', text: 'The mix' }]" :model-value="mfx.src" :disabled="!paired" @update:model-value="(v: string) => send(mixCmd.mfxSource(v as 'ret' | 'mix'))" />
    <Fader label="Stem trim" :model-value="mfx.trim" :min="-24" :max="0" unit="dB" :disabled="!paired" @commit="(v: number) => send(mixCmd.mfxTrim(v))" />
    <p>Limiter gain reduction <span class="num warm">{{ mfx.gr }} dB</span></p>
    <div class="row">
      <button type="button" :disabled="!paired" @click="send('mfx=reset')">Clear the chain</button>
      <button type="button" :disabled="!paired" @click="send('mfx=stats')">Stats to the log</button>
    </div>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; gap: 0.5rem; }
</style>
