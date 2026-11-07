<!-- web-next/src/pages/Mix.vue - the mixer as the approved sketch: 12 strips (V1..V8 voices, B1..B3 buses, M master)
     with their POST EQ curves, inserts, meters (the "levels" topic) and levels; the selected strip's detail below; the
     master FX, the send FX, and the sidechain's place (0.5.0). -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../ui/Card.vue";
import Fader from "../ui/Fader.vue";
import Segmented from "../ui/Segmented.vue";
import StripView from "./mix/StripView.vue";
import StripDetail from "./mix/StripDetail.vue";
import { RequestError } from "../lib/client";
import { mixCmd, strips } from "../lib/mix";
import { useSidecar } from "../lib/useSidecar";
import { coalescer } from "../lib/coalesce";
import { dbText } from "../lib/status";
import { has } from "../lib/box";

const { status, levels, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? null) as Record<string, any> | null);
const all = computed(() => strips(st.value, levels.value?.strips));
const sel = ref(0);
const selected = computed(() => all.value[sel.value]);
const mfx = computed(() => st.value?.ins?.mfx as Record<string, any> | undefined);
const fx = computed(() => (st.value?.fx ?? []) as Record<string, any>[]);
const hasIns = computed(() => !!st.value?.ins);
const master = computed(() => (levels.value ? Math.max(...levels.value.master) : null));
const msg = ref("");
// dials and faders: the newest value of each control wins while one is on its way (lib/coalesce.ts)
const latest = coalescer(cmd, (e) => (msg.value = e instanceof RequestError ? `Refused (${e.code}): ${e.message}` : (e as Error).message));
function send(c: string) {
  msg.value = "";
  void latest(c);
}
// a strip's fader: a voice's OUT level, the master's MFX level, none for a bus
const faderOf = (i: number) => (i < 8 ? all.value[i].out?.lev ?? null : i === 11 ? mfx.value?.lev ?? null : null);
function setLevel(i: number, v: number) {
  send(i < 8 ? mixCmd.out(all.value[i], "lev", v) : mixCmd.mfxLevel(v));
}
</script>

<template>
  <h1 class="title" tabindex="-1">Mix</h1>
  <p v-if="st && !hasIns" class="card">This Sidecar's build reports no mixer (no "ins" in its status).</p>
  <Card label="Mixer">
    <p class="small">Master <span class="num accent">{{ master === null ? "–" : dbText(master) }}</span> · a strip opens its EQ, inserts and sends below.</p>
    <div class="scroller">
      <div class="strips" role="group" aria-label="Strips">
        <StripView
          v-for="(s, i) in all"
          :key="s.id"
          :strip="s"
          :selected="sel === i"
          :level="faderOf(i)"
          :disabled="!paired"
          @select="sel = i"
          @level="(v: number) => setLevel(i, v)"
        />
      </div>
    </div>
    <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
  </Card>

  <div class="grid">
    <StripDetail v-if="selected" :strip="selected" :disabled="!paired" @cmd="send" />

    <div class="col">
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

      <Card v-if="has(status, 'sendfx')" label="Send FX">
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

      <Card label="Sidechain" tone="warm">
        <p>Pump and duck from a Rytm track or the audio in come with Sidecar 0.5.0.</p>
      </Card>
    </div>
  </div>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.scroller { overflow-x: auto; padding-bottom: 0.25rem; }
.strips { display: grid; grid-template-columns: repeat(12, minmax(4.5rem, 1fr)); gap: 0.5rem; min-width: 56rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 24rem), 1fr)); gap: 0 1.25rem; align-items: start; }
.col { min-width: 0; }
.row { display: flex; flex-wrap: wrap; gap: 0.5rem; }
.fxl { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.75rem; }
.sends { display: grid; grid-template-columns: repeat(12, 1fr); gap: 0.25rem; height: 3rem; margin-top: 0.375rem; }
.sends span { display: flex; flex-direction: column; justify-content: flex-end; align-items: center; height: 100%; }
.sends i { display: block; width: 100%; background: var(--accent); min-height: 1px; }
.sends small { font-size: var(--text-min); color: var(--label); }
</style>
