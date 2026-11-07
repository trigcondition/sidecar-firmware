<!-- Settings › Wi-Fi (pi/engine/wifi.h; the old page's Status › Wi-Fi). The only place an SSID is shown. Join: the name
     and password in a POST body only; the password field is cleared at once and never kept. Connect and save restarts
     the Sidecar; the page follows the join through the restart (join.state) and says why it failed. -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import { useSidecar } from "../../lib/useSidecar";
import { confirmAsk } from "../../lib/confirm";
import { JOIN_REASON, bars, checkJoin } from "../../lib/wifi";

interface Net { ssid: string; rssi: number; ch: number; band: string; secured: boolean }
interface WifiStatus { on: boolean; net: string; ip: string; ssid: string; connected: boolean; scanning: boolean; cmdline: boolean; trialSecs: number; join: { state: string; reason: string; left: number }; networks: Net[]; chip: string }

const { api, paired } = useSidecar();
const w = ref<WifiStatus | null>(null);
const err = ref("");
const busy = ref(false);
let timer: ReturnType<typeof setInterval> | null = null;
let following = false;

async function load() {
  if (!paired.value) return;
  try {
    w.value = await api.json<WifiStatus>("/wifi/status");
    err.value = "";
    if (following && w.value.join && (w.value.join.state === "up" || w.value.join.state === "failed")) following = false;
  } catch (e) {
    err.value = following ? "The Sidecar is restarting for the new network…" : (e as Error).message;
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 3000);
});
onUnmounted(() => timer && clearInterval(timer));

async function power(on: boolean) {
  busy.value = true;
  try {
    await api.post("/wifi/power", { on: on ? 1 : 0 });
    await load();
  } finally {
    busy.value = false;
  }
}
async function scan() {
  await api.json("/wifi/scan").catch(() => undefined);
  setTimeout(load, 5000);
}

// the join form
const pick = ref("");
const other = ref(false);
const ssid = ref("");
const psk = ref("");
const showPsk = ref(false);
const open = ref(false);
const formMsg = ref("");
const chosen = computed(() => (other.value ? ssid.value : pick.value));
const net = computed(() => w.value?.networks.find((n) => n.ssid === pick.value));
function choose(n: Net) {
  pick.value = n.ssid;
  other.value = false;
  open.value = !n.secured;
}
async function join() {
  const name = chosen.value, pass = open.value ? "" : psk.value;
  psk.value = ""; // never kept, whatever happens next
  const bad = checkJoin(name, pass, open.value);
  if (bad) {
    formMsg.value = bad;
    return;
  }
  const ok = await confirmAsk({
    title: "Connect and save",
    body: `The Sidecar restarts to join "${name}" (one network at a time). If it is not up within ${w.value?.trialSecs ?? 90} s it goes back to how it is now.`,
    yes: "Restart and join",
  });
  if (!ok) return;
  try {
    const r = await api.post<{ ok: boolean; error?: string }>("/wifi/join", { ssid: name, psk: pass, open: open.value ? 1 : 0 });
    formMsg.value = r.ok ? "Restarting to join… this page follows it." : (r.error ?? "refused");
    if (r.ok) following = true;
  } catch (e) {
    formMsg.value = (e as Error).message;
  }
}
async function forget() {
  if (!(await confirmAsk({ title: "Forget network", body: "The stored network and its key are removed; the Sidecar uses Ethernet from its next start.", yes: "Forget it", danger: true }))) return;
  await api.post("/wifi/forget");
  await load();
}
const joinText = computed(() => {
  const j = w.value?.join;
  if (!j || j.state === "none") return "";
  if (j.state === "restarting") return "Restarting to join…";
  if (j.state === "trying") return `Trying… ${j.left} s left`;
  if (j.state === "up") return "Joined.";
  if (j.state === "failed") return `It did not join: ${JOIN_REASON[j.reason] ?? j.reason}. Back on the previous setting.`;
  return j.state;
});
</script>

<template>
  <Card label="Wi-Fi">
    <p v-if="!paired">The Wi-Fi settings need the pairing.</p>
    <template v-else-if="w">
      <p>
        On <b>{{ w.net === "wifi" ? `Wi-Fi “${w.ssid}”` : "Ethernet" }}</b>
        <template v-if="w.ip"> · {{ w.ip }}</template>
        <template v-if="w.ssid && w.net !== 'wifi'"> · stored: “{{ w.ssid }}”</template>
      </p>
      <p v-if="w.cmdline" class="small">cmdline.txt sets the network: the settings here do not apply.</p>
      <Segmented label="Wi-Fi" :options="[{ value: true, text: 'On' }, { value: false, text: 'Off' }]" :model-value="w.on" :disabled="busy" @update:model-value="power" />
      <p v-if="joinText" role="status" :class="{ warm: w.join.state === 'failed' }">{{ joinText }}</p>

      <template v-if="w.on">
        <div class="row">
          <span class="label">Networks</span>
          <button type="button" @click="scan">Scan again</button>
        </div>
        <ul class="nets" aria-label="Networks">
          <li v-for="n in w.networks" :key="n.ssid">
            <button type="button" class="net" :aria-pressed="!other && pick === n.ssid" @click="choose(n)">
              <span v-if="!other && pick === n.ssid" aria-hidden="true">✓ </span>
              <span class="name">{{ n.ssid }}</span>
              <span class="small">{{ n.secured ? "secured" : "open" }} · {{ n.band }} GHz · signal {{ bars(n.rssi) }} of 4</span>
            </button>
          </li>
          <li>
            <button type="button" class="net" :aria-pressed="other" @click="other = true; pick = ''">
              <span v-if="other" aria-hidden="true">✓ </span>Other network…
            </button>
          </li>
        </ul>
        <form v-if="other || pick" class="join" @submit.prevent="join">
          <div v-if="other" class="f">
            <label for="wf-ssid">Network name</label>
            <input id="wf-ssid" v-model="ssid" autocomplete="off" maxlength="32" />
          </div>
          <label class="check"><input v-model="open" type="checkbox" /> Open network (no password)</label>
          <div v-if="!open" class="f">
            <label for="wf-psk">Password</label>
            <span class="row">
              <input id="wf-psk" v-model="psk" :type="showPsk ? 'text' : 'password'" autocomplete="off" maxlength="64" />
              <button type="button" :aria-pressed="showPsk" aria-controls="wf-psk" @click="showPsk = !showPsk">{{ showPsk ? "Hide password" : "Show password" }}</button>
            </span>
          </div>
          <div class="row">
            <button type="submit" :disabled="!chosen">Connect and save</button>
            <span v-if="net" class="small">{{ net.ssid }}</span>
          </div>
          <p v-if="formMsg" role="status" class="small">{{ formMsg }}</p>
        </form>
      </template>
      <button v-if="w.ssid" type="button" @click="forget">Forget network</button>
    </template>
    <p v-else-if="err" class="warm" role="status">{{ err }}</p>
    <p v-else>Loading…</p>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.nets { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.375rem; }
.net { width: 100%; text-align: left; display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.25rem 0.75rem; }
.net[aria-pressed="true"] { border: 2px solid var(--accent); background: var(--accent-tint); font-weight: 700; }
.name { flex: 1 1 8rem; }
.join { display: flex; flex-direction: column; gap: 0.5rem; }
.f { display: flex; flex-direction: column; gap: 0.25rem; }
.check { flex-direction: row !important; align-items: center; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
</style>
