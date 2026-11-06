<!-- Settings › Audio (the old page's Sound tab, its settings): MIDI notes, output, IN L/R, computer audio, FIFO. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import SaveState from "../../ui/SaveState.vue";
import { useSetting } from "../../lib/pending";
import { useSidecar } from "../../lib/useSidecar";
import { boxName } from "../../lib/tiles";
import { has } from "../../lib/box";

const { status, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const box = computed(() => boxName(status.value));

const notes = useSetting<boolean>(status, { read: () => st.value.notes, toCmd: (v) => `notes=${v ? 1 : 0}`, send: cmd });
const out = useSetting<string>(status, { read: () => st.value.out, toCmd: (v) => `out=${v}`, send: cmd });
const fxin = useSetting<number>(status, { read: () => st.value.fxin?.want, toCmd: (v) => `fxin=${v}`, send: cmd });
const mac = useSetting<boolean>(status, { read: () => (st.value.mac == null ? undefined : !!st.value.mac), toCmd: (v) => `mac=${v ? "on" : "off"}`, send: cmd });
const fifo = ref<number | null>(null);
const fifoMsg = ref("");
async function setFifo() {
  if (fifo.value == null) return;
  fifoMsg.value = "";
  try {
    await cmd(`fifo=${Math.round(fifo.value)}`);
    fifoMsg.value = "Sent; the Sidecar keeps it within its limits.";
  } catch (e) {
    fifoMsg.value = (e as Error).message;
  }
}
const fxinState = computed(() => st.value.fxin?.state as string | undefined);
</script>

<template>
  <Card label="Audio">
    <div class="field">
      <span id="l-notes" class="label">MIDI notes play the voices</span>
      <Segmented label="MIDI notes" :options="[{ value: true, text: 'On' }, { value: false, text: 'Off' }]" :model-value="notes.value.value ?? false" :disabled="!paired" @update:model-value="notes.set" />
      <SaveState :state="notes.state.value" :msg="notes.msg.value" />
    </div>
    <div class="field">
      <span class="label">Output</span>
      <Segmented label="Output" :options="[{ value: 'main', text: 'Main L/R' }, { value: 'tracks', text: 'Tracks' }]" :model-value="out.value.value ?? 'tracks'" :disabled="!paired" @update:model-value="out.set" />
      <SaveState :state="out.state.value" :msg="out.msg.value" />
    </div>
    <div v-if="fxinState && fxinState !== 'unsupported' && has(status, 'fxin')" class="field">
      <span class="label">{{ box }} IN L/R</span>
      <Segmented label="IN L/R source" :options="[{ value: 0, text: 'Inputs' }, { value: 1, text: 'FX return' }]" :model-value="fxin.value.value ?? 0" :disabled="!paired" @update:model-value="fxin.set" />
      <SaveState :state="fxin.state.value" :msg="fxin.msg.value" />
      <p v-if="fxin.value.value === 1" class="small warm"><span aria-hidden="true">⚠ </span>Feedback: an FX return that goes back into the same FX howls.</p>
      <p class="small">The {{ box }} reports: {{ fxinState }}.</p>
    </div>
    <div class="field">
      <span class="label">Computer audio (USB-C)</span>
      <Segmented label="Computer audio" :options="[{ value: true, text: 'On' }, { value: false, text: 'Off' }]" :model-value="mac.value.value ?? false" :disabled="!paired || st.mac == null" @update:model-value="mac.set" />
      <SaveState :state="mac.state.value" :msg="mac.msg.value" />
      <p v-if="st.mac == null" class="small">Not in this USB-C mode.</p>
    </div>
    <form class="field" @submit.prevent="setFifo">
      <label for="fifo" class="label">FIFO target (frames)</label>
      <div class="row">
        <input id="fifo" v-model.number="fifo" type="number" min="24" max="480" step="12" :placeholder="String(st.fifoTarget ?? '')" inputmode="numeric" />
        <button type="submit" :disabled="!paired || fifo == null">Set</button>
        <span class="small">now <span class="num">{{ st.fifoTarget ?? "–" }}</span></span>
      </div>
      <p v-if="fifoMsg" class="small" role="status">{{ fifoMsg }}</p>
    </form>
  </Card>
</template>

<style scoped>
.field { display: flex; flex-direction: column; gap: 0.375rem; align-items: flex-start; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
input[type="number"] { width: 7rem; }
</style>
