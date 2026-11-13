<!-- Voices › one LFO (core/lfo.h, docs/lfo.md; the old page's LFO tab): target and parameter, wave, sync, retrig, rate,
     depth; its shape with the live phase (decorative: the numbers carry it); for DESIGN the 16 steps to draw (DesignPad). -->
<script setup lang="ts">
import DotFill from "../../ui/DotFill.vue";
import { computed, ref } from "vue";
import DesignPad from "./DesignPad.vue";
import Fader from "../../ui/Fader.vue";
import Segmented from "../../ui/Segmented.vue";
import { LFO_PRESETS, LFO_RETRIGS, LFO_SHAPES, LFO_WAVES, lfoCmd, lfoParamRows, lfoRateText, lfoTargets, lfoTitle, wavePath, type LfoState } from "../../lib/lfo";

const props = defineProps<{ n: number; lfo: LfoState; disabled: boolean; pt?: number }>();
const targets = computed(() => lfoTargets(props.n, props.pt)); // a B (pt): no Rytm target
const emit = defineEmits<{ cmd: [string] }>();
const l = computed(() => props.lfo);
const off = computed(() => l.value.tgt === "OFF");
const params = computed(() => lfoParamRows(l.value.tgt, l.value.par, l.value.pn, l.value.pns));
const path = computed(() => wavePath(l.value, 120, 40));
const brush = ref(0);
function design(hex: string) {
  emit("cmd", lfoCmd.design(props.n, hex));
}
</script>

<template>
  <section class="lfo" :class="{ off }" :aria-label="lfoTitle(n, pt)">
    <div class="head">
      <span class="num id">{{ lfoTitle(n, pt) }}</span>
      <svg viewBox="0 0 120 40" class="wave" aria-hidden="true">
        <DotFill :d="path" :width="120" :bottom="40" :step="5" :r="1.4" :muted="off" />
        <path :d="path" />
        <line v-if="!off" :x1="(l.ph / 1000) * 120" :x2="(l.ph / 1000) * 120" y1="0" y2="40" class="ph" />
      </svg>
    </div>
    <div class="row">
      <label :for="`lt${n}`" class="label">Target</label>
      <select :id="`lt${n}`" :value="l.tgt" :disabled="disabled" @change="emit('cmd', lfoCmd.target(n, ($event.target as HTMLSelectElement).value))">
        <option v-for="t in targets" :key="t" :value="t">{{ t }}</option>
      </select>
      <template v-if="!off">
        <label :for="`lp${n}`" class="label">Parameter</label>
        <select :id="`lp${n}`" :value="l.par" :disabled="disabled" @change="emit('cmd', lfoCmd.set(n, 'par', +($event.target as HTMLSelectElement).value))">
          <option v-for="p in params" :key="p.k" :value="p.k">{{ p.name }}</option>
        </select>
      </template>
    </div>
    <template v-if="!off">
      <div class="row">
        <label :for="`lw${n}`" class="label">Wave</label>
        <select :id="`lw${n}`" :value="l.wav" :disabled="disabled" @change="emit('cmd', lfoCmd.set(n, 'wav', +($event.target as HTMLSelectElement).value))">
          <option v-for="(w, i) in LFO_WAVES" :key="w" :value="i">{{ w }}</option>
        </select>
        <span class="label">Sync</span>
        <Segmented label="Sync" :options="[{ value: 0, text: 'Free' }, { value: 1, text: 'Clock' }]" :model-value="l.sync" :disabled="disabled" @update:model-value="(v: number) => emit('cmd', lfoCmd.set(n, 'sync', v))" />
      </div>
      <span class="label">Retrig</span>
      <Segmented label="Retrig" :options="LFO_RETRIGS.map((r, i) => ({ value: i, text: r }))" :model-value="l.trig" :disabled="disabled" @update:model-value="(v: number) => emit('cmd', lfoCmd.set(n, 'trig', v))" />
      <Fader label="Rate" :model-value="l.rate" :min="0" :max="l.sync ? 18 : 127" :format="(v: number) => lfoRateText(l.sync, v)" :disabled="disabled" @commit="(v: number) => emit('cmd', lfoCmd.set(n, 'rate', v))" />
      <Fader label="Depth" :model-value="l.dep" :min="-64" :max="63" :disabled="disabled" @commit="(v: number) => emit('cmd', lfoCmd.set(n, 'dep', v))" />
      <template v-if="l.wav === 7">
        <span class="label">Design</span>
        <DesignPad :des="l.des" :brush="brush" :ph="l.ph" :disabled="disabled" @commit="design" />
        <div class="row">
          <span class="label">Brush</span>
          <Segmented label="Brush" :options="LFO_SHAPES.map((s, i) => ({ value: i, text: s }))" :model-value="brush" @update:model-value="(v: number) => (brush = v)" />
        </div>
        <div class="row">
          <span class="label">Presets</span>
          <button v-for="p in LFO_PRESETS" :key="p" type="button" :disabled="disabled" @click="emit('cmd', lfoCmd.preset(n, p))">{{ p }}</button>
        </div>
      </template>
    </template>
  </section>
</template>

<style scoped>
.lfo { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.75rem; display: flex; flex-direction: column; gap: 0.5rem; min-width: 0; }
.lfo.off { opacity: 0.85; }
.head { display: flex; align-items: center; gap: 0.75rem; }
.id { font-size: 1.125rem; }
.wave { width: 7.5rem; height: 2.5rem; }
.wave path { fill: none; stroke: var(--accent); stroke-width: 2; }
.off .wave path { stroke: var(--control-border); }
.ph { stroke: var(--warm); stroke-width: 2; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
</style>
