<!-- web-next/src/pages/Morph.vue - the morph scenes (0.5.0, core/mod/scene.h; the touchscreen's MORPH view): twelve
     scene pads, A and B on the ends of the fader, the fader itself, and the selected scene's locks (a value each, add,
     remove), copy and clear. Each edit is a console line (scene=...); the state comes back on /status "scene". A build
     without scenes (0.4.x) says so. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../ui/Card.vue";
import Fader from "../ui/Fader.vue";
import { RequestError } from "../lib/client";
import { coalescer } from "../lib/coalesce";
import { confirmAsk } from "../lib/confirm";
import { useSidecar } from "../lib/useSidecar";
import {
  FADER_MAX, LOCKS, SCENES, SOURCES, SUGGEST, abLine, clearLine, copyLine, editLine, faderLine, lockLine, morphed, sceneView, selLine,
  targetName, unlockLine, valueText,
} from "../lib/scene";

const { status, cmd, paired } = useSidecar();
const sc = computed(() => sceneView(status.value as Record<string, unknown> | null));
const msg = ref("");
const fail = (e: unknown) => (msg.value = e instanceof RequestError ? `Refused (${e.code}): ${e.message}` : (e as Error).message);
async function send(line: string) {
  msg.value = "";
  try {
    await cmd(line);
  } catch (e) {
    fail(e);
  }
}
const live = coalescer(cmd, fail); // the fader and the lock values while they move: the newest wins

// the fader: its own value while it moves, the Sidecar's otherwise (the status is ~1 s behind)
const fader = ref(0);
let moving = false;
watch(() => sc.value?.fader, (f) => { if (!moving && f !== undefined) fader.value = f; }, { immediate: true });
function faderInput(v: number) {
  moving = true;
  fader.value = v;
  void live(faderLine(v));
}
function faderCommit(v: number) {
  moving = false;
  void live(faderLine(v));
}
const pct = (f: number) => `${Math.round((f / FADER_MAX) * 100)} %`;

// the pads
const sel = computed(() => sc.value?.sel ?? 0);
const pads = computed(() => Array.from({ length: SCENES }, (_, i) => ({ n: i, locks: sc.value?.locks[i].length ?? 0, a: sc.value?.a === i, b: sc.value?.b === i })));
const setA = () => sc.value && send(abLine(sel.value, sc.value.b === sel.value ? -1 : sc.value.b));
const setB = () => sc.value && send(abLine(sc.value.a === sel.value ? -1 : sc.value.a, sel.value));
const unset = (side: "a" | "b") => sc.value && send(abLine(side === "a" ? -1 : sc.value.a, side === "b" ? -1 : sc.value.b));

// the selected scene's locks: each one's value as a fader (0..127.99, stored x 128)
const locks = computed(() => sc.value?.locks[sel.value] ?? []);
// the same target's value in the other end's scene, so what the morph plays now can be shown
const other = computed(() => {
  const s = sc.value;
  if (!s) return {} as Record<string, number>;
  const o = s.a === sel.value ? s.b : s.b === sel.value ? s.a : -1;
  return Object.fromEntries((o >= 0 ? s.locks[o] : []).map((l) => [l.name, l.v])) as Record<string, number>;
});
function playing(name: string, v: number): string | null {
  const s = sc.value;
  if (!s || (s.a !== sel.value && s.b !== sel.value)) return null;
  const ov = other.value[name];
  const [a, b] = s.a === sel.value ? [v, ov] : [ov, v];
  const m = morphed(a, b, fader.value);
  return m === null ? null : valueText(m);
}
const setLock = (name: string, v: number) => void live(lockLine(sel.value, name, v * 128));
const addName = ref("");
const addValue = ref(64);
const addOk = computed(() => !!targetName(addName.value));
async function add() {
  const n = targetName(addName.value);
  if (!n) return;
  await send(lockLine(sel.value, n, addValue.value * 128));
  if (!msg.value) addName.value = "";
}
const copyTo = ref(1);
async function copy() {
  const to = copyTo.value, n = sc.value?.locks[to].length ?? 0;
  if (to === sel.value) return;
  if (n > 0 && !(await confirmAsk({ title: `Copy over scene ${to + 1}?`, body: `Scene ${to + 1}'s ${n} locks are replaced by scene ${sel.value + 1}'s.`, yes: "Copy over it", danger: true }))) return;
  await send(copyLine(sel.value, to));
}
async function clear() {
  if (!(await confirmAsk({ title: `Clear scene ${sel.value + 1}?`, body: `Its ${locks.value.length} locks go.`, yes: "Clear", danger: true }))) return;
  await send(clearLine(sel.value));
}
const source = computed(() => SOURCES[sc.value?.src ?? 0] ?? "");
</script>

<template>
  <h1 class="title" tabindex="-1">Morph</h1>
  <Card v-if="!sc" label="Scenes">
    <p>Morph scenes come with Sidecar 0.5.0. This Sidecar does not report them.</p>
  </Card>
  <template v-else>
    <Card label="Morph">
      <div class="ends">
        <span class="end"><span class="label">A</span> <b class="num">{{ sc.a >= 0 ? sc.a + 1 : "–" }}</b></span>
        <div class="fader">
          <Fader
            label="Morph fader, A to B"
            hide-label
            :model-value="fader"
            :min="0"
            :max="FADER_MAX"
            :disabled="!paired || sc.a < 0 || sc.b < 0"
            :format="pct"
            @update:model-value="faderInput"
            @commit="faderCommit"
          />
        </div>
        <span class="end"><span class="label">B</span> <b class="num">{{ sc.b >= 0 ? sc.b + 1 : "–" }}</b></span>
      </div>
      <p class="small">
        <template v-if="sc.a < 0 || sc.b < 0">Put a scene on A and one on B to morph between them.</template>
        <template v-else>Played <span class="num">{{ pct(sc.played) }}</span><template v-if="source"> · last moved by {{ source }}</template>.</template>
        <template v-if="sc.rscn > 0"> The Rytm's own scene: <span class="num">{{ sc.rscn }}</span>.</template>
      </p>
      <p v-if="sc.locked" class="small warm">This kit's scene file has lines this build cannot read: changes are not saved this session.</p>
    </Card>

    <Card label="Scenes">
      <div class="pads" role="radiogroup" aria-label="Scene">
        <button
          v-for="p in pads"
          :key="p.n"
          type="button"
          role="radio"
          class="pad"
          :class="{ sel: p.n === sel, ab: p.a || p.b }"
          :aria-checked="p.n === sel"
          :aria-label="`Scene ${p.n + 1}: ${p.locks} locks${p.a ? ', on A' : ''}${p.b ? ', on B' : ''}`"
          :disabled="!paired"
          @click="send(selLine(p.n))"
        >
          <span class="num pn">{{ p.n + 1 }}</span>
          <span class="small">{{ p.locks ? `${p.locks} lock${p.locks > 1 ? "s" : ""}` : "empty" }}</span>
          <span v-if="p.a || p.b" class="badge" aria-hidden="true">{{ p.a ? "A" : "" }}{{ p.b ? "B" : "" }}</span>
        </button>
      </div>
      <div class="row">
        <button type="button" :disabled="!paired" @click="sc.a === sel ? unset('a') : setA()">{{ sc.a === sel ? `Take ${sel + 1} off A` : `Scene ${sel + 1} on A` }}</button>
        <button type="button" :disabled="!paired" @click="sc.b === sel ? unset('b') : setB()">{{ sc.b === sel ? `Take ${sel + 1} off B` : `Scene ${sel + 1} on B` }}</button>
        <label class="check"><input type="checkbox" :checked="sc.edit" :disabled="!paired" @change="send(editLine(($event.target as HTMLInputElement).checked))" /> EDIT: knob turns on the Sidecar's pages lock into the held scene</label>
      </div>
    </Card>

    <Card :label="`Scene ${sel + 1} · ${locks.length} of ${LOCKS} locks`">
      <p v-if="!locks.length">No locks yet. Add one below, or hold the scene's key on the Rytm with EDIT on and turn a knob.</p>
      <ul v-else class="locks">
        <li v-for="l in locks" :key="l.name">
          <Fader
            :label="l.name"
            :model-value="+(l.v / 128).toFixed(2)"
            :min="0"
            :max="127.99"
            :step="0.01"
            :disabled="!paired"
            @update:model-value="(v: number) => setLock(l.name, v)"
            @commit="(v: number) => setLock(l.name, v)"
          />
          <span class="small plays">{{ playing(l.name, l.v) !== null ? `plays ${playing(l.name, l.v)}` : "" }}</span>
          <button type="button" :aria-label="`Remove ${l.name}`" :disabled="!paired" @click="send(unlockLine(sel, l.name))">Remove</button>
        </li>
      </ul>
      <form class="row" @submit.prevent="add">
        <label for="tgt" class="label">Target</label>
        <input id="tgt" v-model="addName" list="targets" autocomplete="off" spellcheck="false" placeholder="V1:P3" class="tgt" :disabled="!paired || locks.length >= LOCKS" />
        <datalist id="targets"><option v-for="t in SUGGEST" :key="t" :value="t" /></datalist>
        <div class="val"><Fader v-model="addValue" label="Value" :min="0" :max="127" :disabled="!paired" /></div>
        <button type="submit" :disabled="!paired || !addOk || locks.length >= LOCKS">Add the lock</button>
      </form>
      <p class="small">A target is a name like V3:P5 (voice 3's P5), O4:PAN, MFX:LEV, FX1:P2 or RYTM:PERF3.</p>
      <div class="row">
        <label for="copyto" class="label">Copy to</label>
        <select id="copyto" v-model.number="copyTo" :disabled="!paired">
          <option v-for="n in SCENES" :key="n" :value="n - 1" :disabled="n - 1 === sel">Scene {{ n }}</option>
        </select>
        <button type="button" :disabled="!paired || !locks.length || copyTo === sel" @click="copy">Copy</button>
        <button type="button" class="danger" :disabled="!paired || !locks.length" @click="clear">Clear scene {{ sel + 1 }}</button>
      </div>
    </Card>
    <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
  </template>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.ends { display: flex; align-items: center; gap: 1rem; }
.end { display: flex; flex-direction: column; align-items: center; min-width: 3rem; }
.end b { font-size: 2rem; color: var(--accent); }
.fader { flex: 1; min-width: 0; }
.pads { display: grid; grid-template-columns: repeat(auto-fill, minmax(5.5rem, 1fr)); gap: 0.5rem; }
.pad { position: relative; display: flex; flex-direction: column; align-items: center; gap: 0.125rem; padding: 0.625rem 0.25rem; min-height: 4.5rem; }
.pad .pn { font-size: 1.5rem; }
.pad.sel { border: 2px solid var(--accent); background: var(--accent-tint); }
.pad.ab { box-shadow: inset 0 -3px 0 var(--accent); }
.badge { position: absolute; top: 0.25rem; right: 0.375rem; font-weight: 700; color: var(--accent); font-size: var(--text-min); }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; margin-top: 0.75rem; }
.check { display: inline-flex; align-items: center; gap: 0.375rem; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
.locks { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.5rem; }
.locks li { display: grid; grid-template-columns: 1fr 7rem auto; align-items: center; gap: 0.75rem; border-bottom: 1px solid var(--border); padding-bottom: 0.5rem; }
.tgt { width: 9rem; text-transform: uppercase; }
.val { flex: 1 1 12rem; }
.danger { border-color: var(--warm); color: var(--warm); }
@media (max-width: 40rem) {
  .locks li { grid-template-columns: 1fr auto; }
  .locks li > :first-child { grid-column: 1 / -1; }
}
</style>
