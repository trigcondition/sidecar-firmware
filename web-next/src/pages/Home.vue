<!-- web-next/src/pages/Home.vue - the Sidecar at a glance (the old page's Status tab and its Log & console): the tiles,
     the box's tracks, the pairing while unpaired, the live log and a console line. -->
<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, shallowRef, triggerRef } from "vue";
import Card from "../ui/Card.vue";
import Tile from "../ui/Tile.vue";
import { RequestError } from "../lib/client";
import { LogBuffer, type LogPush } from "../lib/logbuf";
import { STATS_LINE, boxName, buildTiles } from "../lib/tiles";
import { useSidecar } from "../lib/useSidecar";
import { REC_STATES, hms } from "../lib/rec";
import { boxes as boxList, has } from "../lib/box";
import { uptimeText } from "../lib/status";

const { client, state, hello, paired, status, pair, api, cmd } = useSidecar();

// the log: its pushes into lines; the stats lines feed the tiles
const buf = new LogBuffer(3000);
const lines = shallowRef(buf.lines);
const stats = shallowRef(buf.stats);
const follow = ref(true);
const hideStats = ref(true);
const logEl = ref<HTMLElement | null>(null);
let off: (() => void) | null = null;
onMounted(() => {
  off = client.subscribe<LogPush>("log", (p) => {
    buf.add(p);
    lines.value = buf.lines;
    triggerRef(lines);
    stats.value = { ...buf.stats };
    if (follow.value) nextTick(() => logEl.value && (logEl.value.scrollTop = logEl.value.scrollHeight));
  });
});
onUnmounted(() => off?.());

const tiles = computed(() => buildTiles(status.value, stats.value));
const shown = computed(() => (hideStats.value ? lines.value.filter((l) => !STATS_LINE.test(l.text)) : lines.value));
const isError = (t: string) => /ERROR|fail|fault|warn/i.test(t);
const box = computed(() => boxName(status.value));
const rec = computed(() => (status.value as Record<string, any> | null)?.rec as Record<string, any> | undefined);

// the box's tracks (/status "src"): the machine each one reports; hidden until any has reported
const TRACK = ["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "T9", "T10", "T11", "T12"];
const src = computed(() => {
  const s = (status.value as Record<string, unknown> | null)?.src;
  if (!Array.isArray(s) || !s.some((x) => x && x.vb >= 0)) return null;
  return s.map((x, i) => ({ name: TRACK[i] ?? `T${i + 1}`, m: (x && x.m) || "-", ours: x && x.m && x.m !== "RYTM" && x.m !== "?" }));
});

// pairing
const code = ref("");
const pairMsg = ref("");
async function doPair() {
  pairMsg.value = "";
  try {
    await pair(code.value);
    code.value = "";
    pairMsg.value = paired.value ? "Paired." : "Paired, but the Sidecar did not take the token yet.";
  } catch (e) {
    pairMsg.value = (e as Error).message;
  }
}
async function showCode() {
  try {
    const r = await api.post<{ shown?: boolean; wait?: number }>("/pair/show");
    pairMsg.value = r.shown ? `The code is on the ${box.value}'s screen now.` : `Try again in ${r.wait ?? 10} s.`;
  } catch (e) {
    pairMsg.value = (e as Error).message;
  }
}

// the console line
const line = ref("");
const lineMsg = ref("");
async function send() {
  const c = line.value.trim().replace(/^:/, "");
  if (!c) return;
  lineMsg.value = "";
  try {
    await cmd(c);
    line.value = "";
  } catch (e) {
    lineMsg.value = e instanceof RequestError ? `Refused (${e.code}): ${e.message}` : (e as Error).message;
  }
}
</script>

<template>
  <h1 class="visually-hidden" tabindex="-1">Home</h1>

  <Card v-if="!paired && state === 'open'" label="Pair this browser" tone="warm">
    <p>Changes need the pairing code. It is on the {{ box }}'s screen and the Sidecar's screen.</p>
    <form class="row" @submit.prevent="doPair">
      <label for="code">Pairing code</label>
      <input id="code" v-model="code" inputmode="numeric" autocomplete="off" maxlength="9" />
      <button type="submit" :disabled="code.replace(/\s/g, '').length !== 8">Pair</button>
      <button v-if="has(status, 'remoteui')" type="button" @click="showCode">Show it on the {{ box }}</button>
    </form>
    <p v-if="pairMsg" role="status">{{ pairMsg }}</p>
  </Card>

  <Card label="Status">
    <p class="meta">
      <template v-if="status">{{ status.host }}.local · up <span class="num">{{ uptimeText(status.uptime) }}</span></template>
      <template v-if="hello"> · Sidecar {{ hello.version }}</template>
    </p>
    <ul v-if="boxList(status).length" class="boxes" aria-label="Boxes on USB">
      <li v-for="b in boxList(status)" :key="b.id" :class="{ on: b.on }">
        <b>{{ b.name }}</b> <span class="small">· {{ b.role }}</span>
      </li>
    </ul>
    <div class="tiles">
      <Tile v-for="t in tiles" :key="t.key" :tile="t" />
    </div>
    <p v-if="rec" class="rec">
      <span class="label">Recorder</span>
      <span :class="{ warm: rec.state === 2 }"><span v-if="rec.state === 2" aria-hidden="true">● </span>{{ REC_STATES[rec.state] }}</span>
      <span v-if="rec.state === 2" class="num warm">{{ hms(rec.frames / 48000) }}</span>
      <span v-else-if="rec.take" class="small">last take {{ rec.take }}</span>
      <router-link to="/rec">Rec page</router-link>
    </p>
  </Card>

  <Card v-if="src" :label="`${box} tracks`">
    <ul class="src">
      <li v-for="t in src" :key="t.name" :class="{ ours: t.ours }">
        <span class="label">{{ t.name }}</span>
        <span>{{ t.m }}</span>
      </li>
    </ul>
    <p class="small">The machine each track reports. {{ box.toUpperCase() }}: the {{ box }}'s own sound, not a Sidecar voice.</p>
  </Card>

  <Card label="Log and console">
    <div class="row">
      <label class="check"><input v-model="follow" type="checkbox" /> Follow</label>
      <label class="check"><input v-model="hideStats" type="checkbox" /> Hide the once-a-second lines</label>
      <button type="button" @click="buf.clear(); lines = buf.lines">Clear</button>
    </div>
    <div ref="logEl" class="log" role="log" aria-label="Sidecar log" tabindex="0">
      <div v-for="l in shown" :key="l.n" :class="{ err: isError(l.text), gap: l.gap }">
        <span v-if="isError(l.text)" class="visually-hidden">Warning: </span>{{ l.text }}
      </div>
      <p v-if="!shown.length" class="small">Waiting for the log…</p>
    </div>
    <form class="row" @submit.prevent="send">
      <label for="console">Console</label>
      <input id="console" v-model="line" class="grow" autocomplete="off" spellcheck="false" placeholder="notes=1" :disabled="!paired" />
      <button type="submit" :disabled="!paired || !line.trim()">Send</button>
    </form>
    <p v-if="!paired" class="small">The console needs the pairing.</p>
    <p v-if="lineMsg" role="status" class="warm">{{ lineMsg }}</p>
  </Card>
</template>

<style scoped>
.meta { margin: 0; color: var(--label); }
.boxes { list-style: none; margin: 0; padding: 0; display: flex; flex-wrap: wrap; gap: 0.5rem; }
.boxes li { border: 1px solid var(--border); border-radius: var(--radius-pill); padding: 0.25rem 0.75rem; }
.boxes li.on { border: 2px solid var(--accent); }
.rec { display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.25rem 0.75rem; margin: 0; }
.rec a { color: var(--accent); }
.tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(11rem, 1fr)); gap: 0.75rem; }
.src { list-style: none; margin: 0; padding: 0; display: grid; grid-template-columns: repeat(auto-fill, minmax(6rem, 1fr)); gap: 0.5rem; }
.src li { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem; display: flex; flex-direction: column; }
.src li.ours { border-color: var(--accent); }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.grow { flex: 1 1 12rem; }
.check { display: inline-flex; align-items: center; gap: 0.375rem; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
.log {
  height: 18rem; overflow: auto; font-family: ui-monospace, Menlo, monospace; font-size: var(--text-min);
  border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem; white-space: pre-wrap; overflow-wrap: anywhere;
  background: var(--bg);
}
.log .err { color: var(--warm); }
.log .gap { color: var(--label); font-style: italic; }
</style>
