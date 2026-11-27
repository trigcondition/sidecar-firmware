<!-- web-next/src/pages/Loop.vue - the master looper (0.5.0, core/loop/loop.h; the touchscreen's LOOP view): six grab keys (the last
     1/2..16 bars, on the last bar line; while a loop plays a grab is a resample), STOP and CUT, the eight grabs as pads (a tap plays
     one from the next bar line; KEEP writes the one shown to the card as a take), BAL and LEV, FADE, auto-mute, LIVE VIA and the POST
     calibration. Each action is a console line (loop=...); the state comes back on /status "loop". A build without the looper (0.4.x)
     says so; a looper that is off (loopram=0, or no RAM for it) says how to turn it on. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../ui/Card.vue";
import Fader from "../ui/Fader.vue";
import { coalescer } from "../lib/coalesce";
import { notify } from "../lib/notify";
import { useSidecar } from "../lib/useSidecar";
import { FADES, LENGTHS, STATES, balGains, balLine, calLine, fadeLine, grabLine, keepLine, levLine, liveLine, livePercent, loopView, muteLine, playLine, positionText, stopLine } from "../lib/loop";

const { status, cmd, paired } = useSidecar();
const lp = computed(() => loopView(status.value as Record<string, unknown> | null));
const fail = (e: unknown) => notify.error(e);
async function send(line: string): Promise<boolean> {
  try {
    await cmd(line);
    return true;
  } catch (e) {
    fail(e);
    return false;
  }
}
const live = coalescer(cmd, fail); // BAL and LEV while they move: the newest wins

// the looper's own words (a refusal "PLAY FIRST", "16 BARS NEEDS 47 BPM", a keep's take): a toast each time it says a new one
watch(() => lp.value?.msgN, (n, was) => {
  const m = lp.value?.msg;
  if (n && n !== was && m) (/^(KEPT|POST LAT)/.test(m) ? notify.ok : notify.warn)(m);
});

const selId = ref(0); // the grab shown, by id: a newer grab landing moves the list, not the choice (0: the newest)
const sel = computed(() => Math.max(0, lp.value?.grabs.findIndex((g) => g.id === selId.value) ?? 0));
const grab = computed(() => lp.value?.grabs[sel.value]);
const playingNow = computed(() => lp.value?.state === "playing" || lp.value?.state === "wait" || lp.value?.state === "fading");
const stateText = computed(() => {
  const v = lp.value;
  if (!v) return "";
  if (v.pending) return playingNow.value ? "Resample: waiting for the bar's audio" : "Grab: waiting for the bar's audio";
  const p = positionText(v);
  return `${STATES[v.state] ?? v.state}${p ? " · " + p : ""}`;
});
function pick(id: number) {
  selId.value = id;
  void send(playLine(id));
}
const bal = computed(() => lp.value?.bal ?? 64);
const gains = computed(() => balGains(bal.value));
const livePerf = ref(1);
watch(() => lp.value?.live, (m) => { if (m) livePerf.value = m; }, { immediate: true });
</script>

<template>
  <h1 class="visually-hidden" tabindex="-1">Loop</h1>
  <Card v-if="!lp" label="Loop">
    <p>The master looper comes with Sidecar 0.5.2. This Sidecar does not report one.</p>
  </Card>
  <Card v-else-if="!lp.on" label="Loop">
    <p>The looper is off on this Sidecar: <code>loopram=0</code> is set, or there was no RAM for it at boot. A 1 GB board keeps it off by default (it takes 80 MB from the sample pool); <code>loopram=48</code> in <code>cmdline.txt</code> turns it on.</p>
  </Card>
  <template v-else>
    <Card label="Loop">
      <p class="state" role="status">{{ stateText }}</p>
      <p class="small">The ring holds the last <span class="num">{{ lp.ringSeconds }}</span> s of the Rytm's main output, loop and live together. A grab takes the last bars up to the last bar line and plays them from the next one.</p>
      <div class="row" role="group" aria-label="Grab the last bars">
        <button v-for="l in LENGTHS" :key="l.hb" type="button" class="len" :disabled="!paired" :aria-label="`Grab the last ${l.label} ${l.hb > 2 ? 'bars' : 'bar'}`" @click="send(grabLine(l.hb))">
          <span class="num">{{ l.label }}</span>
        </button>
        <span class="gap" aria-hidden="true"></span>
        <button type="button" :disabled="!paired || lp.state === 'ring'" @click="send(stopLine())">Stop <span class="small">(fade, next bar line)</span></button>
        <button type="button" class="danger" :disabled="!paired || lp.state === 'ring'" @click="send(stopLine(true))">Cut <span class="small">(at once)</span></button>
      </div>
      <p v-if="lp.pending" class="small">Waiting for the bar's audio to arrive from the Rytm (a few milliseconds).</p>
    </Card>

    <Card :label="`Grabs · ${lp.grabs.length} of 8`">
      <p v-if="!lp.grabs.length">No grabs yet. Press a length above while the Rytm plays.</p>
      <div v-else class="pads" role="radiogroup" aria-label="Grab">
        <button
          v-for="(g, i) in lp.grabs"
          :key="g.id"
          type="button"
          role="radio"
          class="pad"
          :class="{ sel: i === sel, playing: g.playing, kept: g.kept }"
          :aria-checked="i === sel"
          :aria-label="`Grab ${i + 1}: ${g.label} ${g.label === '1' ? 'bar' : 'bars'} at ${Math.round(g.bpm)} BPM${g.kept ? ', kept' : ''}${g.playing ? ', playing' : ''}`"
          :disabled="!paired"
          @click="pick(g.id)"
        >
          <span class="num pn">{{ g.label }}</span>
          <span class="small">{{ Math.round(g.bpm) }} BPM</span>
          <span v-if="g.playing || g.kept || g.gap" class="badge" aria-hidden="true">{{ g.playing ? "▶" : "" }}{{ g.kept ? "●" : "" }}{{ g.gap ? "!" : "" }}</span>
        </button>
      </div>
      <div class="row">
        <button type="button" :disabled="!paired || !grab || grab.kept || !grab.copied" @click="grab && send(keepLine(grab.id))">
          Keep grab {{ sel + 1 }} on the card
        </button>
        <span v-if="grab?.kept" class="small">Kept: it is in Rec › Takes.</span>
        <span v-else-if="grab && !grab.copied" class="small">Still copying…</span>
        <span v-if="grab?.gap" class="small warm">The Rytm's audio had a gap in this one.</span>
      </div>
    </Card>

    <Card label="Loop and live">
      <Fader label="Balance (BAL): live ← → loop" :model-value="lp.bal" :min="0" :max="127" :disabled="!paired" @update:model-value="(v: number) => live(balLine(v))" @commit="(v: number) => live(balLine(v))" />
      <p class="small">
        Loop <span class="num">{{ Math.round(gains.loop * 100) }} %</span> · the Sidecar's own output <span class="num">{{ Math.round(gains.own * 100) }} %</span>. The Rytm's own tracks go down by auto-mute or LIVE VIA below.
      </p>
      <Fader label="Loop level (LEV, 100 = as grabbed)" :model-value="lp.lev" :min="0" :max="127" :disabled="!paired" @update:model-value="(v: number) => live(levLine(v))" @commit="(v: number) => live(levLine(v))" />
      <div class="row">
        <label for="fade" class="label">Fade back</label>
        <select id="fade" :value="lp.fade" :disabled="!paired" @change="send(fadeLine(Number(($event.target as HTMLSelectElement).value)))">
          <option v-for="(f, i) in FADES" :key="f" :value="i">{{ f }} bar{{ f === "1" ? "" : "s" }}</option>
        </select>
        <label class="check"><input type="checkbox" :checked="lp.mute" :disabled="!paired" @change="send(muteLine(($event.target as HTMLInputElement).checked))" /> Auto-mute the Rytm's tracks while a loop plays</label>
      </div>
      <div class="row">
        <label for="live" class="label">Live via</label>
        <select id="live" :value="lp.live" :disabled="!paired" @change="send(liveLine(Number(($event.target as HTMLSelectElement).value)))">
          <option :value="0">Off</option>
          <option v-for="m in 12" :key="m" :value="m">PERF {{ m }}</option>
        </select>
        <span v-if="lp.live" class="small">PERF {{ lp.live }} gets <span class="num">{{ livePercent(bal) }} %</span> at this balance; its depths on the Rytm say how far the live tracks go down.</span>
      </div>
    </Card>

    <Card label="Timing">
      <p class="small">
        The loop starts on the bar line the sequencer's clock gives (the Rytm must play). POST latency: <span class="num">{{ lp.postLat }}</span> frames beyond the voice path.
      </p>
      <div class="row">
        <button type="button" :disabled="!paired || lp.calibrating" @click="send(calLine)">{{ lp.calibrating ? "Listening for the clicks…" : "Calibrate POST latency" }}</button>
      </div>
    </Card>
  </template>
</template>

<style scoped>
.state { font-size: 1.25rem; margin: 0 0 0.25rem; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; margin-top: 0.75rem; }
.gap { flex: 1 1 1rem; }
.len { min-width: 3.5rem; }
.len .num { font-size: 1.25rem; }
.pads { display: grid; grid-template-columns: repeat(auto-fill, minmax(5.5rem, 1fr)); gap: 0.5rem; }
.pad { position: relative; display: flex; flex-direction: column; align-items: center; gap: 0.125rem; padding: 0.625rem 0.25rem; min-height: 4.5rem; }
.pad .pn { font-size: 1.5rem; }
.pad.sel { border: 2px solid var(--accent); background: var(--accent-tint); }
.pad.playing { box-shadow: inset 0 -3px 0 var(--accent); }
.badge { position: absolute; top: 0.25rem; right: 0.375rem; font-weight: 700; color: var(--accent); font-size: var(--text-min); }
.check { display: inline-flex; align-items: center; gap: 0.375rem; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
.danger { border-color: var(--warm); color: var(--warm); }
</style>
