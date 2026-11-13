<!-- Seq › the roll's tools, one at a time in a drawer under the toolbar (the old "Tools" card): the chord stamp on the
     selected note, strum and humanise over the steps on screen. The command lines are the touch editor's (lib/seq.ts,
     lib/seqx.ts). Escape closes the drawer (the toolbar puts the focus back on its button). -->
<script setup lang="ts">
import { ref, useId } from "vue";
import { cmd as seqCmd, noteName, type SeqNote, type SeqView } from "../../lib/seq";
import { humanLine, strumLine } from "../../lib/seqx";
import { stepsText } from "../../lib/sequi";

export type Tool = "chord" | "strum" | "human";
const props = defineProps<{ tool: Tool; view: SeqView; selected: SeqNote | null; first: number; count: number; disabled: boolean }>();
const emit = defineEmits<{ send: [string]; close: [] }>();
const id = useId();

// the chord stamp
const chord = ref(1);
const inv = ref(0);
function stamp() {
  const v = props.view, at = props.selected;
  if (!at) return;
  emit("send", seqCmd.chord(v.track, at.step, at.pitch, chord.value, inv.value, v.defaults.vel, v.defaults.len, 0));
}
// strum and humanise over the steps on screen
const strum = ref(12), hTime = ref(8), hVel = ref(16), hSeed = ref(1);
const last = () => props.first + props.count - 1;
</script>

<template>
  <div class="drawer" @keydown.esc.stop="emit('close')">
    <template v-if="tool === 'chord'">
      <div class="fields">
        <label :for="`${id}-chd`" class="f"><span class="label">Chord</span>
          <select :id="`${id}-chd`" v-model.number="chord">
            <option v-for="(c, i) in view.chords" :key="i" :value="i" :disabled="i === 0">{{ c }}</option>
          </select>
        </label>
        <label :for="`${id}-inv`" class="f"><span class="label">Inversion</span>
          <select :id="`${id}-inv`" v-model.number="inv">
            <option v-for="i in 4" :key="i" :value="i - 1">{{ i - 1 }}</option>
          </select>
        </label>
        <button type="button" class="go" :disabled="disabled || !selected" @click="stamp">
          {{ selected ? `Stamp on ${noteName(selected.pitch)}, step ${selected.step + 1}` : "Stamp on the selected note" }}
        </button>
      </div>
      <p class="small hint">The chord's notes go on the selected note's step, up from its pitch.<template v-if="!selected"> Select a note first.</template></p>
    </template>

    <template v-else-if="tool === 'strum'">
      <div class="fields">
        <label :for="`${id}-strum`" class="f"><span class="label">Strum</span>
          <input :id="`${id}-strum`" v-model.number="strum" type="number" min="-96" max="96" />
        </label>
        <button type="button" class="go" :disabled="disabled" @click="emit('send', strumLine(view.track, first, last(), strum))">Strum steps {{ stepsText(first, count) }}</button>
      </div>
      <p class="small hint">Breaks every chord on screen (a step with 2 or more notes): this many 96ths of a step a note, up from the lowest; below 0, down from the highest.</p>
    </template>

    <template v-else>
      <div class="fields">
        <label :for="`${id}-ht`" class="f"><span class="label">Time</span>
          <input :id="`${id}-ht`" v-model.number="hTime" type="number" min="0" max="96" />
        </label>
        <label :for="`${id}-hv`" class="f"><span class="label">Velocity</span>
          <input :id="`${id}-hv`" v-model.number="hVel" type="number" min="0" max="127" />
        </label>
        <label :for="`${id}-hs`" class="f"><span class="label">Seed</span>
          <input :id="`${id}-hs`" v-model.number="hSeed" type="number" min="0" max="9999" />
        </label>
        <button type="button" class="go" :disabled="disabled" @click="emit('send', humanLine(view.track, first, last(), hTime, hVel, hSeed))">Humanise steps {{ stepsText(first, count) }}</button>
      </div>
      <p class="small hint">Moves each note on screen by up to ± this time (96ths of a step) and velocity; the same seed gives the same result. Undo takes it back.</p>
    </template>
  </div>
</template>

<style scoped>
.drawer { padding: 0.75rem; border: 1px solid var(--border); border-radius: var(--radius-control); background: var(--bg); }
.fields { display: flex; flex-wrap: wrap; align-items: flex-end; gap: 0.5rem 0.875rem; }
.f { display: flex; flex-direction: column; gap: 0.25rem; }
.f input { width: 6rem; }
.go { border-color: var(--accent); }
.hint { margin: 0.5rem 0 0; color: var(--label); }
</style>
