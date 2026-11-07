<!-- Settings › Access: the pairing code's gate (pi/engine/paircode.h): how many devices are remembered, this browser's
     pairing, forget this device / every device. -->
<script setup lang="ts">
import { computed, onMounted, ref } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { confirmAsk } from "../../lib/confirm";

const { api, paired, pairing, client, status } = useSidecar();
const ps = ref<{ gate: string; clients: number; host: string } | null>(null);
const msg = ref("");
async function load() {
  try {
    ps.value = await api.json("/pair/status");
  } catch (e) {
    msg.value = (e as Error).message;
  }
}
onMounted(load);
const gate = computed(() => ps.value?.gate ?? ((status.value as Record<string, any> | null)?.pair?.gate as string | undefined));
const remembered = computed(() => pairing.saved() != null);

async function forgetMe() {
  if (!(await confirmAsk({ title: "Forget this device", body: "This browser will need the pairing code again.", yes: "Forget this device", danger: true }))) return;
  await api.post("/pair/forget").catch(() => undefined);
  pairing.forgetLocal();
  await client.authenticate().catch(() => false);
  await load();
}
async function forgetAll() {
  if (!(await confirmAsk({ title: "Forget every device", body: "Every browser and tool, this one too, will need the pairing code again. The MIDI bridge closes.", yes: "Forget every device", danger: true }))) return;
  await api.post("/pair/revoke").catch(() => undefined);
  pairing.forgetLocal();
  await client.authenticate().catch(() => false);
  await load();
}
</script>

<template>
  <Card label="Access">
    <p>
      Pairing code:
      <b>{{ gate === "off" ? "off (anyone on the network can change things)" : gate === "fixed" ? "fixed code" : "on" }}</b>
      <template v-if="ps"> · {{ ps.clients }} device{{ ps.clients === 1 ? "" : "s" }} remembered</template>
    </p>
    <p>This browser: {{ paired ? (remembered ? "paired and remembered" : "paired for this visit") : "not paired" }}.</p>
    <div class="row">
      <button type="button" :disabled="!paired" @click="forgetMe">Forget this device</button>
      <button type="button" class="danger" :disabled="!paired" @click="forgetAll">Forget every device</button>
    </div>
    <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; gap: 0.5rem; }
.danger { border-color: var(--warm); color: var(--warm); }
</style>
