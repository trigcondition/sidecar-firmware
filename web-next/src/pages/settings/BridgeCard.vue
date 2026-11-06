<!-- Settings › MIDI bridge (pi/engine/midibridge_pi.h): the Rytm's MIDI over the network. Its token is fetched only while
     it is on, shown only when asked, never put in a URL. OS upgrades and drive writes are confirmed first. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import { useSidecar } from "../../lib/useSidecar";
import { confirmAsk } from "../../lib/confirm";
import { boxName } from "../../lib/tiles";

const { api, paired, status } = useSidecar();
const mb = computed(() => (status.value as Record<string, any> | null)?.midibridge as Record<string, any> | undefined);
const box = computed(() => boxName(status.value));
const token = ref("");
const show = ref(false);
const msg = ref("");
const busy = ref(false);

async function fetchToken() {
  if (!paired.value || !mb.value?.on) {
    token.value = "";
    return;
  }
  try {
    token.value = ((await api.json<{ token?: string }>("/midibridge")).token ?? "") as string;
  } catch {
    token.value = "";
  }
}
watch(() => [paired.value, mb.value?.on, mb.value?.state], fetchToken, { immediate: true });

async function set(k: "on" | "os" | "data", v: boolean) {
  if (v && k !== "on") {
    const what = k === "os" ? `OS upgrades for the ${box.value} through the bridge` : "writes to the +Drive through the bridge";
    if (!(await confirmAsk({ title: "MIDI bridge", body: `Allow ${what}? Only for a tool you trust, while you use it.`, yes: "Allow" }))) return;
  }
  busy.value = true;
  msg.value = "";
  try {
    const r = await api.post<{ ok?: boolean; error?: string }>("/midibridge/set", { [k]: v ? 1 : 0 });
    if (r.ok === false) msg.value = r.error ?? "refused";
  } catch (e) {
    msg.value = (e as Error).message;
  } finally {
    busy.value = false;
  }
}
async function copy() {
  try {
    await navigator.clipboard.writeText(token.value);
    msg.value = "Copied.";
  } catch {
    msg.value = "The browser did not allow copying: select it instead.";
  }
}
</script>

<template>
  <Card label="MIDI bridge">
    <p v-if="!mb">Not in this build.</p>
    <template v-else>
      <p>{{ box }} MIDI over the network: <b>{{ mb.state }}</b><template v-if="mb.on"> · port {{ mb.port }}</template></p>
      <Segmented label="MIDI bridge" :options="[{ value: true, text: 'On' }, { value: false, text: 'Off' }]" :model-value="!!mb.on" :disabled="!paired || busy" @update:model-value="(v: boolean) => set('on', v)" />
      <template v-if="mb.on">
        <div class="row">
          <span class="label">Token</span>
          <code v-if="show" class="tok">{{ token || "–" }}</code>
          <span v-else class="small">hidden</span>
          <button type="button" :aria-pressed="show" @click="show = !show">{{ show ? "Hide" : "Show" }}</button>
          <button type="button" :disabled="!token" @click="copy">Copy</button>
        </div>
        <span class="label">OS upgrades through the bridge</span>
        <Segmented label="OS upgrades" :options="[{ value: true, text: 'Allow' }, { value: false, text: 'Block' }]" :model-value="!!mb.osUpgrade" :disabled="!paired || busy" @update:model-value="(v: boolean) => set('os', v)" />
        <span class="label">+Drive writes through the bridge</span>
        <Segmented label="Drive writes" :options="[{ value: true, text: 'Allow' }, { value: false, text: 'Block' }]" :model-value="!!mb.dataWrites" :disabled="!paired || busy" @update:model-value="(v: boolean) => set('data', v)" />
        <p class="small">To the {{ box }} <span class="num">{{ mb.toRytm ?? 0 }}</span> B · from it <span class="num">{{ mb.fromRytm ?? 0 }}</span> B · dropped <span class="num">{{ mb.drops ?? 0 }}</span></p>
      </template>
      <p v-if="msg" class="small" role="status">{{ msg }}</p>
    </template>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.tok { overflow-wrap: anywhere; }
</style>
