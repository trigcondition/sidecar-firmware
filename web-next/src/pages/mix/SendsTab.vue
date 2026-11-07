<!-- web-next/src/pages/mix/SendsTab.vue - Mix > Send FX: the two send effects, their returns and the tracks' sends. -->
<script setup lang="ts">
import { computed } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { useSend } from "../../lib/useSend";

const { status, paired } = useSidecar();
const send = useSend();
const fx = computed(() => ((status.value as Record<string, any> | null)?.fx ?? []) as Record<string, any>[]);
</script>

<template>
  <Card label="Send FX">
    <ul class="fxl">
      <li v-for="(f, i) in fx" :key="i">
        <b>FX {{ i + 1 }}</b> · {{ f.type || "OFF" }} · returns {{ f.post ? "POST (0/1)" : "PRE (2/3)" }}
        <div class="sends" :aria-label="`FX ${i + 1} sends`" role="img">
          <span v-for="(v, k) in (f.send as number[])" :key="k" :title="`track ${k + 1}: ${v}`"><i :style="{ height: `${(v / 127) * 100}%` }" /><small>{{ k + 1 }}</small></span>
        </div>
        <p class="visually-hidden">Sends: {{ (f.send as number[]).map((v, k) => `track ${k + 1} ${v}`).join(", ") }}</p>
      </li>
    </ul>
    <p class="small">The kit on the Rytm sets these; they show here.</p>
    <button type="button" :disabled="!paired" @click="send('killfx')">Kill the FX tails</button>
  </Card>
</template>

<style scoped>
.fxl { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.75rem; }
.sends { display: grid; grid-template-columns: repeat(12, 1fr); gap: 0.25rem; height: 3rem; margin-top: 0.375rem; max-width: 40rem; }
.sends span { display: flex; flex-direction: column; justify-content: flex-end; align-items: center; height: 100%; }
.sends i { display: block; width: 100%; background: var(--accent); min-height: 1px; }
.sends small { font-size: var(--text-min); color: var(--label); }
</style>
