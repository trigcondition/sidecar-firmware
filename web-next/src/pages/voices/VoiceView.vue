<!-- Voices › the selected track as the touchscreen's VOICE page (the "voices" topic, pi/engine/voiceview_json.h): its
     machine, P1..P8 with the Rytm's labels, and the machine's own panel (6-OP FM's algorithm, DAISYSP's modes,
     MULTISAMPLE's zones). The knobs show; they are turned on the Rytm. The track: the Rytm's selected one, or one picked
     here (voicepick=, shared with the touchscreen). -->
<script setup lang="ts">
import { computed, onUnmounted, ref, shallowRef, watch } from "vue";
import Card from "../../ui/Card.vue";
import Knob from "../../ui/Knob.vue";
import { RequestError } from "../../lib/client";
import { useSidecar } from "../../lib/useSidecar";
import { TRACKS, daisyLines, daisyModel, fmLayout, params, pickLine, trackName, zoneBars, type VoiceView } from "../../lib/voice";

const { client, hello, cmd, paired } = useSidecar();
const view = shallowRef<VoiceView | null>(null);
const served = computed(() => !!hello.value?.topics.includes("voices"));
let off: (() => void) | null = null;
watch(served, (on) => {
  if (on && !off) off = client.subscribe<VoiceView>("voices", (v) => (view.value = v), { ms: 200 });
  if (!on && off) { off(); off = null; }
}, { immediate: true });
onUnmounted(() => off?.());

const v = computed(() => (view.value?.valid ? view.value : null));
const knobs = computed(() => (v.value ? params(v.value) : []));
const fm = computed(() => (v.value?.mode === "fm" && v.value.fm ? { ...fmLayout(v.value.fm), p: v.value.fm } : null));
const daisy = computed(() => (v.value?.mode === "daisy" && v.value.daisy ? { lines: daisyLines(v.value.daisy), model: daisyModel(v.value.daisy.model) } : null));
const multi = computed(() => (v.value?.mode === "multi" && v.value.multi ? { bars: zoneBars(v.value.multi), p: v.value.multi } : null));
const steps = computed(() => (v.value?.locks ?? []).map((n, i) => ({ n, trig: !!v.value?.trig?.[i] })));

const picked = ref(-1);
const msg = ref("");
async function pick(t: number) {
  msg.value = "";
  try {
    await cmd(pickLine(t));
    picked.value = t;
  } catch (e) {
    msg.value = e instanceof RequestError ? `Refused (${e.code}): ${e.message}` : (e as Error).message;
  }
}
// FM's drawing: a box 40 x 26 a grid cell, 64 x 52 apart
const BX = 64, BY = 52;
const ox = (x: number) => 8 + x * BX, oy = (y: number) => 8 + y * BY;
</script>

<template>
  <Card label="Selected track">
    <p v-if="!served" class="small">This Sidecar does not send the VOICE page yet (the "voices" topic).</p>
    <template v-else>
      <div class="pick" role="group" aria-label="Show track">
        <button type="button" :aria-pressed="picked < 0" :disabled="!paired" @click="pick(-1)">Follow the Rytm</button>
        <button v-for="(t, i) in TRACKS" :key="t" type="button" class="num" :aria-pressed="picked === i" :aria-label="`T${i + 1} ${t}`" :disabled="!paired" @click="pick(i)">{{ i + 1 }}</button>
      </div>
      <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
      <p v-if="!v" class="small">Waiting for the Rytm to report its selected track…</p>
      <template v-else>
        <div class="head">
          <span class="num tr">{{ trackName(v.track) }}</span>
          <span class="name">{{ v.name }}</span>
          <span class="small">{{ v.sidecar ? "Sidecar machine" : "the Rytm's own sound" }}</span>
        </div>
        <div v-if="knobs.length" class="knobs">
          <Knob v-for="k in knobs" :key="k.n" :label="k.label" :model-value="k.value" readonly />
        </div>
        <p class="small">The knobs follow the Rytm: turn them there.</p>

        <figure v-if="fm" class="panel">
          <figcaption><span class="label">6-OP FM</span> {{ fm.p.name || "no patch" }} · ALG <span class="num">{{ fm.p.alg + 1 }}</span> · FB <span class="num">{{ fm.p.fb }}</span></figcaption>
          <svg :viewBox="`0 0 ${fm.w * BX + 8} ${fm.h * BY + 8}`" class="alg" role="img" :aria-label="`Algorithm ${fm.p.alg + 1}: carriers ${fm.ops.filter((o) => o.carrier).map((o) => o.n).join(', ')}`">
            <line v-for="([m, k], i) in fm.links" :key="i" :x1="ox(fm.ops[m].x) + 24" :y1="oy(fm.ops[m].y) + 30" :x2="ox(fm.ops[k].x) + 24" :y2="oy(fm.ops[k].y)" class="link" />
            <g v-for="o in fm.ops" :key="o.n">
              <rect :x="ox(o.x)" :y="oy(o.y)" width="48" height="30" rx="6" :class="o.carrier ? 'car' : 'mod'" />
              <rect :x="ox(o.x) + 4" :y="oy(o.y) + 24" :width="40 * o.out / 99" height="3" class="out" />
              <text :x="ox(o.x) + 24" :y="oy(o.y) + 14" text-anchor="middle" class="opn">{{ o.n }}</text>
              <text :x="ox(o.x) + 24" :y="oy(o.y) + 22" text-anchor="middle" class="opf">{{ o.freq }}</text>
            </g>
            <path v-if="fm.fb !== null" :d="`M ${ox(fm.ops[fm.fb].x) + 48} ${oy(fm.ops[fm.fb].y) + 15} h 8 v -19 h -32 v 4`" class="link fb" />
          </svg>
        </figure>

        <figure v-else-if="daisy" class="panel">
          <figcaption><span class="label">DAISYSP</span> {{ daisy.model }}</figcaption>
          <svg viewBox="0 0 200 60" class="modes" role="img" :aria-label="`${daisy.model}: ${daisy.lines.length} resonator modes`">
            <line x1="0" y1="59" x2="200" y2="59" class="base" />
            <line v-for="(l, i) in daisy.lines" :key="i" :x1="4 + l.x * 192" :x2="4 + l.x * 192" y1="58" :y2="58 - l.h * 54" class="mode" />
          </svg>
        </figure>

        <figure v-else-if="multi" class="panel">
          <figcaption><span class="label">MULTISAMPLE</span> {{ multi.p.inst || "no instrument" }}<template v-if="!multi.p.ready"> · loading</template></figcaption>
          <svg viewBox="0 0 200 24" class="zones" role="img" :aria-label="`${multi.bars.length} key zones`">
            <rect v-for="(z, i) in multi.bars" :key="i" :x="z.x0 * 200 + 1" :width="Math.max(1, (z.x1 - z.x0) * 200 - 2)" y="2" height="20" rx="3" class="zone" />
          </svg>
        </figure>

        <div v-if="steps.length" class="steps" role="img" :aria-label="`P-locks on ${steps.filter((s) => s.n).length} of ${steps.length} steps`">
          <span v-for="(s, i) in steps" :key="i" :class="{ trig: s.trig }" :style="{ opacity: s.n ? 0.35 + Math.min(1, s.n / 8) * 0.65 : 0.15 }" />
        </div>
      </template>
    </template>
  </Card>
</template>

<style scoped>
.pick { display: flex; flex-wrap: wrap; gap: 0.375rem; }
.pick button { min-width: var(--target); }
.pick button[aria-pressed="true"] { border: 2px solid var(--accent); background: var(--accent-tint); font-weight: 700; }
.head { display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.25rem 0.75rem; }
.tr { font-size: 1.25rem; color: var(--accent); }
.name { font-family: var(--font-dot); font-size: 1.5rem; }
.knobs { display: grid; grid-template-columns: repeat(auto-fill, minmax(4.5rem, 1fr)); gap: 0.75rem; justify-items: center; }
.panel { margin: 0; display: flex; flex-direction: column; gap: 0.5rem; }
.alg { width: 100%; max-width: 24rem; height: auto; }
.alg .mod { fill: var(--card); stroke: var(--control-border); stroke-width: 1.5; }
.alg .car { fill: var(--accent-tint); stroke: var(--accent); stroke-width: 2; }
.alg .out { fill: var(--accent); }
.alg .link { stroke: var(--label); stroke-width: 1.5; fill: none; }
.alg .opn { fill: var(--text); font-size: 11px; font-weight: 700; }
.alg .opf { fill: var(--label); font-size: 7px; }
.modes, .zones { width: 100%; max-width: 30rem; height: auto; }
.modes .base { stroke: var(--border); }
.modes .mode { stroke: var(--accent); stroke-width: 2.5; stroke-linecap: round; }
.zones .zone { fill: var(--accent-tint); stroke: var(--accent); }
.steps { display: grid; grid-template-columns: repeat(16, 1fr); gap: 3px; max-width: 30rem; }
.steps span { aspect-ratio: 1; border-radius: 3px; background: var(--accent); }
.steps span.trig { outline: 1px solid var(--text); }
</style>
