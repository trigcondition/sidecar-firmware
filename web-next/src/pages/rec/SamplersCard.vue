<!-- Rec › the sampler (one take into /Samples/<project>/, trimmed, sliced) and the auto-sampler (a source played over
     notes and velocities into an instrument); core/samp, the old page's Record tab. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { ASAMP_STATES, SAMP_STATES, asampLines, sampLines, type AsampSettings, type SampSettings } from "../../lib/rec";

const { status, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const s = computed(() => st.value.samp as Record<string, any> | undefined);
const a = computed(() => st.value.asamp as Record<string, any> | undefined);
const SOURCES: [string, string][] = [["in", "AUDIO IN L/R"], ["main", "MAIN (resample)"], ...["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"].map((n, i) => [`track:${i + 1}`, n] as [string, string]),
  ...["BD", "BT", "SD", "LT", "CH/OH", "MT/HT", "CY/CB", "RS/CP"].map((n, i) => [`voice:${i + 1}`, `Sidecar voice ${n}`] as [string, string])];
const smp = ref<SampSettings>({ src: "in", start: "now", thr: -30, len: 0, slices: "trans" });
const as = ref<AsampSettings>({ src: "fm:0", notes: "c2:c6:3", vel: "100", hold: 1500, tail: 2500, name: "" });
const msg = ref("");
async function run(lines: string[]) {
  msg.value = "";
  try {
    for (const l of lines) await cmd(l);
  } catch (e) {
    msg.value = (e as Error).message;
  }
}
</script>

<template>
  <Card label="Sample">
    <div class="grid">
      <label>Source <select v-model="smp.src"><option v-for="[v, n] in SOURCES" :key="v" :value="v">{{ n }}</option></select></label>
      <label>Start <select v-model="smp.start"><option value="now">now</option><option value="thr">at a threshold</option><option value="play">the Rytm's PLAY</option><option value="bar">its next bar</option></select></label>
      <label v-if="smp.start === 'thr'">Threshold (dB) <input v-model.number="smp.thr" type="number" min="-90" max="0" /></label>
      <label>Length in bars (0 free) <input v-model.number="smp.len" type="number" min="0" max="64" /></label>
      <label>Slices <select v-model="smp.slices"><option value="trans">at transients</option><option value="grid:16">grid of 16</option><option value="grid:8">grid of 8</option><option value="off">none</option></select></label>
    </div>
    <div class="row">
      <button type="button" :disabled="!paired" @click="run(sampLines(smp))"><span aria-hidden="true">● </span>Arm / record</button>
      <button type="button" :disabled="!paired" @click="run(['samp=stop'])"><span aria-hidden="true">■ </span>Stop</button>
      <span v-if="s" role="status">
        {{ SAMP_STATES[s.state] ?? "?" }}<template v-if="s.state === 2"> <span class="num">{{ (s.ms / 1000).toFixed(1) }}</span> s</template><template v-if="s.bpm"> · {{ s.bpm }} bpm</template>
        <template v-if="s.wav"> · last {{ s.wav }} ({{ s.nSlices }} slices)</template><template v-if="s.error"> · <span class="warm">{{ s.error }}</span></template>
      </span>
    </div>
    <p class="small">One take into /Samples/&lt;project&gt;/ (24-bit), trimmed and normalised, sliced; MULTISAMPLE lists it at once.</p>
  </Card>

  <Card label="Auto-sampler">
    <div class="grid">
      <label>Source <input v-model="as.src" autocomplete="off" spellcheck="false" aria-describedby="as-help" /></label>
      <label>Notes <input v-model="as.notes" autocomplete="off" spellcheck="false" /></label>
      <label>Velocities <input v-model="as.vel" autocomplete="off" spellcheck="false" /></label>
      <label>Hold (ms) <input v-model.number="as.hold" type="number" min="10" /></label>
      <label>Release (ms) <input v-model.number="as.tail" type="number" min="0" /></label>
      <label>Name <input v-model="as.name" placeholder="auto" autocomplete="off" /></label>
    </div>
    <p id="as-help" class="small">Source: fm:PAT, juno:PAT, voice:V, track:T (BD..CB as 1..12), ext[:usb|dev|rytm][:CH] (an external synth: MIDI out, AUDIO IN).</p>
    <div class="row">
      <button type="button" :disabled="!paired" @click="run(asampLines(as))">Start</button>
      <button type="button" :disabled="!paired" @click="run(['asamp=cancel'])">Cancel</button>
      <template v-if="a">
        <progress :value="a.total ? a.done : 0" :max="a.total || 1" :aria-label="`Auto-sampler ${a.done} of ${a.total}`" />
        <span role="status">{{ ASAMP_STATES[a.state] ?? "?" }}<template v-if="a.state === 1 || a.state === 2"> {{ a.label }}</template><template v-if="a.sfz"> · {{ a.sfz }} ({{ a.written }} samples)</template><template v-if="a.error"> · <span class="warm">{{ a.error }}</span></template></span>
      </template>
    </div>
    <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
  </Card>
</template>

<style scoped>
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 13rem), 1fr)); gap: 0.5rem 1rem; }
.grid label { display: flex; flex-direction: column; gap: 0.25rem; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
progress { width: 8rem; accent-color: var(--accent); }
</style>
