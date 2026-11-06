<!-- Library › the project's POOL (core/lib/pools.h; the old page's Pool tab): SAMPLES / WAVETABLES / INSTRUMENTS,
     banks A..H, the RAM it takes, each row's file and state; accept a changed file, replace, unload (rows in use ask
     first), select the unused ones, undo. The answers come back in the log (lib/logwait.ts). -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import { confirmAsk } from "../../lib/confirm";
import { hb } from "../../lib/fs";
import { useLogWait } from "../../lib/logwait";
import { BANKS, POOLS, STATE_TONE, answerText, inUse, inUseQuestion, poolAnswer, poolRow, rowName, rowText, unloadLines, type PoolRow } from "../../lib/pool";
import { useSidecar } from "../../lib/useSidecar";

const { api, cmd, paired, client } = useSidecar();
const waitFor = useLogWait(client);
const k = ref("smp");
const bank = ref(0);
try {
  const s = localStorage.getItem("sidecar.pool");
  if (s && POOLS[s]) k.value = s;
  bank.value = +(localStorage.getItem("sidecar.poolbank") || 0) & 7;
} catch { /* storage blocked */ }
const data = ref<Record<string, any> | null>(null);
const msg = ref("");
const bad = ref(false);
const sel = ref(new Set<string>());
let timer: ReturnType<typeof setInterval> | null = null;
async function load() {
  if (!paired.value) return;
  try {
    data.value = await api.json(`/pool?l=${k.value}&b=${bank.value}`);
  } catch (e) {
    say((e as Error).message, true);
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 2000);
});
onUnmounted(() => timer && clearInterval(timer));
watch([k, bank], ([kk, b]) => {
  sel.value = new Set();
  try { localStorage.setItem("sidecar.pool", kk); localStorage.setItem("sidecar.poolbank", String(b)); } catch { /* not kept */ }
  cmd(`pool=${kk}:shown:${BANKS[b]}`).catch(() => undefined); // the sampler's takes go to the bank shown
  load();
});
function say(t: string, isBad = false) { msg.value = t; bad.value = isBad; }

const info = computed(() => data.value?.st?.[k.value] as Record<string, any> | undefined);
const locked = computed(() => !!info.value?.locked || !!info.value?.ro || !!data.value?.bank?.locked);
const ram = computed(() => data.value?.ram as Record<string, number> | undefined);
const rows = computed<PoolRow[]>(() => (data.value?.bank?.rows ?? []).map(poolRow));
const name = (e: PoolRow) => rowName(bank.value, e.row);
const isFile = (e: PoolRow) => e.type === "sample" || e.type === "instrument";

async function send(c: string): Promise<ReturnType<typeof poolAnswer>> {
  const w = waitFor((l) => poolAnswer(l, k.value));
  try {
    await cmd(c);
  } catch (e) {
    say((e as Error).message, true);
    return null;
  }
  return w;
}
async function accept(e: PoolRow) {
  const [t, b] = answerText("accept", name(e), await send(`pool=${k.value}:accept:${name(e)}`));
  say(t, b);
  load();
}
const replacing = ref<{ row: string; path: string } | null>(null);
async function replace(e: PoolRow) {
  const r = replacing.value;
  if (!r || !r.path) return;
  if (r.path.length > 159) return say("The path is too long for the POOL (159 characters).", true);
  replacing.value = null;
  const yes = inUse(e);
  if (yes && !(await confirmAsk({ title: "Replace", body: inUseQuestion(r.row, e, "REPLACE"), yes: "Replace" }))) return say(`${r.row}: not changed`);
  const [t, b] = answerText("replace", r.row, await send(`pool=${k.value}:replace:${r.row}:${r.path}${yes ? ":yes" : ""}`), r.path.split("/").pop());
  say(t, b);
  load();
}
async function unload(list: PoolRow[]) {
  if (!list.length) return;
  const used = list.filter(inUse);
  if (used.length && !(await confirmAsk({ title: "Unload", body: used.length === 1 ? inUseQuestion(name(used[0]), used[0], "UNLOAD") : `${used.map(name).join(", ")} are in use. Unload them?`, yes: "Unload" }))) return;
  let n = 0, last: ReturnType<typeof poolAnswer> = null;
  for (const l of unloadLines(k.value, list.map(name), new Set(used.map(name)))) {
    last = await send(l);
    if (last && last.n > 0) n += last.n;
  }
  say(...(n ? ([`${n} unloaded (Undo takes it back)`, false] as [string, boolean]) : answerText("unload", list.map(name).join(", "), last)));
  sel.value = new Set();
  load();
}
async function selectUnused() {
  try {
    const j = await api.json<{ unused?: number[]; warn?: string }>(`/pool?l=${k.value}&b=${bank.value}&u=1`);
    sel.value = new Set((j.unused ?? []).map(rowText).filter((r) => r[0] === BANKS[bank.value]));
    say(`${sel.value.size} unused in bank ${BANKS[bank.value]}${j.warn ? ` · ${j.warn}` : ""}`);
  } catch (e) {
    say((e as Error).message, true);
  }
}
async function undo() {
  const a = await send(`pool=${k.value}:undo`);
  say(a ? a.text : "sent, no answer yet", !a);
  load();
}
function toggle(r: string) {
  const s = new Set(sel.value);
  s.has(r) ? s.delete(r) : s.add(r);
  sel.value = s;
}
</script>

<template>
  <Card label="Pool">
    <p v-if="!paired">The POOL needs the pairing.</p>
    <template v-else>
      <div class="row">
        <Segmented label="List" :options="Object.entries(POOLS).map(([v, t]) => ({ value: v, text: t }))" :model-value="k" @update:model-value="(v: string) => (k = v)" />
        <Segmented label="Bank" :options="[...BANKS].map((b, i) => ({ value: i, text: b }))" :model-value="bank" @update:model-value="(v: number) => (bank = v)" />
      </div>
      <p class="small">
        <template v-if="data?.st?.project">Project {{ data.st.project }} · </template>
        <template v-if="ram?.budget">RAM <span class="num">{{ hb(ram.used + ram.tables) }}</span> of {{ hb(ram.budget) }} ({{ ram.pct }} %)</template>
      </p>
      <p v-if="locked" class="warm" role="status"><span aria-hidden="true">⚠ </span>{{ info?.why || "This POOL is locked: no changes now." }}</p>
      <div class="scroller">
        <table>
          <thead><tr><th scope="col"><span class="visually-hidden">Select</span></th><th scope="col">Row</th><th scope="col">File</th><th scope="col">State</th><th scope="col">Size</th><th scope="col">Played</th><th scope="col">Sounds</th><th scope="col"><span class="visually-hidden">Actions</span></th></tr></thead>
          <tbody>
            <tr class="off"><td></td><td class="num">{{ BANKS[bank] }}000</td><td colspan="6">OFF</td></tr>
            <tr v-for="e in rows" :key="e.row">
              <td><input type="checkbox" :checked="sel.has(name(e))" :disabled="locked" :aria-label="`Select ${name(e)}`" @change="toggle(name(e))" /></td>
              <td class="num">{{ name(e) }}</td>
              <td :title="e.path">{{ e.name || "–" }}<span v-if="e.state === 'MISSING' && e.path" class="small">{{ ` · was ${e.path}` }}</span></td>
              <td><span class="badge" :class="STATE_TONE[e.state]">{{ e.state }}</span></td>
              <td>{{ isFile(e) && e.size ? hb(e.size) : isFile(e) ? "–" : "" }}{{ e.type === "instrument" ? ` · ${e.members} samples` : "" }}{{ e.ch ? (e.ch === 2 ? " · ST" : " · M") : "" }}{{ e.ram ? " · RAM" : "" }}</td>
              <td>{{ e.used || "" }}</td>
              <td>{{ e.users || "" }}</td>
              <td class="acts">
                <template v-if="replacing && replacing.row === name(e)">
                  <label :for="`rp-${e.row}`" class="visually-hidden">Path on the card</label>
                  <input :id="`rp-${e.row}`" v-model="replacing.path" placeholder="/Samples/…" autocomplete="off" @keydown.enter.prevent="replace(e)" />
                  <button type="button" @click="replace(e)">Replace</button><button type="button" @click="replacing = null">Cancel</button>
                </template>
                <template v-else>
                  <button v-if="e.state === 'CHANGED'" type="button" :disabled="locked" @click="accept(e)">Accept</button>
                  <button v-if="isFile(e)" type="button" :disabled="locked" @click="replacing = { row: name(e), path: e.path ? `/Samples/${e.path}` : '' }">Replace…</button>
                  <button type="button" :disabled="locked" @click="unload([e])">Unload</button>
                </template>
              </td>
            </tr>
            <tr v-if="!rows.length"><td colspan="8" class="small">Bank {{ BANKS[bank] }} is empty: add files with +SMP / +WT / +INST in Files below.</td></tr>
          </tbody>
        </table>
      </div>
      <div class="row">
        <button type="button" :disabled="locked" @click="selectUnused">Select unused</button>
        <button type="button" :disabled="locked || !sel.size" @click="unload(rows.filter((e) => sel.has(name(e))))">Unload selected ({{ sel.size }})</button>
        <button type="button" :disabled="locked" @click="undo">Undo</button>
      </div>
      <p v-if="msg" role="status" class="small" :class="{ warm: bad }">{{ msg }}</p>
    </template>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.scroller { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-size: 0.9375rem; }
th, td { text-align: left; padding: 0.375rem 0.5rem; border-bottom: 1px solid var(--border); vertical-align: middle; }
th { font-size: var(--label-size); color: var(--label); text-transform: uppercase; letter-spacing: var(--label-tracking); font-weight: 500; }
.off td { color: var(--label); }
td input[type="checkbox"] { min-width: 1.25rem; min-height: 1.25rem; }
.acts { display: flex; flex-wrap: wrap; gap: 0.25rem; }
.badge { font-size: var(--text-min); border: 1px solid var(--control-border); border-radius: var(--radius-pill); padding: 0.125rem 0.5rem; }
.badge.ok { border-color: var(--accent); color: var(--accent); }
.badge.bad, .badge.warn { border-color: var(--warm); color: var(--warm); border-width: 2px; }
</style>
