<!-- web-next/src/pages/Seq.vue - the Sidecar sequencer: the shown seq track as a piano roll (the "seq" and "seqpos"
     topics, the touch editor's view), edited with the touch editor's own command lines; its notes, chords, undo; the
     track's and the pattern's settings (the old page's Sound › Poly tracks and Sequencer forms). -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef, watch } from "vue";
import Card from "../ui/Card.vue";
import Chip from "../ui/Chip.vue";
import Fader from "../ui/Fader.vue";
import Segmented from "../ui/Segmented.vue";
import PianoRoll from "./seq/PianoRoll.vue";
import { RequestError } from "../lib/client";
import { patternName } from "../lib/status";
import { DIR, KEYS, POLY_ALLOC, QNT, SCALES, SPD, SUB, cmd as seqCmd, noteName, parseView, pitchWindow, playStep, trackName, type SeqNote, type SeqPos, type SeqView } from "../lib/seq";
import { useSidecar } from "../lib/useSidecar";

const { client, cmd, paired, hello, status } = useSidecar();
const view = shallowRef<SeqView | null>(null);
const pos = shallowRef<SeqPos | null>(null);
const got = ref(false);
const offs: (() => void)[] = [];
onMounted(() => {
  offs.push(client.subscribe("seq", (v) => ((view.value = parseView(v)), (got.value = true))));
  offs.push(client.subscribe<SeqPos>("seqpos", (p) => (pos.value = p), { ms: 50 })); // (the header's runs at 250: this speeds it up)
});
onUnmounted(() => offs.forEach((f) => f()));
const served = computed(() => !hello.value || hello.value.topics.includes("seq"));

const T = computed(() => view.value?.track ?? -1);
const msg = ref("");
async function send(c: string) {
  msg.value = "";
  try {
    await cmd(c);
  } catch (e) {
    msg.value = e instanceof RequestError ? `Refused (${e.code}): ${e.message}` : (e as Error).message;
  }
}

// the window: pages of 16 steps (one on a phone, two where there is room), 24 pitches
const narrow = ref(typeof matchMedia === "function" && matchMedia("(max-width: 40rem)").matches);
// large cells: 8 steps by 12 pitches, rows 2.75 rem (44 px touch targets); a "page" is then 8 steps
const large = ref(false);
const pageSteps = computed(() => (large.value ? 8 : 16));
const page = ref(0);
const perView = computed(() => (narrow.value || large.value ? 1 : Math.min(2, view.value?.pages ?? 1)));
const count = computed(() => Math.min(pageSteps.value * perView.value, Math.max(1, (view.value?.length ?? 16) - page.value * pageSteps.value)));
const first = computed(() => page.value * pageSteps.value);
const pagesOf = computed(() => Math.ceil((view.value?.length ?? 16) / pageSteps.value));
const ROWS = computed(() => (large.value ? 12 : 24));
watch(large, () => {
  page.value = 0;
  if (view.value) top.value = pitchWindow(view.value.notes, ROWS.value, view.value.defaults?.note ?? 60);
});
const top = ref(72);
let windowFor = -1;
watch(view, (v) => {
  if (!v) return;
  if (windowFor !== v.track) {
    windowFor = v.track;
    top.value = pitchWindow(v.notes, ROWS.value, v.defaults?.note ?? 60);
    page.value = 0;
  }
  if (page.value >= pagesOf.value) page.value = Math.max(0, pagesOf.value - 1);
});
function scroll(dp: number, ds: number) {
  if (dp) top.value = Math.max(ROWS.value - 1, Math.min(127, top.value + (dp > 0 ? 12 : -12)));
  if (ds && view.value) page.value = Math.max(0, Math.min(pagesOf.value - perView.value, page.value + (ds > 0 ? 1 : -1)));
}

// drawing: off by default, as the touchscreen's VIEW (a stray tap during play changes nothing)
const draw = ref(false);

// the selected note, found again in each new view by its step and pitch
const selKey = ref<{ step: number; pitch: number } | null>(null);
const selected = computed(() => (selKey.value && view.value?.notes.find((n) => n.step === selKey.value!.step && n.pitch === selKey.value!.pitch)) || null);
function select(n: SeqNote | null) {
  selKey.value = n ? { step: n.step, pitch: n.pitch } : null;
}

function add(step: number, pitch: number) {
  const v = view.value;
  if (!v) return;
  send(seqCmd.add(v.track, step, pitch, 0, v.defaults.vel, v.defaults.len));
  selKey.value = { step, pitch };
}
function remove(n: SeqNote) {
  send(seqCmd.del(T.value, n.step, n.pitch));
  selKey.value = null;
}
function nudge(n: SeqNote, ds: number, dp: number) {
  const s2 = Math.max(0, Math.min((view.value?.length ?? 16) - 1, n.step + ds)), p2 = Math.max(0, Math.min(127, n.pitch + dp));
  send(seqCmd.move(T.value, n.step, n.pitch, s2, p2, n.micro));
  selKey.value = { step: s2, pitch: p2 };
}
const setVel = (n: SeqNote, v: number) => send(seqCmd.vel(T.value, n.step, n.pitch, v));
const setLen = (n: SeqNote, l: number) => send(seqCmd.len(T.value, n.step, n.pitch, l));
const setMicro = (n: SeqNote, m: number) => send(seqCmd.move(T.value, n.step, n.pitch, n.step, n.pitch, m));

// the chord stamp
const chord = ref(1);
const inv = ref(0);
function stamp() {
  const v = view.value, at = selected.value;
  if (!v || !at) return;
  send(seqCmd.chord(v.track, at.step, at.pitch, chord.value, inv.value, v.defaults.vel, v.defaults.len, 0));
}

// the track's and the pattern's settings
const swing = ref(50);
watch(() => view.value?.swing, (s) => typeof s === "number" && (swing.value = s), { immediate: true });
const polyRow = computed(() => {
  const p = (status.value as Record<string, any> | null)?.poly;
  return Array.isArray(p) ? p.find((x: any) => x.t === T.value + 1) : undefined;
});
const poly = ref({ voices: 0, alloc: 0, unison: 1, detune: 0, spread: 0 });
watch([() => view.value?.poly, polyRow], () => {
  const v = view.value?.poly, r = polyRow.value;
  if (v) poly.value = { voices: v.voices, alloc: v.alloc, unison: Math.max(1, v.unison), detune: r?.d ?? poly.value.detune, spread: r?.s ?? poly.value.spread };
}, { immediate: true });
const pat = ref({ key: 0, scale: 0, qnt: 0 });
watch(() => view.value && [view.value.key, view.value.scale, view.value.qnt], () => {
  if (view.value) pat.value = { key: view.value.key, scale: view.value.scale, qnt: view.value.qnt };
}, { immediate: true });

const transportText = computed(() => ["stopped", "playing", "paused"][pos.value?.transport ?? view.value?.transport ?? 0] ?? "");
const head = computed(() => playStep(pos.value, T.value));
const opts = (names: string[]) => names.map((n, i) => ({ value: i, text: n }));
</script>

<template>
  <h1 class="title" tabindex="-1">Seq</h1>
  <p v-if="!served" class="card">This Sidecar's build has no sequencer view (no core/seq in it).</p>
  <p v-else-if="!got" class="card">Waiting for the sequencer…</p>
  <p v-else-if="!view" class="card">No Sidecar sequencer in this build.</p>
  <template v-else>
    <Card :label="view.track >= 0 ? `${trackName(view.track)} · ${view.machine || 'seq track'}` : 'Sequencer'">
      <div class="facts">
        <span>Pattern <b class="num">{{ patternName(view.pattern) || "–" }}</b></span>
        <span v-if="view.bpm10"><b class="num">{{ (view.bpm10 / 10).toFixed(1) }}</b> BPM</span>
        <span>{{ transportText }}</span>
        <span v-if="view.project">{{ view.project }}</span>
        <span><span class="num">{{ view.length }}</span> steps</span>
      </div>
      <div v-if="view.tracks.some((t) => t >= 0)" class="row" role="group" aria-label="Seq tracks">
        <span class="label">Track</span>
        <Chip v-for="t in view.tracks.filter((x) => x >= 0)" :key="t" :pressed="t === view.track" :disabled="!paired" @click="send(seqCmd.show(t))">{{ trackName(t) }}</Chip>
      </div>
      <p v-if="view.track < 0">None of this pattern's tracks is a seq track. Make one on the Rytm (TRK + the SEQ page).</p>
      <template v-else>
        <div class="row">
          <Segmented label="Mode" :options="[{ value: false, text: 'View' }, { value: true, text: 'Draw' }]" :model-value="draw" :disabled="!paired" @update:model-value="(v: boolean) => (draw = v)" />
          <button type="button" :disabled="page === 0" @click="scroll(0, -1)">‹ Steps</button>
          <span class="small">steps <span class="num">{{ first + 1 }}–{{ first + count }}</span> of {{ view.length }}</span>
          <button type="button" :disabled="first + count >= view.length" @click="scroll(0, 1)">Steps ›</button>
          <button type="button" @click="scroll(1, 0)">Octave up</button>
          <button type="button" @click="scroll(-1, 0)">Octave down</button>
          <Segmented label="Cell size" :options="[{ value: false, text: 'Normal' }, { value: true, text: 'Large cells' }]" :model-value="large" @update:model-value="(v: boolean) => (large = v)" />
        </div>
        <PianoRoll
          :notes="view.notes"
          :first="first"
          :count="count"
          :top="top"
          :rows="ROWS"
          :row-rem="large ? 2.75 : 1.5"
          :play="head"
          :draw="draw && paired"
          :selected="selected"
          :length="view.length"
          @select="select"
          @add="add"
          @remove="remove"
          @nudge="nudge"
          @vel="(n, d) => setVel(n, n.vel + d)"
          @len="(n, d) => setLen(n, n.len + d)"
          @scroll="scroll"
        />
        <p class="small">{{ view.notes.length }} notes. <template v-if="!draw">Drawing is off: taps only select.</template></p>
        <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
      </template>
    </Card>

    <div v-if="view.track >= 0" class="grid">
      <Card :label="selected ? `Note ${noteName(selected.pitch)} · step ${selected.step + 1}` : 'Note'">
        <p v-if="!selected">Select a note in the roll (a tap, or Enter on it).</p>
        <template v-else>
          <Fader :key="'v' + selected.step + selected.pitch" label="Velocity" :model-value="selected.vel" :min="1" :max="127" :disabled="!paired" @commit="(v: number) => setVel(selected!, v)" />
          <Fader :key="'l' + selected.step + selected.pitch" label="Length" :model-value="selected.len" :min="SUB / 4" :max="SUB * 16" :step="SUB / 4" :format="(v: number) => `${+(v / SUB).toFixed(2)} steps`" :disabled="!paired" @commit="(v: number) => setLen(selected!, v)" />
          <Fader :key="'m' + selected.step + selected.pitch" label="Micro timing" :model-value="selected.micro" :min="-96" :max="96" :format="(v: number) => `${v > 0 ? '+' : ''}${v}/96`" :disabled="!paired" @commit="(v: number) => setMicro(selected!, v)" />
          <div class="row">
            <button type="button" :disabled="!paired" @click="nudge(selected, 0, 12)">Octave up</button>
            <button type="button" :disabled="!paired" @click="nudge(selected, 0, -12)">Octave down</button>
            <button type="button" class="danger" :disabled="!paired" @click="remove(selected)">Delete</button>
          </div>
        </template>
      </Card>

      <Card label="Tools">
        <div class="row">
          <button type="button" :disabled="!paired || !view.undo" @click="send(seqCmd.undo(view.track))">Undo <span class="num">{{ view.undo || "" }}</span></button>
          <button type="button" :disabled="!paired || !view.redo" @click="send(seqCmd.redo(view.track))">Redo <span class="num">{{ view.redo || "" }}</span></button>
        </div>
        <div class="row">
          <label for="chd" class="label">Chord</label>
          <select id="chd" v-model.number="chord">
            <option v-for="(c, i) in view.chords" :key="i" :value="i" :disabled="i === 0">{{ c }}</option>
          </select>
          <label for="inv" class="label">Inversion</label>
          <select id="inv" v-model.number="inv">
            <option v-for="i in 4" :key="i" :value="i - 1">{{ i - 1 }}</option>
          </select>
          <button type="button" :disabled="!paired || !selected" @click="stamp">Stamp on the selected note</button>
        </div>
        <p class="small">The stamp puts the chord's notes on the selected note's step, from its pitch.</p>
      </Card>

      <Card :label="`${trackName(view.track)} settings`">
        <Fader v-model="swing" label="Swing" :min="50" :max="80" unit="%" :disabled="!paired" @commit="(v: number) => send(seqCmd.swing(view!.track, v))" />
        <div class="row">
          <label for="spd" class="label">Speed</label>
          <select id="spd" :value="view.speed" :disabled="!paired" @change="send(seqCmd.spd(view!.track, +($event.target as HTMLSelectElement).value))">
            <option v-for="(s, i) in SPD" :key="s" :value="i">{{ s }}</option>
          </select>
          <label for="dir" class="label">Direction</label>
          <select id="dir" :value="view.dir" :disabled="!paired" @change="send(seqCmd.dir(view!.track, +($event.target as HTMLSelectElement).value))">
            <option v-for="(d, i) in DIR" :key="d" :value="i">{{ d }}</option>
          </select>
        </div>
        <span class="label">Poly</span>
        <div class="row">
          <label for="pv">Voices</label>
          <select id="pv" v-model.number="poly.voices"><option v-for="v in 9" :key="v" :value="v - 1">{{ v === 1 ? "AUTO" : v - 1 }}</option></select>
          <label for="pa">Allocation</label>
          <select id="pa" v-model.number="poly.alloc"><option v-for="(a, i) in POLY_ALLOC" :key="a" :value="i">{{ a }}</option></select>
          <label for="pu">Unison</label>
          <select id="pu" v-model.number="poly.unison"><option v-for="u in 8" :key="u" :value="u">{{ u === 1 ? "OFF" : u }}</option></select>
        </div>
        <div class="row">
          <label for="pd">Detune</label>
          <input id="pd" v-model.number="poly.detune" type="number" min="0" max="127" />
          <label for="ps">Spread</label>
          <input id="ps" v-model.number="poly.spread" type="number" min="0" max="127" />
          <button type="button" :disabled="!paired" @click="send(seqCmd.poly(view!.track, poly))">Set poly</button>
        </div>
        <p v-if="view.poly.info" class="small">{{ view.poly.info }}</p>
      </Card>

      <Card label="Pattern">
        <div class="row">
          <label for="key" class="label">Key</label>
          <select id="key" v-model.number="pat.key"><option v-for="(k, i) in KEYS" :key="k" :value="i">{{ k }}</option></select>
          <label for="scale" class="label">Scale</label>
          <select id="scale" v-model.number="pat.scale"><option v-for="(s, i) in SCALES" :key="s" :value="i">{{ s }}</option></select>
        </div>
        <span class="label">Quantise to the scale</span>
        <Segmented label="Quantise" :options="opts(QNT)" :model-value="pat.qnt" @update:model-value="(v: number) => (pat.qnt = v)" />
        <button type="button" :disabled="!paired" @click="send(seqCmd.key(pat.key, pat.scale, pat.qnt))">Set key and scale</button>
      </Card>
    </div>
  </template>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.facts { display: flex; flex-wrap: wrap; gap: 0.25rem 1.25rem; color: var(--label); }
.facts b { color: var(--text); }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 22rem), 1fr)); gap: 0 1.25rem; align-items: start; }
.danger { border-color: var(--warm); color: var(--warm); }
input[type="number"] { width: 5.5rem; }
</style>
