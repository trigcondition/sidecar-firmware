<!-- web-next/src/pages/mix/MixerTab.vue - Mix > Mixer, as the approved sketch: 12 strips (V1..V8 voices, B1..B3 buses,
     M master) with their POST EQ curves, inserts, meters (the "levels" topic) and levels; the selected strip's detail
     below. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import StripView from "./StripView.vue";
import StripDetail from "./StripDetail.vue";
import { mixCmd, strips } from "../../lib/mix";
import { useSidecar } from "../../lib/useSidecar";
import { useSend } from "../../lib/useSend";
import { dbText } from "../../lib/status";

const { status, levels, paired } = useSidecar();
const send = useSend();
const st = computed(() => (status.value ?? null) as Record<string, any> | null);
const all = computed(() => strips(st.value, levels.value?.strips));
const sel = ref(0);
const selected = computed(() => all.value[sel.value]);
const mfx = computed(() => st.value?.ins?.mfx as Record<string, any> | undefined);
const hasIns = computed(() => !!st.value?.ins);
const master = computed(() => (levels.value ? Math.max(...levels.value.master) : null));
// a strip's fader: a voice's OUT level, the master's MFX level, none for a bus
const faderOf = (i: number) => (i < 8 ? all.value[i].out?.lev ?? null : i === 11 ? mfx.value?.lev ?? null : null);
function setLevel(i: number, v: number) {
  send(i < 8 ? mixCmd.out(all.value[i], "lev", v) : mixCmd.mfxLevel(v));
}
</script>

<template>
  <p v-if="st && !hasIns" class="card">This Sidecar's build reports no mixer (no "ins" in its status).</p>
  <Card label="Mixer">
    <p class="small">Master <span class="num accent">{{ master === null ? "–" : dbText(master) }}</span> · a strip opens its EQ, inserts and sends below.</p>
    <div class="scroller">
      <div class="strips" role="group" aria-label="Strips">
        <StripView
          v-for="(s, i) in all"
          :key="s.id"
          :strip="s"
          :selected="sel === i"
          :level="faderOf(i)"
          :disabled="!paired"
          @select="sel = i"
          @level="(v: number) => setLevel(i, v)"
        />
      </div>
    </div>
  </Card>
  <StripDetail v-if="selected" :strip="selected" :disabled="!paired" @cmd="send" />
</template>

<style scoped>
.scroller { overflow-x: auto; padding-bottom: 0.25rem; }
.strips { display: grid; grid-template-columns: repeat(12, minmax(4.5rem, 1fr)); gap: 0.5rem; min-width: 56rem; }
</style>
