<!-- web-next/src/pages/Home.vue - the Sidecar at a glance (the old page's Status tab and its Log & console): a health row
     (one pill per area, coral when it needs looking at), then the tiles in three groups - Engine, Connections, System -
     drawn from lib/health.ts, the box's tracks, the pairing while unpaired, and the log with a console line (the last
     lines, the whole log on request). A power warning is said once, by the banner on every page (App.vue); its
     "Details" brings this page with ?show=power, and router.ts focuses that tile (id tile-power). -->
<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, shallowRef, triggerRef, watch } from "vue";
import Card from "../ui/Card.vue";
import Chip from "../ui/Chip.vue";
import Gauge from "../ui/Gauge.vue";
import Spark from "../ui/Spark.vue";
import StatTile from "../ui/StatTile.vue";
import { RequestError } from "../lib/client";
import { LogBuffer, type LogPush } from "../lib/logbuf";
import { STATS_LINE, boxName, shortBoxName } from "../lib/tiles";
import { TEMP, pills as healthPills, pushSample, readEngine, readOb, readPower, readUsbC } from "../lib/health";
import { useSidecar } from "../lib/useSidecar";
import { REC_STATES, hms } from "../lib/rec";
import { boxes as boxList, has } from "../lib/box";
import { uptimeText } from "../lib/status";

const { client, state, hello, paired, status, pair, api, cmd } = useSidecar();
const st = computed(() => status.value as Record<string, any> | null);

// the log: its pushes into lines; the stats lines feed the tiles
const buf = new LogBuffer(3000);
const lines = shallowRef(buf.lines);
const stats = shallowRef(buf.stats);
const follow = ref(true);
const hideStats = ref(true);
const wholeLog = ref(false);
const logEl = ref<HTMLElement | null>(null);
let off: (() => void) | null = null;
const toEnd = () => nextTick(() => logEl.value && (logEl.value.scrollTop = logEl.value.scrollHeight));
onMounted(() => {
  off = client.subscribe<LogPush>("log", (p) => {
    buf.add(p);
    lines.value = buf.lines;
    triggerRef(lines);
    stats.value = { ...buf.stats };
    if (follow.value && wholeLog.value) toEnd();
  });
});
onUnmounted(() => off?.());
const shown = computed(() => (hideStats.value ? lines.value.filter((l) => !STATS_LINE.test(l.text)) : lines.value));
const visible = computed(() => (wholeLog.value ? shown.value : shown.value.slice(-6)));
watch(wholeLog, (w) => w && follow.value && toEnd());
const isError = (t: string) => /ERROR|fail|fault|warn/i.test(t);
function clearLog() {
  buf.clear();
  lines.value = buf.lines;
}

// the model
const eng = computed(() => readEngine(status.value, stats.value));
const pills = computed(() => healthPills(status.value, eng.value));
const ob = computed(() => readOb(status.value, stats.value));
const usbc = computed(() => readUsbC(status.value, stats.value));
const power = computed(() => readPower(status.value));
const box = computed(() => boxName(status.value));
const rec = computed(() => st.value?.rec as Record<string, any> | undefined);
const cap = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const pct = (v: number | null) => (v === null ? "–" : `${Math.round(v)}`);

// the busiest render core over the last minute (one sample per engine line, once a second)
const busy = ref<number[]>([]);
watch(() => stats.value.engine, (e, was) => {
  if (e && e !== was && eng.value.busiest !== null) busy.value = pushSample(busy.value, eng.value.busiest);
});
const blockWarn = computed(() => eng.value.budget !== null && eng.value.blockMax !== null && eng.value.blockMax > eng.value.budget);

const fifo = computed(() => eng.value.fifo);
const fifoMax = computed(() => (fifo.value ? Math.max(fifo.value.target * 2, fifo.value.max) : 1));
const temp = computed(() => (typeof st.value?.temp === "number" ? (st.value.temp as number) : null));
const tempHot = computed(() => !!st.value?.tempWarn || (st.value?.clockLow ?? 0) > 0);
const boxOn = computed(() => !!(st.value?.streaming || st.value?.rytmMidi));
const web = computed(() => st.value?.web as Record<string, number> | undefined);

// the box's tracks (/status "src"): the machine each one reports; hidden until any has reported
const TRACK = ["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "T9", "T10", "T11", "T12"];
const src = computed(() => {
  const s = st.value?.src;
  if (!Array.isArray(s) || !s.some((x) => x && x.vb >= 0)) return null;
  return s.map((x, i) => ({ name: TRACK[i] ?? `T${i + 1}`, m: (x && x.m) || "-", ours: x && x.m && x.m !== "RYTM" && x.m !== "?" }));
});
const ROLE: Record<string, string> = { host: "in use", waiting: "waiting", listening: "listening", spare: "spare", other: "other" };

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

  <section class="health" aria-labelledby="health-h">
    <h2 id="health-h" class="visually-hidden">Health</h2>
    <ul v-if="pills.length" class="pills">
      <li v-for="p in pills" :key="p.key" class="hp" :class="{ warn: p.warn }">
        <span class="dot" aria-hidden="true">{{ p.warn ? "!" : "" }}</span>
        <span aria-hidden="true">{{ p.text }}</span>
        <span class="visually-hidden">{{ p.spoken }}{{ p.warn ? " (needs a look)" : "" }}</span>
      </li>
    </ul>
    <p class="meta">
      <template v-if="status">{{ status.host }}.local · up <span class="tnum">{{ uptimeText(status.uptime) }}</span></template>
      <template v-if="hello"> · Sidecar {{ hello.version }}</template>
    </p>
  </section>

  <Card label="Engine">
    <div class="grid engine">
      <StatTile id="cpu" class="cpu" label="CPU" :value="pct(eng.busiest)" unit="% busiest" :warn="blockWarn || (eng.busiest ?? 0) >= 90"
        :detail="eng.mhz ? `${eng.mhz} MHz` : ''" :more="eng.passMax !== null ? [`pass max ${eng.passMax} µs`, 'core 0 (system) from the 10 s idle line'] : []">
        <div class="cpu-body">
        <ul class="cores">
          <li v-for="(c, i) in eng.cores" :key="i">
            <span class="cl">Core {{ i }}<span v-if="i === 0" class="visually-hidden"> (system)</span></span>
            <Gauge :label="`Core ${i}${i === 0 ? ' (system)' : ''}`" :value="c" :max="100" :text="c === null ? '' : `${Math.round(c)} percent`" :warn="(c ?? 0) >= 90" />
            <span class="cv tnum">{{ c === null ? "–" : `${Math.round(c)} %` }}</span>
          </li>
        </ul>
        <div class="cpu-side">
        <div class="spark-row">
          <span class="cl">Busiest core, last minute</span>
          <Spark :values="busy" label="Busiest core" />
        </div>
        <div v-if="eng.blockMax !== null" class="pair">
          <span class="cl">Block max</span>
          <Gauge label="Longest block against the voice budget" :value="eng.blockMax" :max="Math.max(eng.budget ?? 0, eng.blockMax) * 1.25"
            :text="`${eng.blockMax} microseconds`" :mark="eng.budget" mark-label="budget" :warn="blockWarn" />
          <span class="cv tnum">{{ eng.blockMax }}<template v-if="eng.budget !== null"> / {{ eng.budget }}</template> µs</span>
        </div>
        </div>
        </div>
      </StatTile>

      <StatTile id="fifo" class="fifo" label="FIFO" :value="fifo ? String(fifo.min) : '–'" unit="frames lowest" :warn="!!fifo && fifo.min === 0"
        :detail="fifo ? `${fifo.min}–${fifo.max} this second · target ${fifo.target}` : 'waiting for the engine line'">
        <Gauge v-if="fifo" label="FIFO fill, lowest to highest" :value="fifo.min" :range="[fifo.min, fifo.max]" :max="fifoMax"
          :text="`${fifo.min} to ${fifo.max} frames`" :mark="fifo.target" mark-label="target" :warn="fifo.min === 0" />
      </StatTile>

      <StatTile v-if="st?.underruns !== undefined" id="under" class="under" label="Underruns" :value="String(st.underruns)" :warn="st.underruns > 0"
        :detail="`while playing · ${st.restartUnderruns ?? 0} at stream starts`"
        :more="[`${st.streams ?? 0} stream start${st.streams === 1 ? '' : 's'} since boot`, 'stream starts refill the FIFO: not a glitch']" />
    </div>
  </Card>

  <Card label="Connections">
    <div class="grid">
      <StatTile v-if="ob" id="box" :label="box" :detail="ob.streaming ? `Overbridge streaming · link ${ob.link}` : `no Overbridge stream · link ${ob.link}`"
        :warn="ob.bad" :more="ob.rate !== null ? [`IN ${ob.rate}/s`, `gap ${ob.gap}`, `lost ${ob.lost}`] : []">
        <template #big><span class="word" :class="{ dim: !boxOn }">{{ boxOn ? "Connected" : "Not seen" }}</span></template>
        <p class="tags">
          <span class="tag" :class="{ on: st?.streaming }">{{ st?.streaming ? "✓" : "–" }} Overbridge</span>
          <span class="tag" :class="{ on: st?.rytmMidi }">{{ st?.rytmMidi ? "✓" : "–" }} USB MIDI</span>
        </p>
      </StatTile>

      <StatTile v-if="usbc" id="usbc" label="USB-C" :detail="usbc.detail" :more="usbc.more">
        <template #big><span class="word" :class="{ dim: usbc.mode === 'Off' }">{{ usbc.mode }}</span></template>
      </StatTile>

      <StatTile v-if="rec" id="rec" label="Recorder" :value="rec.state === 2 ? hms(rec.frames / 48000) : null"
        :detail="rec.state === 2 ? 'recording' : rec.take ? `last take ${rec.take}` : 'no take yet'">
        <template v-if="rec.state !== 2" #big><span class="word">{{ cap(REC_STATES[rec.state] ?? "–") }}</span></template>
        <router-link to="/rec" class="go">Rec page</router-link>
      </StatTile>
    </div>
    <ul v-if="boxList(status).length > 1" class="boxes" aria-label="Boxes on USB">
      <li v-for="(b, i) in boxList(status)" :key="b.key ?? `${b.id}-${i}`" :class="{ on: !!b.on }">
        <b>{{ shortBoxName(b.name) || "Box" }}</b> <span class="small">· {{ ROLE[b.role] ?? b.role }}</span>
      </li>
    </ul>
  </Card>

  <Card label="System">
    <div class="grid">
      <StatTile v-if="temp !== null" id="temp" label="Temperature" :value="String(temp)" unit="°C" :warn="tempHot"
        :detail="(st?.clockLow ?? 0) > 0 ? `clock lowered to ${st?.clockLow} MHz` : `SoC · warning at ${TEMP.warn} °C`">
        <Gauge label="SoC temperature" :value="temp" :min="TEMP.min" :max="TEMP.max" :text="`${temp} degrees`"
          :zone="TEMP.throttle" :mark="TEMP.warn" mark-label="warning at" :warn="tempHot" />
      </StatTile>

      <StatTile v-if="power" id="power" label="Power" :value="power.amps === null ? null : String(power.amps)" unit="A" :warn="power.chip === 'too weak'" :detail="power.uv" :more="power.more">
        <p class="tags"><span class="tag" :class="{ on: power.chip === 'ok', bad: power.chip === 'too weak' }">{{ power.chip === "ok" ? "✓ OK" : power.chip === "too weak" ? "⚠ Too weak" : "? Unknown" }}</span></p>
      </StatTile>

      <StatTile v-if="web" id="web" label="This page" :value="String(web.requests)" unit="requests"
        :detail="`slowest ${web.maxus} µs · ${web.commands} commands`" :more="web.refused ? [`${web.refused} refused (busy)`] : []" />
    </div>
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
      <Chip class="sm" :pressed="follow" @click="follow = !follow">Follow</Chip>
      <Chip class="sm" :pressed="hideStats" @click="hideStats = !hideStats">Hide the once-a-second lines</Chip>
      <button type="button" class="sm" @click="clearLog">Clear</button>
    </div>
    <div id="log" ref="logEl" class="log" :class="{ whole: wholeLog }" role="log" aria-label="Sidecar log" tabindex="0">
      <div v-for="l in visible" :key="l.n" :class="{ err: isError(l.text), gap: l.gap }">
        <span v-if="isError(l.text)" class="visually-hidden">Warning: </span>{{ l.text }}
      </div>
      <p v-if="!shown.length" class="small">{{ lines.length ? "Only once-a-second lines so far (hidden)." : "Waiting for the log…" }}</p>
    </div>
    <button v-if="shown.length > 6" type="button" class="sm toggle" aria-controls="log" :aria-expanded="wholeLog" @click="wholeLog = !wholeLog">
      {{ wholeLog ? "Show the last lines only" : `Show the whole log (${shown.length} lines)` }}
    </button>
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
.tnum { font-variant-numeric: tabular-nums; }
.health { display: flex; flex-direction: column; gap: 0.5rem; margin: 0.75rem 0 0.25rem; }
.pills { list-style: none; margin: 0; padding: 0; display: flex; flex-wrap: wrap; gap: 0.5rem; }
.hp {
  display: inline-flex; align-items: center; gap: 0.5rem; padding: 0.375rem 0.875rem 0.375rem 0.625rem;
  border: 1px solid var(--control-border); border-radius: var(--radius-pill); background: var(--card);
  font-weight: 500; font-variant-numeric: tabular-nums; white-space: nowrap;
}
.dot { width: 0.625rem; height: 0.625rem; border-radius: 50%; background: var(--accent); flex: none; }
.hp.warn { border-color: var(--warm); background: var(--warm-tint); color: var(--warm); font-weight: 700; }
.hp.warn .dot {
  width: 1rem; height: 1rem; background: var(--warm); color: var(--on-accent);
  display: inline-flex; align-items: center; justify-content: center; font-size: 0.6875rem; font-weight: 900; line-height: 1;
}
.meta { margin: 0; color: var(--label); font-size: var(--text-min); }

.grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 0.75rem; }
/* Engine: the CPU tile wide, FIFO and Underruns stacked beside it */
.engine { grid-template-areas: "cpu cpu fifo" "cpu cpu under"; }
.engine .cpu { grid-area: cpu; }
.engine .fifo { grid-area: fifo; }
.engine .under { grid-area: under; }
.cpu-body { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 0.5rem 1.5rem; align-items: start; }
.cpu-side { display: flex; flex-direction: column; gap: 0.75rem; }
.spark-row { display: flex; flex-direction: column; gap: 0.25rem; }
@media (max-width: 64rem) {
  .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .grid > :last-child:nth-child(odd) { grid-column: 1 / -1; }
  .engine { grid-template-areas: "cpu cpu" "fifo under"; }
  .engine > :last-child:nth-child(odd) { grid-column: auto; }
}
@media (max-width: 40rem) {
  .grid { grid-template-columns: minmax(0, 1fr); }
  .engine { grid-template-areas: "cpu" "fifo" "under"; }
  .cpu-body { grid-template-columns: minmax(0, 1fr); }
}

.cores { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.375rem; }
.cores li, .pair { display: grid; grid-template-columns: 4.5rem minmax(0, 1fr) 4.25rem; align-items: center; gap: 0.625rem; }
.pair { grid-template-columns: 4.5rem minmax(0, 1fr) auto; }
.cl { color: var(--label); font-size: var(--text-min); white-space: nowrap; }
.cv { font-size: var(--text-min); text-align: right; white-space: nowrap; }

.word { font-size: 1.375rem; font-weight: 600; line-height: 1.1; }
.word.dim { color: var(--label); }
.tags { display: flex; flex-wrap: wrap; gap: 0.375rem; margin: 0; }
.tag {
  display: inline-flex; align-items: center; gap: 0.25rem; padding: 0.125rem 0.625rem; border-radius: var(--radius-pill);
  border: 1px solid var(--border); color: var(--label); font-size: var(--text-min);
}
.tag.on { border-color: var(--accent); color: var(--accent); }
.tag.bad { border-color: var(--warm); color: var(--warm); background: var(--warm-tint); font-weight: 700; }
.go { color: var(--accent); align-self: flex-start; display: inline-flex; align-items: center; min-height: var(--target); }

.boxes { list-style: none; margin: 0; padding: 0; display: flex; flex-wrap: wrap; gap: 0.5rem; }
.boxes li { border: 1px solid var(--border); border-radius: var(--radius-pill); padding: 0.25rem 0.75rem; }
.boxes li.on { border: 2px solid var(--accent); }
.src { list-style: none; margin: 0; padding: 0; display: grid; grid-template-columns: repeat(auto-fill, minmax(6rem, 1fr)); gap: 0.5rem; }
.src li { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem; display: flex; flex-direction: column; }
.src li.ours { border-color: var(--accent); }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.grow { flex: 1 1 12rem; }
.sm { border-radius: var(--radius-pill); padding: 0.25rem 0.875rem; font-size: var(--text-min); }
.toggle { align-self: flex-start; color: var(--accent); }
.log {
  overflow: auto; font-family: ui-monospace, Menlo, monospace; font-size: var(--text-min); line-height: 1.45;
  border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem 0.625rem; white-space: pre-wrap; overflow-wrap: anywhere;
  background: var(--bg); max-height: 10rem;
}
.log.whole { height: 22rem; max-height: none; }
.log .err { color: var(--warm); }
.log .gap { color: var(--label); font-style: italic; }
</style>
