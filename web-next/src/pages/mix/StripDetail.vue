<!-- web-next/src/pages/mix/StripDetail.vue - the selected strip: its POST EQ (the curve large, its 4 knobs), its two
     insert slots (type, P1..P4), and for a voice its output (route, pan, width) and its 3 sends. -->
<script setup lang="ts">
import DotFill from "../../ui/DotFill.vue";
import SpectrumDots from "../../ui/SpectrumDots.vue";
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import Knob from "../../ui/Knob.vue";
import Segmented from "../../ui/Segmented.vue";
import { EQ_NAMES, INS_LABELS, INS_TYPES, MFX_TYPES, eqPath, eqText, mixCmd, type Strip } from "../../lib/mix";

const props = defineProps<{ strip: Strip; disabled: boolean }>();
const emit = defineEmits<{ cmd: [string] }>();
const s = computed(() => props.strip);
const types = computed(() => (s.value.chain < 0 ? MFX_TYPES : s.value.chain < 8 ? INS_TYPES : INS_TYPES.filter((t) => t !== "AMP")));
const labels = (slot: number) => {
  const x = s.value.slots[slot];
  if (!x) return ["P1", "P2", "P3", "P4"];
  if (x.l && typeof x.l[0] === "string") return x.l as string[]; // the master gives its labels
  return INS_LABELS[x.t] ?? ["P1", "P2", "P3", "P4"];
};
const valueText = (slot: number, k: number) => (v: number) => s.value.slots[slot]?.v?.[k] || String(v);
const freqs = ["40", "200", "1.2k", "6k", "16k"];
const live = ref(false); // the spectrum shows: the EQ's fill steps back (grey dots), the curve stays the accent
</script>

<template>
  <Card :label="`${s.id} · ${s.name}`">
    <section class="part eq" aria-label="POST EQ">
      <span class="label">POST EQ</span>
      <div class="eqbody">
      <div class="curve">
      <svg viewBox="0 0 360 140" class="big" role="img" :aria-label="`EQ curve: ${eqText(s.peq)}`">
        <g class="grid"><line x1="0" y1="70" x2="360" y2="70" /><line x1="90" y1="0" x2="90" y2="140" /><line x1="180" y1="0" x2="180" y2="140" /><line x1="270" y1="0" x2="270" y2="140" /></g>
        <SpectrumDots :strip="s.id" :width="360" :bottom="140" :r="1.7" @live="(v: boolean) => (live = v)" /><!-- the strip's sound after its EQ (its own redraws) -->
        <DotFill :d="eqPath(s.peq, 360, 140)" :width="360" :bottom="140" :step="5.5" :r="1.6" :muted="live" />
        <path class="line" :d="eqPath(s.peq, 360, 140)" />
      </svg>
      <div class="freqs small" aria-hidden="true"><span v-for="f in freqs" :key="f">{{ f }}</span></div>
      </div>
      <div class="eqctl">
      <div class="knobs">
        <Knob :disabled="disabled" v-for="(n, k) in EQ_NAMES" :key="n" :label="n" :model-value="s.peq[k]" @commit="(v: number) => emit('cmd', mixCmd.eq(s, k, v))" />
      </div>
      <button type="button" :disabled="disabled" @click="emit('cmd', mixCmd.eqOff(s))">EQ off</button>
      </div>
      </div>
    </section>

    <div class="parts">

    <section v-for="slot in s.chain < 0 ? [0, 1, 2, 3] : [0, 1]" :key="slot" class="part" :aria-label="s.chain < 0 ? `Slot ${slot + 1}` : `Insert ${'AB'[slot]}`">
      <div class="row">
        <label :for="`ins-${s.id}-${slot}`" class="label">{{ s.chain < 0 ? `Slot ${slot + 1}` : `Insert ${"AB"[slot]}` }}</label>
        <select :id="`ins-${s.id}-${slot}`" :value="s.slots[slot]?.t ?? 'OFF'" :disabled="disabled" @change="emit('cmd', mixCmd.type(s, slot, ($event.target as HTMLSelectElement).value))">
          <option v-for="t in types" :key="t" :value="t">{{ t }}</option>
        </select>
      </div>
      <div v-if="s.slots[slot] && s.slots[slot].t !== 'OFF'" class="knobs">
        <Knob
          v-for="k in 4"
          :key="k"
          :disabled="disabled"
          :label="labels(slot)[k - 1] || `P${k}`"
          :model-value="s.slots[slot].p[k - 1] ?? 0"
          :format="valueText(slot, k - 1)"
          @commit="(v: number) => emit('cmd', mixCmd.param(s, slot, k - 1, v))"
        />
        <Knob v-if="s.chain < 0" :disabled="disabled" label="MIX" :model-value="s.slots[slot].mix ?? 127" @commit="(v: number) => emit('cmd', mixCmd.mfxMix(slot, v))" />
      </div>
    </section>

    <section v-if="s.out" class="part" aria-label="Output">
      <span class="label">Output</span>
      <Segmented
        label="Route: own track, PRE pair or POST pair"
        :options="[{ value: 'trk', text: 'Own' }, { value: 'pre', text: 'PRE' }, { value: 'post', text: 'POST' }]"
        :model-value="s.out.route"
        :disabled="disabled"
        @update:model-value="(v: string) => emit('cmd', mixCmd.route(s, v as 'trk' | 'pre' | 'post'))"
      />
      <div class="knobs">
        <Knob :disabled="disabled" label="PAN" :model-value="s.out.pan" :format="(v: number) => (v === 64 ? 'C' : v < 64 ? `L${64 - v}` : `R${v - 64}`)" @commit="(v: number) => emit('cmd', mixCmd.out(s, 'pan', v))" />
        <Knob :disabled="disabled" label="WIDTH" :model-value="s.out.wid" @commit="(v: number) => emit('cmd', mixCmd.out(s, 'wid', v))" />
      </div>
    </section>

    <section v-if="s.sends" class="part" aria-label="Sends">
      <span class="label">Sends</span>
      <div class="knobs">
        <Knob :disabled="disabled" v-for="b in 3" :key="b" :label="`B${b}`" :model-value="s.sends[b - 1] ?? 0" @commit="(v: number) => emit('cmd', mixCmd.send(s, b - 1, v))" />
      </div>
    </section>
    </div>
  </Card>
</template>

<style scoped>
/* the EQ across the top (curve left, its knobs right); the inserts, output and sends as columns under it, wrapping
   to fewer columns as the page narrows */
.part { display: flex; flex-direction: column; gap: 0.5rem; border-top: 1px solid var(--border); padding-top: 0.75rem; min-width: 0; }
.part.eq { border-top: 0; padding-top: 0; }
.eqbody { display: flex; flex-wrap: wrap; align-items: center; gap: 1rem 2rem; }
.curve { flex: 1 1 20rem; max-width: 36rem; }
.eqctl { display: flex; flex-direction: column; align-items: flex-start; gap: 0.5rem; }
.parts { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 17.5rem), 1fr)); gap: 1rem 1.5rem; margin-top: 1rem; }
.big { display: block; width: 100%; height: auto; }
.grid line { stroke: var(--border); stroke-width: 1; }
.line { fill: none; stroke: var(--accent); stroke-width: 2.5; }
.freqs { display: flex; justify-content: space-between; color: var(--label); }
.knobs { display: flex; flex-wrap: wrap; gap: 0.5rem; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
:root[data-theme="contrast"] .line { stroke: var(--text); }
</style>
