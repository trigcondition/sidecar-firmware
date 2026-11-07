<!-- Seq › the shown track's and the pattern's settings in one panel of tabs (the old page's Sound › Poly tracks and
     Sequencer forms): the track (swing, speed, direction, random notes, seed mod), its poly voices, the pattern (key,
     scale, quantise, seed). Each group goes out with its own button, as the command lines need: the track's form is a
     batch of lines (lib/seqx.ts trackLines), poly one line; swing goes out when the fader is let go. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Fader from "../../ui/Fader.vue";
import Segmented from "../../ui/Segmented.vue";
import Tabs from "./Tabs.vue";
import { AFFECTS, RND, SEED_QUANT, WEIGHT, seedLine, trackLines, type TrackSettings } from "../../lib/seqx";
import { DIR, KEYS, POLY_ALLOC, QNT, SCALES, SPD, cmd as seqCmd, trackName, type SeqView } from "../../lib/seq";
import { useSidecar } from "../../lib/useSidecar";

const props = defineProps<{ view: SeqView; disabled: boolean }>();
const emit = defineEmits<{ send: [string[]] }>();
const { status } = useSidecar();
const T = computed(() => props.view.track);
const tab = ref<"track" | "poly" | "pattern">("track");
const tabs = computed(() => [
  { id: "track" as const, text: trackName(T.value) },
  { id: "poly" as const, text: "Poly" },
  { id: "pattern" as const, text: "Pattern" },
]);

// the track: swing (the view's), speed, direction, random notes and seed mod (seqtop, /status "seqx")
const swing = ref(50);
watch(() => props.view.swing, (s) => typeof s === "number" && (swing.value = s), { immediate: true });
const seqx = computed(() => (status.value as Record<string, any> | null)?.seqx as Record<string, any> | undefined);
const sxRow = computed(() => seqx.value?.t?.find((t: any) => t.t === T.value + 1));
const trk = ref<TrackSettings>({ spd: 0, dir: 0, rm: 0, rl: 36, ro: 1, rw: 0, ms: 0, mr: 8, mq: 0, ma: 0 });
const trkForm = ref<HTMLElement | null>(null);
watch([sxRow, () => props.view], () => {
  if (trkForm.value?.contains(document.activeElement)) return; // (not while a field is being edited)
  const r = sxRow.value, v = props.view;
  if (r) trk.value = { spd: r.spd, dir: r.dir, rm: r.rnd[0], rl: r.rnd[1], ro: r.rnd[2] || 1, rw: r.rnd[3], ms: r.smod[0], mr: r.smod[1] || 8, mq: r.smod[2], ma: r.smod[3] };
  else if (v) trk.value = { ...trk.value, spd: v.speed, dir: v.dir };
}, { immediate: true });

// poly: the view's voices, allocation and unison; detune and spread from /status "poly"
const polyRow = computed(() => {
  const p = (status.value as Record<string, any> | null)?.poly;
  return Array.isArray(p) ? p.find((x: any) => x.t === T.value + 1) : undefined;
});
const poly = ref({ voices: 0, alloc: 0, unison: 1, detune: 0, spread: 0 });
watch([() => props.view.poly, polyRow], () => {
  const v = props.view.poly, r = polyRow.value;
  if (v) poly.value = { voices: v.voices, alloc: v.alloc, unison: Math.max(1, v.unison), detune: r?.d ?? poly.value.detune, spread: r?.s ?? poly.value.spread };
}, { immediate: true });
const inst = computed(() => (status.value as Record<string, any> | null)?.inst as Record<string, any> | undefined);
const instText = computed(() => {
  const i = inst.value;
  if (!i) return "";
  const used = (i.v ?? []).reduce((a: number, x: any) => a + (x.path === "inst" ? x.on : 0), 0);
  return `Instances ${used} of ${i.total}, ${Math.round(i.bytes / 1024)} KB each${i.noroom ? `, ${i.noroom} notes found no free instance` : ""}`;
});

// the pattern: key, scale, quantise; the seed
const pat = ref({ key: 0, scale: 0, qnt: 0 });
watch(() => [props.view.key, props.view.scale, props.view.qnt], () => {
  pat.value = { key: props.view.key, scale: props.view.scale, qnt: props.view.qnt };
}, { immediate: true });
const seed = ref(0), seedFree = ref(true);
watch(seqx, (x) => { if (x && document.activeElement?.id !== "seed") { seed.value = x.seed ?? 0; seedFree.value = !x.fixed; } }, { immediate: true });
const opts = (names: string[]) => names.map((n, i) => ({ value: i, text: n }));
</script>

<template>
  <Card label="Settings">
    <Tabs v-model="tab" label="Settings" :tabs="tabs">
      <div v-if="tab === 'track'" class="panel">
        <Fader v-model="swing" class="wide" label="Swing" :min="50" :max="80" unit="%" :disabled="disabled" @commit="(v: number) => emit('send', [seqCmd.swing(T, v)])" />
        <p class="small note">Swing goes out when the fader is let go; the rest with the button below.</p>
        <form ref="trkForm" class="form" @submit.prevent="emit('send', trackLines(T + 1, trk, SPD, DIR))">
          <label for="spd" class="label">Speed</label>
          <div class="ctl"><select id="spd" v-model.number="trk.spd"><option v-for="(s, i) in SPD" :key="s" :value="i">{{ s }}</option></select></div>
          <label for="dir" class="label">Direction</label>
          <div class="ctl"><select id="dir" v-model.number="trk.dir"><option v-for="(d, i) in DIR" :key="d" :value="i">{{ d }}</option></select></div>
          <label for="rm" class="label">Random notes</label>
          <div class="ctl">
            <select id="rm" v-model.number="trk.rm"><option v-for="(r, i) in RND" :key="r" :value="i">{{ r }}</option></select>
            <template v-if="trk.rm">
              <label class="sub">low note <input id="rl" v-model.number="trk.rl" type="number" min="0" max="127" /></label>
              <label class="sub">octaves <input id="ro" v-model.number="trk.ro" type="number" min="1" max="4" /></label>
              <label class="sub">weight <select id="rw" v-model.number="trk.rw"><option v-for="(w, i) in WEIGHT" :key="w" :value="i">{{ w }}</option></select></label>
            </template>
          </div>
          <label for="ms" class="label">Seed mod</label>
          <div class="ctl">
            <label class="sub">source P (0 off) <input id="ms" v-model.number="trk.ms" type="number" min="0" max="16" /></label>
            <template v-if="trk.ms">
              <label class="sub">range <input id="mr" v-model.number="trk.mr" type="number" min="2" max="64" /></label>
              <label class="sub">quantise <select id="mq" v-model.number="trk.mq"><option v-for="(q, i) in SEED_QUANT" :key="q" :value="i">{{ q }}</option></select></label>
              <label class="sub">affects <select id="ma" v-model.number="trk.ma"><option v-for="(a, i) in AFFECTS" :key="a" :value="i">{{ a }}</option></select></label>
              <span v-if="sxRow" class="small">seed +{{ sxRow.off }}</span>
            </template>
          </div>
          <div class="act"><button type="submit" class="primary" :disabled="disabled">Set {{ trackName(T) }}</button></div>
        </form>
      </div>

      <div v-else-if="tab === 'poly'" class="panel">
        <form class="form" @submit.prevent="emit('send', [seqCmd.poly(T, poly)])">
          <label for="pv" class="label">Voices</label>
          <div class="ctl"><select id="pv" v-model.number="poly.voices"><option v-for="v in 9" :key="v" :value="v - 1">{{ v === 1 ? "AUTO" : v - 1 }}</option></select></div>
          <label for="pa" class="label">Allocation</label>
          <div class="ctl"><select id="pa" v-model.number="poly.alloc"><option v-for="(a, i) in POLY_ALLOC" :key="a" :value="i">{{ a }}</option></select></div>
          <label for="pu" class="label">Unison</label>
          <div class="ctl"><select id="pu" v-model.number="poly.unison"><option v-for="u in 8" :key="u" :value="u">{{ u === 1 ? "OFF" : u }}</option></select></div>
          <label for="pd" class="label">Detune</label>
          <div class="ctl"><input id="pd" v-model.number="poly.detune" type="number" min="0" max="127" /></div>
          <label for="ps" class="label">Spread</label>
          <div class="ctl"><input id="ps" v-model.number="poly.spread" type="number" min="0" max="127" /></div>
          <div class="act"><button type="submit" class="primary" :disabled="disabled">Set poly</button></div>
        </form>
        <p v-if="view.poly.info" class="small note">{{ view.poly.info }}</p>
        <p v-if="instText" class="small note">{{ instText }}</p>
      </div>

      <div v-else class="panel">
        <form class="form" @submit.prevent="emit('send', [seqCmd.key(pat.key, pat.scale, pat.qnt)])">
          <label for="key" class="label">Key</label>
          <div class="ctl"><select id="key" v-model.number="pat.key"><option v-for="(k, i) in KEYS" :key="k" :value="i">{{ k }}</option></select></div>
          <label for="scale" class="label">Scale</label>
          <div class="ctl"><select id="scale" v-model.number="pat.scale"><option v-for="(s, i) in SCALES" :key="s" :value="i">{{ s }}</option></select></div>
          <span class="label">Quantise</span>
          <div class="ctl"><Segmented label="Quantise to the scale" :options="opts(QNT)" :model-value="pat.qnt" @update:model-value="(v: number) => (pat.qnt = v)" /></div>
          <div class="act"><button type="submit" class="primary" :disabled="disabled">Set key and scale</button></div>
        </form>
        <form class="form sep" @submit.prevent="emit('send', [seedLine(seed, seedFree)])">
          <label for="seed" class="label">Seed</label>
          <div class="ctl">
            <input id="seed" v-model.number="seed" type="number" min="0" max="9999" :disabled="seedFree" />
            <label class="check"><input v-model="seedFree" type="checkbox" /> Free (a new one each loop)</label>
          </div>
          <div class="act"><button type="submit" class="primary" :disabled="disabled">Set seed</button></div>
        </form>
      </div>
    </Tabs>
  </Card>
</template>

<style scoped>
.panel { display: flex; flex-direction: column; gap: 0.75rem; max-width: 44rem; }
.wide { max-width: 28rem; }
.note { margin: 0; color: var(--label); }
/* one label column, one control column: every row lines up */
.form { display: grid; grid-template-columns: 9rem minmax(0, 1fr); align-items: start; gap: 0.5rem 1rem; }
.form > .label { margin: 0; min-height: var(--target); display: flex; align-items: center; }
.ctl { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem 1rem; min-width: 0; }
.act { grid-column: 2; }
.sep { padding-top: 0.75rem; border-top: 1px solid var(--border); }
.sub { display: inline-flex; align-items: center; gap: 0.5rem; color: var(--label); }
input[type="number"] { width: 5.5rem; }
.primary { border-color: var(--accent); border-width: 2px; font-weight: 700; }
.check { display: inline-flex; align-items: center; gap: 0.375rem; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
@media (max-width: 30rem) {
  .form { grid-template-columns: minmax(0, 1fr); gap: 0.25rem; }
  .ctl { margin-bottom: 0.5rem; }
  .act { grid-column: 1; }
}
</style>
