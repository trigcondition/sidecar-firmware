<!-- web-next/src/pages/mix/SidechainTab.vue - Mix > Sidechain: a gain stage on any insert chain (a voice, a bus, the master), after its two
     slots and before its POST EQ. PUMP is a trig-driven gain envelope; DUCK is the insert COMP keyed by a source. The source is a
     Rytm track's stem, the audio in, or a Sidecar voice. Stems exist only in Overbridge mode: without them DUCK does nothing (PUMP on a track runs on its MIDI trigs),
     and the page says so. -->
<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Fader from "../../ui/Fader.vue";
import Segmented from "../../ui/Segmented.vue";
import { SC_CHAINS, SC_MODES, SC_SOURCES, mixCmd, monLease, pumpPath, scText, sidechains, type ScMode } from "../../lib/mix";
import { useSidecar } from "../../lib/useSidecar";
import { useSend } from "../../lib/useSend";

const { status, paired } = useSidecar();
const send = useSend();
const chain = ref("V1");
const sc = computed(() => sidechains(status.value as Record<string, any> | null));
const cur = computed(() => sc.value.list[Math.max(0, SC_CHAINS.indexOf(chain.value))]);
const on = computed(() => sc.value.list.filter((x) => x.mode !== "OFF").map((x) => x.id));
const chainName = (id: string) => (id[0] === "V" ? `Voice ${id[1]}` : id[0] === "B" ? `Bus ${id[1]}` : `Master chain ${id[1]}`);
const ok = computed(() => cur.value.mode === "OFF" || cur.value.status === "OK");
const why = computed(() => {
  const st = cur.value.status;
  if (st === "NO STEMS") return "No stems: the Rytm's tracks reach the Sidecar only over Overbridge (the stem layout), so this stage does nothing until they arrive.";
  if (st === "NO AUDIO IN") return "No audio in: it comes from the Rytm over Overbridge, so this stage does nothing until it arrives.";
  if (st === "SEND FX OFF") return "Send FX off: this bus's chain runs inside its send FX's return, so this stage does nothing while that send FX is OFF.";
  return `Does nothing now: ${st}.`;
});
const hint = computed(() =>
  cur.value.mode === "PUMP"
    ? "Each trig of the source starts the envelope. An audio in has no trigs: its level onset above THR does."
    : "The insert COMP, keyed by the source's level: the chain is turned down while the source is over THR. KBAS and KWID shape the key: a high pass at KBAS (18 dB per octave) and a low pass KWID octaves above it (12 dB per octave); MON lets you hear it.",
);
const path = computed(() => pumpPath(cur.value.p, 360, 90));

// MON (0.5.2): the chain plays its filtered key instead of its output. Not saved; it ends with the page or the chain shown.
const monOn = computed(() => sc.value.list.filter((x) => x.mon).map((x) => x.id));
const lease = monLease(send, (id) => monOn.value.includes(id));
const monOff = (ids: string[]) => ids.forEach((id) => lease.off(id));
watch(chain, (_n, old) => monOff(monOn.value.filter((id) => id === old)));
// The Sidecar drops a MON from the web after 3 s unless it is renewed (a closed tab, a lost link): the lease renews it every second.
const toggleMon = (id: string, on: boolean) => lease.set(id, on);
onMounted(() => lease.start());
onBeforeUnmount(() => {
  lease.stop();
  monOff(monOn.value);
});
</script>

<template>
  <p v-if="status && !sc.supported" class="card">This Sidecar's build has no sidechain.</p>
  <Card v-else label="Sidechain">
    <div class="row">
      <label for="sc-chain" class="label">Chain</label>
      <select id="sc-chain" v-model="chain">
        <option v-for="id in SC_CHAINS" :key="id" :value="id">{{ chainName(id) }}{{ on.includes(id) ? " (on)" : "" }}</option>
      </select>
    </div>
    <p v-if="on.length" class="small">On: {{ on.join(", ") }}</p>

    <span class="label">Mode</span>
    <Segmented
      label="Sidechain mode"
      :options="SC_MODES.map((m) => ({ value: m, text: m === 'OFF' ? 'Off' : m === 'PUMP' ? 'Pump' : 'Duck' }))"
      :model-value="cur.mode"
      :disabled="!paired"
      @update:model-value="(v: ScMode) => send(mixCmd.scMode(cur.id, v))"
    />

    <template v-if="cur.mode !== 'OFF'">
      <div class="row">
        <label for="sc-src" class="label">Source</label>
        <select id="sc-src" :value="cur.src" :disabled="!paired" @change="send(mixCmd.scSrc(cur.id, ($event.target as HTMLSelectElement).value))">
          <optgroup v-for="g in SC_SOURCES" :key="g.group" :label="g.group">
            <option v-for="o in g.items" :key="o.value" :value="o.value">{{ o.text }}</option>
          </optgroup>
        </select>
      </div>
      <p v-if="!ok" class="warn" role="status">{{ why }}</p>
      <p class="small">{{ hint }}</p>
      <svg v-if="cur.mode === 'PUMP'" viewBox="0 0 360 90" class="env" role="img" aria-label="The pump envelope: the chain's gain after a trig">
        <line x1="0" y1="1" x2="360" y2="1" class="ref" />
        <path :d="path" class="line" />
      </svg>
      <div v-if="cur.mode === 'DUCK'" class="row">
        <button
          type="button"
          class="pill"
          :aria-pressed="cur.mon"
          :disabled="!paired"
          @click="toggleMon(cur.id, !cur.mon)"
        >
          Monitor the key (MON)
        </button>
        <span class="small">{{ cur.mon ? "You hear the filtered key instead of this chain. It switches off when you leave this page, and about 3 seconds after the page stops sending it." : "Plays the filtered key in place of the chain, to set KBAS and KWID by ear." }}</span>
      </div>
      <div class="faders">
        <template v-for="(n, k) in cur.names.slice(0, 6)" :key="`${cur.mode}${k}`">
          <Fader
            v-if="n !== '-'"
            :label="n"
            :model-value="cur.p[k]"
            :format="(v: number) => scText(cur.mode, k, v)"
            :disabled="!paired"
            @commit="(v: number) => send(mixCmd.scParam(cur.id, k, v))"
          />
        </template>
      </div>
      <button type="button" :disabled="!paired" @click="send(mixCmd.scMode(cur.id, 'OFF'))">Sidechain off</button>
    </template>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.faders { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 12rem), 1fr)); gap: 0.75rem 1.5rem; margin: 0.75rem 0; }
.warn { margin: 0.5rem 0; }
.env { display: block; width: 100%; max-width: 30rem; height: auto; }
.env .ref { stroke: var(--border); stroke-width: 1; }
.env .line { fill: none; stroke: var(--accent); stroke-width: 2.5; }
:root[data-theme="contrast"] .env .line { stroke: var(--text); }
</style>
