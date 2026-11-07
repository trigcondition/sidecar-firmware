<!-- web-next/src/pages/Seq.vue - the Sidecar sequencer: the shown seq track as a piano roll (the "seq" and "seqpos"
     topics, the touch editor's view), edited with the touch editor's own command lines. One card holds the roll: a
     toolbar on top (mode, the steps and pitches shown, undo, the tools in a drawer), the roll, and an inspector under it
     (the selected note; its step's options). Below, the track's and the pattern's settings in one panel of tabs (the
     old page's Sound › Poly tracks and Sequencer forms). -->
<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, shallowRef, useId, watch } from "vue";
import Card from "../ui/Card.vue";
import Chip from "../ui/Chip.vue";
import Fader from "../ui/Fader.vue";
import Segmented from "../ui/Segmented.vue";
import PianoRoll from "./seq/PianoRoll.vue";
import StepEditor from "./seq/StepEditor.vue";
import SeqSettings from "./seq/SeqSettings.vue";
import SeqTools, { type Tool } from "./seq/SeqTools.vue";
import Tabs from "./seq/Tabs.vue";
import { RequestError } from "../lib/client";
import { patternName } from "../lib/status";
import { SUB, cmd as seqCmd, noteName, parseView, pitchWindow, playStep, trackName, type SeqNote, type SeqPos, type SeqView } from "../lib/seq";
import { lenText, microText, stepsText } from "../lib/sequi";
import { useSidecar } from "../lib/useSidecar";
import { coalescer } from "../lib/coalesce";

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
// a phone: always 44 px both ways, 16 steps a page that scroll sideways (the "Large cells" choice is for wider screens)
const large = ref(false);
const big = computed(() => large.value || narrow.value);
const pageSteps = computed(() => (large.value && !narrow.value ? 8 : 16));
const page = ref(0);
const perView = computed(() => (narrow.value || large.value ? 1 : Math.min(2, view.value?.pages ?? 1)));
const count = computed(() => Math.min(pageSteps.value * perView.value, Math.max(1, (view.value?.length ?? 16) - page.value * pageSteps.value)));
const first = computed(() => page.value * pageSteps.value);
const pagesOf = computed(() => Math.ceil((view.value?.length ?? 16) / pageSteps.value));
const ROWS = computed(() => (big.value ? 12 : 24));
watch(big, () => {
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
// velocity and length by keys: a command per press, the newest wins (lib/coalesce.ts)
const latest = coalescer(cmd, (e) => (msg.value = e instanceof RequestError ? `Refused (${e.code}): ${e.message}` : (e as Error).message));
const setVel = (n: SeqNote, v: number) => void latest(seqCmd.vel(T.value, n.step, n.pitch, v));
const setLen = (n: SeqNote, l: number) => void latest(seqCmd.len(T.value, n.step, n.pitch, l));
// + / - and [ / ] count on from the last value sent, not from the view (it comes back later)
const sentVel = new Map<string, number>(), sentLen = new Map<string, number>();
const noteKey = (n: SeqNote) => `${T.value}:${n.step}:${n.pitch}`;
function stepVel(n: SeqNote, d: number) {
  const v = Math.max(1, Math.min(127, (sentVel.get(noteKey(n)) ?? n.vel) + d));
  sentVel.set(noteKey(n), v);
  setVel(n, v);
}
function stepLen(n: SeqNote, d: number) {
  const l = Math.max(SUB / 4, (sentLen.get(noteKey(n)) ?? n.len) + d);
  sentLen.set(noteKey(n), l);
  setLen(n, l);
}
watch(view, () => {
  sentVel.clear();
  sentLen.clear();
});
const setMicro = (n: SeqNote, m: number) => send(seqCmd.move(T.value, n.step, n.pitch, n.step, n.pitch, m));

async function sendLines(lines: string[]) {
  for (const l of lines) await send(l);
}
const seqx = computed(() => (status.value as Record<string, any> | null)?.seqx as Record<string, any> | undefined);

// the tools (chord, strum, humanise): one at a time, in a drawer under the toolbar; Escape closes it
const tool = ref<Tool | null>(null);
const drawerId = useId();
const TOOLS: { id: Tool; text: string }[] = [{ id: "chord", text: "Chord" }, { id: "strum", text: "Strum" }, { id: "human", text: "Humanise" }];
const toolBtns = ref<Record<string, HTMLButtonElement | null>>({});
function toggleTool(t: Tool) {
  tool.value = tool.value === t ? null : t;
}
function closeTool() {
  const t = tool.value;
  tool.value = null;
  if (t) nextTick(() => toolBtns.value[t]?.focus());
}

// the inspector under the roll: the selected note, or its step's options
const pane = ref<"note" | "step">("note");
const PANES = [{ id: "note" as const, text: "Note" }, { id: "step" as const, text: "Step" }];
const transportText = computed(() => ["stopped", "playing", "paused"][pos.value?.transport ?? view.value?.transport ?? 0] ?? "");
const head = computed(() => playStep(pos.value, T.value));
</script>

<template>
  <h1 class="visually-hidden" tabindex="-1">Seq</h1>
  <p v-if="!served" class="card">This Sidecar's build has no sequencer view (no core/seq in it).</p>
  <p v-else-if="!got" class="card">Waiting for the sequencer…</p>
  <p v-else-if="!view" class="card">No Sidecar sequencer in this build.</p>
  <template v-else>
    <Card :label="view.track >= 0 ? `${trackName(view.track)} · ${view.machine || 'seq track'}` : 'Sequencer'">
      <div class="head">
        <div v-if="view.tracks.some((t) => t >= 0)" class="group" role="group" aria-label="Seq tracks">
          <Chip v-for="t in view.tracks.filter((x) => x >= 0)" :key="t" :pressed="t === view.track" :disabled="!paired" @click="send(seqCmd.show(t))">{{ trackName(t) }}</Chip>
        </div>
        <div class="facts">
          <span>Pattern <b class="num">{{ patternName(view.pattern) || "–" }}</b></span>
          <span v-if="view.bpm10"><b class="num">{{ (view.bpm10 / 10).toFixed(1) }}</b> BPM</span>
          <span>{{ transportText }}</span>
          <span v-if="view.project">{{ view.project }}</span>
          <span><span class="num">{{ view.length }}</span> steps</span>
        </div>
      </div>
      <p v-if="view.track < 0">None of this pattern's tracks is a seq track. Make one on the Rytm (TRK + the SEQ page).</p>
      <div v-else class="editor">
        <div class="toolbar">
          <Segmented label="Mode" :options="[{ value: false, text: 'Select' }, { value: true, text: 'Draw' }]" :model-value="draw" :disabled="!paired" @update:model-value="(v: boolean) => (draw = v)" />
          <div class="group" role="group" aria-label="Steps on screen">
            <button type="button" class="icon" aria-label="Earlier steps" :disabled="page === 0" @click="scroll(0, -1)">‹</button>
            <span class="small range">Steps <span class="num">{{ stepsText(first, count) }}</span> <span class="of">/ {{ view.length }}</span></span>
            <button type="button" class="icon" aria-label="Later steps" :disabled="first + count >= view.length" @click="scroll(0, 1)">›</button>
          </div>
          <div class="group" role="group" aria-label="Pitches on screen">
            <span class="small range" aria-hidden="true">Oct</span>
            <button type="button" class="icon" aria-label="Show an octave higher" @click="scroll(1, 0)">▲</button>
            <button type="button" class="icon" aria-label="Show an octave lower" @click="scroll(-1, 0)">▼</button>
          </div>
          <Chip v-if="!narrow" :pressed="large" @click="large = !large">Large cells</Chip>
          <div class="group" role="group" aria-label="History">
            <button type="button" :disabled="!paired || !view.undo" @click="send(seqCmd.undo(view.track))"><span aria-hidden="true">↶ </span>Undo <span class="num">{{ view.undo || "" }}</span></button>
            <button type="button" :disabled="!paired || !view.redo" @click="send(seqCmd.redo(view.track))"><span aria-hidden="true">↷ </span>Redo <span class="num">{{ view.redo || "" }}</span></button>
          </div>
          <div class="group tools" role="group" aria-label="Tools" @keydown.esc="closeTool">
            <button
              v-for="t in TOOLS"
              :key="t.id"
              :ref="(el) => (toolBtns[t.id] = el as HTMLButtonElement | null)"
              type="button"
              class="tool"
              :aria-expanded="tool === t.id"
              :aria-controls="drawerId"
              @click="toggleTool(t.id)"
            >
              {{ t.text }} <span aria-hidden="true">{{ tool === t.id ? "▴" : "▾" }}</span>
            </button>
          </div>
        </div>
        <div :id="drawerId">
          <SeqTools v-if="tool" :tool="tool" :view="view" :selected="selected" :first="first" :count="count" :disabled="!paired" @send="send" @close="closeTool" />
        </div>

        <PianoRoll
          :notes="view.notes"
          :first="first"
          :count="count"
          :top="top"
          :rows="ROWS"
          :row-rem="big ? 2.75 : 1.5"
          :cell-rem="narrow ? 2.75 : undefined"
          :play="head"
          :draw="draw && paired"
          :selected="selected"
          :length="view.length"
          @select="select"
          @add="add"
          @remove="remove"
          @nudge="nudge"
          @vel="stepVel"
          @len="stepLen"
          @len-to="setLen"
          @scroll="scroll"
        />
        <p class="small status">
          <span class="num">{{ view.notes.length }}</span> notes.
          <template v-if="!draw"> Select mode: taps only select (Draw adds notes).</template>
          <template v-else> Draw mode: a tap on an empty cell adds a note; drag a note to move it, its end to resize.</template>
        </p>
        <p v-if="msg" class="small warm" role="status">{{ msg }}</p>

        <section class="inspector" aria-label="Selection">
          <Tabs v-model="pane" label="Edit the selection" :tabs="PANES">
            <template #before>
              <span class="what">
                <template v-if="selected"><b>{{ noteName(selected.pitch) }}</b> · step <span class="num">{{ selected.step + 1 }}</span></template>
                <template v-else>No note selected</template>
              </span>
            </template>
            <div v-if="pane === 'note'">
              <p v-if="!selected" class="small hint">Select a note in the roll (a tap, or Enter on it) to edit it here.</p>
              <div v-else class="notegrid">
                <Fader :key="'v' + selected.step + selected.pitch" label="Velocity" :model-value="selected.vel" :min="1" :max="127" :disabled="!paired" @commit="(v: number) => setVel(selected!, v)" />
                <Fader :key="'l' + selected.step + selected.pitch" label="Length" :model-value="selected.len" :min="SUB / 4" :max="SUB * 16" :step="SUB / 4" :format="lenText" :disabled="!paired" @commit="(v: number) => setLen(selected!, v)" />
                <Fader :key="'m' + selected.step + selected.pitch" label="Micro timing" :model-value="selected.micro" :min="-96" :max="96" :format="microText" :disabled="!paired" @commit="(v: number) => setMicro(selected!, v)" />
                <div class="acts" role="group" aria-label="The selected note">
                  <button type="button" aria-label="Move the note an octave up" :disabled="!paired" @click="nudge(selected, 0, 12)">Oct <span aria-hidden="true">+</span></button>
                  <button type="button" aria-label="Move the note an octave down" :disabled="!paired" @click="nudge(selected, 0, -12)">Oct <span aria-hidden="true">−</span></button>
                  <button type="button" class="danger" :disabled="!paired" @click="remove(selected)">Delete</button>
                </div>
              </div>
            </div>
            <StepEditor v-else :track="view.track + 1" :step="selected ? selected.step + 1 : null" :seqx="seqx" :length="view.length" :disabled="!paired" @send="sendLines" />
          </Tabs>
        </section>
      </div>
    </Card>

    <SeqSettings v-if="view.track >= 0" :view="view" :disabled="!paired" @send="sendLines" />
  </template>
</template>

<style scoped>
.head { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 0.5rem 1.25rem; }
.facts { display: flex; flex-wrap: wrap; gap: 0.25rem 1.25rem; color: var(--label); }
.facts b { color: var(--text); }
.group { display: inline-flex; flex-wrap: wrap; align-items: center; gap: 0.375rem; }
.editor { display: flex; flex-direction: column; gap: 0.625rem; min-width: 0; }
/* the toolbar sits on the roll: one row where there is room, groups that wrap whole on a phone */
.toolbar {
  display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem 1rem;
  padding: 0.5rem; border: 1px solid var(--border); border-radius: var(--radius-control); background: var(--bg);
}
.toolbar .tools { margin-left: auto; }
.icon { padding: 0.5rem; font-size: 1.125rem; line-height: 1; }
.range { color: var(--label); white-space: nowrap; }
.range .num { color: var(--text); font-size: 1rem; }
.tool[aria-expanded="true"] { border-color: var(--accent); border-width: 2px; background: var(--accent-tint); font-weight: 700; }
.status { margin: 0; color: var(--label); }
/* the inspector: part of the roll's card, under the velocity lane */
.inspector { border-top: 1px solid var(--border); padding-top: 0.25rem; margin-top: 0.25rem; }
.what { min-width: 8rem; color: var(--label); }
.what b { color: var(--text); font-size: 1.125rem; }
.hint { margin: 0; color: var(--label); }
.notegrid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)) auto; gap: 0.5rem 1.25rem; align-items: end; }
.acts { display: flex; flex-wrap: wrap; gap: 0.375rem; padding-bottom: 1.75rem; }
.danger { border-color: var(--warm); color: var(--warm); }
@media (max-width: 48rem) {
  .notegrid { grid-template-columns: minmax(0, 1fr); }
  .acts { padding-bottom: 0; }
  .toolbar .tools { margin-left: 0; }
}
</style>
