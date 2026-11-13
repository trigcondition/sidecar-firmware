<!-- Rec › the recorder (recorder.h, rec_pi.cpp; the old page's Record tab): record / arm / stop, the time, the state,
     manual or arm (the Rytm's PLAY starts it), 24 / 16 bit, the destination, the take name, the channels with their
     meters, what the selection needs of the card, the warnings. The Sidecar's own settings are shown (rec.ch, bits,
     vol): a change is sent at once, so every browser and the touchscreen agree. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Chip from "../../ui/Chip.vue";
import Segmented from "../../ui/Segmented.vue";
import SaveState from "../../ui/SaveState.vue";
import { useSetting } from "../../lib/pending";
import { confirmAsk } from "../../lib/confirm";
import { useSidecar } from "../../lib/useSidecar";
import { REC_GROUPS, REC_STATES, hms, recBytesPerSec, recCmd, recDestinations, recLoad, recWarn, takeNameOk } from "../../lib/rec";
import { hb } from "../../lib/fs";

const { status, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const r = computed(() => st.value.rec as Record<string, any> | undefined);
const busy = computed(() => !!r.value && r.value.state !== 0);
const sel = computed<number[]>(() => (Array.isArray(r.value?.ch) ? r.value!.ch : []));

// manual or arm: the page's own choice (as the old page; kept per device)
const mode = ref<"manual" | "arm">("manual");
try {
  const j = JSON.parse(localStorage.getItem("sidecar.rec") || "null");
  if (j?.mode === "arm") mode.value = "arm";
} catch {
  /* storage blocked */
}
watch(mode, (m) => {
  try {
    const j = JSON.parse(localStorage.getItem("sidecar.rec") || "{}");
    localStorage.setItem("sidecar.rec", JSON.stringify({ ...j, mode: m }));
  } catch {
    /* not kept */
  }
});

const bits = useSetting<number>(status, { read: () => r.value?.bits, toCmd: (b) => recCmd.bits(b as 16 | 24), send: cmd });
const vol = useSetting<string>(status, { read: () => r.value?.vol, toCmd: (v) => recCmd.vol(v), send: cmd });
const dests = computed(() => recDestinations(st.value.stor?.vols, r.value?.vol ?? "SD"));
const name = ref("");
watch(() => r.value?.prefix, (p) => { if (typeof p === "string" && document.activeElement?.id !== "rec-name") name.value = p; }, { immediate: true });
const msg = ref("");

async function send(c: string) {
  msg.value = "";
  try {
    await cmd(c);
  } catch (e) {
    msg.value = (e as Error).message;
  }
}
function toggle(id: number) {
  if (busy.value) return;
  const s = new Set(sel.value);
  s.has(id) ? s.delete(id) : s.add(id);
  send(recCmd.ch([...s]));
}
const allIds = REC_GROUPS.flatMap((g) => g.ch.map((c) => c[0]));
function saveName() {
  if (!takeNameOk(name.value)) return void (msg.value = "Take name: up to 24 of A-Z a-z 0-9 _ - (empty: the project's name).");
  send(recCmd.name(name.value));
}

const needKBps = computed(() => recBytesPerSec(sel.value, (r.value?.bits ?? 24) as 16 | 24) / 1024);
const load = computed(() => recLoad(needKBps.value, r.value?.speedKBps ?? 0));
const secs = computed(() => (r.value ? (r.value.state === 1 ? 0 : r.value.frames / 48000) : 0));
const left = computed(() => {
  const x = r.value;
  if (!x?.totalMB || !needKBps.value) return "";
  return hms((x.freeMB * 1024) / needKBps.value);
});
const warnings = computed(() => {
  const x = r.value;
  if (!x) return [];
  const drv = x.vol && x.vol !== "SD", dn = drv ? "the drive" : "the card";
  const w: string[] = [];
  if (drv && x.state === 0 && x.volOk === 0) w.push(`${x.vol} not mounted: plug it in, or record to the SD card`);
  if (x.drops) w.push(`${dn} fell behind: ${x.drops} blocks dropped (${Math.round(x.silence / 48)} ms written as silence)`);
  if (x.outShort) w.push(`${x.outShort} Sidecar frames missing (written as silence)`);
  if (x.error) w.push(x.error);
  if (x.abandoned) w.push("the drive stopped answering: the take's files were left as they were");
  return w;
});

async function go() {
  const x = r.value;
  if (!x) return;
  if (busy.value) return send(recCmd.stop);
  if (!sel.value.length) return void (msg.value = "Select a channel first.");
  const w = recWarn(needKBps.value, x.speedKBps ?? 0);
  if (w && !(await confirmAsk({ title: "Record anyway?", body: `This take needs ${(needKBps.value / 1024).toFixed(1)} MB/s: ${w} % of ${x.vol && x.vol !== "SD" ? `the ${x.vol} drive` : "the SD card"}'s speed. The card may fall behind; missed blocks are written as silence. Fewer channels or 16-bit need less.`, yes: "Record anyway" }))) return;
  send(recCmd.go(mode.value === "arm"));
}
const goText = computed(() => {
  const s = r.value?.state ?? 0;
  return s === 0 ? (mode.value === "arm" ? "Arm" : "Record") : s === 1 ? "Disarm" : "Stop";
});
const stateText = computed(() => {
  const x = r.value;
  if (!x) return "";
  if (!x.ready) return "Recording is off (cmdline recbuf=0, or no card).";
  if (x.state === 1) return `Armed: waiting for PLAY${x.take ? ` (last: ${x.take})` : ""}`;
  if (x.state === 2) return x.take ?? "recording";
  if (x.state === 3) return `${x.take}: closing the files`;
  return x.take ? `Last take: ${x.take}` : "Ready";
});
const peak = (id: number) => (Array.isArray(r.value?.peak) ? (r.value!.peak[id] ?? 0) / 1000 : 0);
</script>

<template>
  <Card label="Recorder" :tone="r && r.state === 2 ? 'warm' : 'accent'">
    <p v-if="!r">This build reports no recorder.</p>
    <template v-else>
      <div class="top">
        <button type="button" class="go" :class="`s${r.state}`" :disabled="!paired || !r.ready" @click="go">
          <span aria-hidden="true">{{ r.state === 0 ? "●" : "■" }}</span> {{ goText }}
        </button>
        <span class="time num" :class="{ live: r.state === 2 }" role="timer" :aria-label="`Recorded ${hms(secs)}`">{{ hms(secs) }}</span>
        <span class="state" role="status">{{ REC_STATES[r.state] }} · {{ stateText }}</span>
      </div>

      <div class="opts">
        <div><span class="label">Mode</span>
          <Segmented label="Mode" :options="[{ value: 'manual', text: 'Manual' }, { value: 'arm', text: 'Arm (PLAY starts it)' }]" :model-value="mode" :disabled="busy" @update:model-value="(v: string) => (mode = v as 'manual' | 'arm')" /></div>
        <div><span class="label">Bits</span>
          <Segmented label="Bits" :options="[{ value: 24, text: '24-bit' }, { value: 16, text: '16-bit' }]" :model-value="bits.value.value ?? 24" :disabled="busy || !paired" @update:model-value="bits.set" />
          <SaveState :state="bits.state.value" :msg="bits.msg.value" /></div>
        <div><span class="label">Record to</span>
          <Segmented label="Record to" :options="dests.map((d) => ({ value: d.name, text: d.ok ? d.name : `${d.name} (not mounted)` }))" :model-value="vol.value.value ?? 'SD'" :disabled="busy || !paired" @update:model-value="vol.set" />
          <SaveState :state="vol.state.value" :msg="vol.msg.value" /></div>
        <form @submit.prevent="saveName"><label for="rec-name" class="label">Take name</label>
          <span class="row"><input id="rec-name" v-model="name" maxlength="24" placeholder="the project's" autocomplete="off" :disabled="busy" /><button type="submit" :disabled="busy || !paired">Set</button></span></form>
      </div>

      <span class="label">Channels</span>
      <div class="row">
        <button type="button" :disabled="busy || !paired" @click="send(recCmd.ch(allIds))">Select all</button>
        <button type="button" :disabled="busy || !paired" @click="send(recCmd.ch([]))">None</button>
        <span class="small">
          {{ sel.length }} file{{ sel.length === 1 ? "" : "s" }} · <span class="num">{{ (needKBps / 1024).toFixed(2) }}</span> MB/s
          <template v-if="r.speedKBps"> · <span :class="{ warm: recWarn(needKBps, r.speedKBps) }"><span class="num">{{ load }}</span> % of {{ r.vol && r.vol !== "SD" ? r.vol : "the card" }}'s {{ (r.speedKBps / 1024).toFixed(1) }} MB/s{{ r.speedMeasured ? "" : " (estimated)" }}</span></template>
          <template v-if="left"> · {{ hb(r.freeMB * 1048576) }} free, <span class="num">{{ left }}</span> left</template>
        </span>
      </div>
      <div v-for="g in REC_GROUPS" :key="g.name" class="group">
        <span class="small">{{ g.name }}</span>
        <div class="chs">
          <div v-for="[id, n] in g.ch" :key="id" class="ch">
            <Chip :pressed="sel.includes(id)" :disabled="busy || !paired" @click="toggle(id)">{{ n }}</Chip>
            <span class="bar" aria-hidden="true"><i :class="{ hot: peak(id) >= 0.99 }" :style="{ width: `${sel.includes(id) && r.state ? Math.min(100, peak(id) * 100) : 0}%` }" /></span>
          </div>
        </div>
      </div>

      <ul v-if="warnings.length" class="warn" role="alert"><li v-for="w in warnings" :key="w"><span aria-hidden="true">⚠ </span>{{ w }}</li></ul>
      <p v-if="r.writes" class="small">
        {{ r.vol && r.vol !== "SD" ? `${r.vol} drive` : "Card" }}: {{ (r.cardKBps / 1024).toFixed(1) }} MB/s while writing (needs {{ (r.needKBps / 1024).toFixed(2) }}),
        worst write {{ r.worstWriteMs }} ms, buffer {{ (r.ringMs / 1000).toFixed(1) }} s, {{ hb(r.bytes * 1024) }} written.
      </p>
      <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
    </template>
  </Card>
</template>

<style scoped>
.top { display: flex; flex-wrap: wrap; align-items: center; gap: 0.75rem 1.25rem; }
.go { font-size: 1.25rem; font-weight: 700; padding: 0.75rem 1.5rem; border-radius: var(--radius-pill); min-width: 9rem; }
.go.s0 span { color: var(--warm); }
.go.s1 { border: 2px solid var(--warm); color: var(--warm); }
.go.s2, .go.s3 { background: var(--warm); color: var(--on-accent); border-color: var(--warm); }
.time { font-size: 2.5rem; line-height: 1; }
.time.live { color: var(--warm); }
.state { color: var(--label); }
.opts { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 15rem), 1fr)); gap: 0.75rem 1.25rem; }
.opts > * { display: flex; flex-direction: column; gap: 0.375rem; align-items: flex-start; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.group { display: flex; flex-direction: column; gap: 0.25rem; }
.chs { display: grid; grid-template-columns: repeat(auto-fill, minmax(8.5rem, 1fr)); gap: 0.375rem; }
.ch { display: flex; flex-direction: column; gap: 0.125rem; }
.ch :deep(.chip) { width: 100%; text-align: left; }
.bar { height: 0.375rem; border-radius: 3px; background: var(--border); overflow: hidden; }
.bar i { display: block; height: 100%; background: var(--accent); }
.bar i.hot { background: var(--warm); }
.warn { margin: 0; padding: 0; list-style: none; color: var(--warm); }
</style>
