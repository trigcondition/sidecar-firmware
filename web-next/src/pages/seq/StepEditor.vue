<!-- Seq › the step editor (the old page's Sequencer step editor; /status "seqx"): a step of the shown track, its ratchet
     or arp, its two FX columns, random notes, and a lock's condition and slide. The step follows the selected note. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import { FX_CMDS, RATES, RT_MODES, VCURVES, stepFrom, stepLines, type StepSettings } from "../../lib/seqx";

const props = defineProps<{ track: number; step: number | null; seqx: any; disabled: boolean; length: number }>(); // track 1..12; step 1-based
const emit = defineEmits<{ send: [string[]] }>();
const s = ref(1);
watch(() => props.step, (v) => { if (v) s.value = v; }, { immediate: true });
const loaded = computed(() => stepFrom(props.seqx?.t, props.track, s.value));
const f = ref<StepSettings>({ ...loaded.value.f });
// a new step, track or status: the fields again (not while the form has focus: the old page's rule)
const form = ref<HTMLFormElement | null>(null);
watch(loaded, (l) => { if (!form.value?.contains(document.activeElement)) f.value = { ...l.f }; });
watch(s, () => (f.value = { ...loaded.value.f }));
function set() {
  emit("send", stepLines(props.track, s.value, f.value, loaded.value.was));
}
const opts = (l: string[]) => l.map((n, i) => ({ v: i, n: n.toUpperCase() }));
</script>

<template>
  <Card label="Step">
    <form ref="form" class="cols" @submit.prevent="set">
      <label>Step <input v-model.number="s" type="number" min="1" :max="length" /></label>
      <fieldset>
        <legend class="label">Ratchet / arp</legend>
        <label>Mode <select v-model.number="f.rm"><option :value="-1">OFF</option><option v-for="o in opts(RT_MODES)" :key="o.v" :value="o.v">{{ o.n }}</option></select></label>
        <template v-if="f.rm >= 0">
          <label>Count <input v-model.number="f.rc" type="number" min="0" max="16" /></label>
          <label>Rate <select v-model.number="f.rr"><option v-for="o in opts(RATES)" :key="o.v" :value="o.v">{{ o.n }}</option></select></label>
          <label>Velocity curve <select v-model.number="f.rv"><option v-for="o in opts(VCURVES)" :key="o.v" :value="o.v">{{ o.n }}</option></select></label>
          <label>Pitch curve <input v-model.number="f.rp" type="number" min="-24" max="24" /></label>
          <label>Octaves <input v-model.number="f.ro" type="number" min="1" max="4" /></label>
          <label>Gate % <input v-model.number="f.rg" type="number" min="1" max="100" /></label>
        </template>
      </fieldset>
      <fieldset>
        <legend class="label">FX columns</legend>
        <label>FX 1 <select v-model.number="f.f1"><option v-for="(c, i) in FX_CMDS" :key="c" :value="i">{{ c }}</option></select></label>
        <label v-if="f.f1">value <input v-model="f.v1" autocomplete="off" /></label>
        <label>FX 2 <select v-model.number="f.f2"><option v-for="(c, i) in FX_CMDS" :key="c" :value="i">{{ c }}</option></select></label>
        <label v-if="f.f2">value <input v-model="f.v2" autocomplete="off" /></label>
      </fieldset>
      <fieldset>
        <legend class="label">Random and locks</legend>
        <label class="check"><input v-model="f.rnd" type="checkbox" /> Random notes on this step</label>
        <label>Lock parameter P (0 none) <input v-model.number="f.lp" type="number" min="0" max="32" /></label>
        <template v-if="f.lp >= 1">
          <label>Condition <input v-model="f.lc" placeholder="--- / fill / 1:2 / 50%" autocomplete="off" /></label>
          <label class="check"><input v-model="f.sl" type="checkbox" /> Slide</label>
        </template>
      </fieldset>
      <button type="submit" :disabled="disabled">Set step {{ s }}</button>
    </form>
  </Card>
</template>

<style scoped>
.cols { display: flex; flex-direction: column; gap: 0.75rem; }
fieldset { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem 0.75rem; display: flex; flex-wrap: wrap; gap: 0.5rem 1rem; margin: 0; }
legend { padding: 0 0.25rem; }
label { display: flex; flex-direction: column; gap: 0.25rem; }
.check { flex-direction: row; align-items: center; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
input[type="number"] { width: 6rem; }
button { align-self: flex-start; }
</style>
