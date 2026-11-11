<!-- web-next/src/pages/mix/StripView.vue - one mixer strip as the approved sketch: its id, machine, the POST EQ's curve,
     its inserts, a dotted meter, the level fader and the level as a number. The whole strip is a button that selects it
     (its detail opens below the mixer); the fader is its own control. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Meter from "../../ui/Meter.vue";
import { eqPath, eqText, insertText, type Strip } from "../../lib/mix";

const props = defineProps<{ strip: Strip; selected: boolean; level: number | null; disabled: boolean }>();
const emit = defineEmits<{ select: []; level: [number] }>();
const fader = ref(props.level ?? 0);
let dragging = false;
watch(() => props.level, (v) => { if (!dragging && v != null) fader.value = v; });
function moved() { dragging = true; }
function letGo() { dragging = false; emit("level", fader.value); }
const curve = computed(() => eqPath(props.strip.peq, 60, 24));
const eq = computed(() => eqText(props.strip.peq));
</script>

<template>
  <div class="strip" :class="{ sel: selected }">
    <button type="button" class="head" :aria-pressed="selected" :aria-label="`${strip.id} ${strip.name}: inserts ${insertText(strip)}, EQ ${eq}`" @click="emit('select')">
      <span class="id num">{{ strip.id }}</span>
      <span class="mach">{{ strip.name }}</span>
      <svg viewBox="0 0 60 24" class="eq" aria-hidden="true"><path :d="curve" /></svg>
      <span class="ins">{{ insertText(strip) }}</span>
    </button>
    <div class="body">
      <Meter :level="strip.level ?? 0" height="8rem" />
      <input
        v-if="level !== null"
        v-model.number="fader"
        type="range"
        min="0"
        max="127"
        class="vfader"
        :disabled="disabled"
        :aria-label="`${strip.id} level`"
        :aria-valuetext="`${strip.id} level ${fader}`"
        @input="moved"
        @change="letGo"
      />
    </div>
    <span class="num lev" aria-hidden="true">{{ level !== null ? fader : strip.level !== null ? Math.round((strip.level ?? 0) * 127) : "–" }}</span>
  </div>
</template>

<style scoped>
.strip { display: flex; flex-direction: column; gap: 0.5rem; padding: 0.5rem 0.375rem; border-radius: 0.75rem; border: 1px solid var(--border); min-width: 0; }
.strip.sel { border: 2px solid var(--accent); background: var(--accent-tint); }
.head { display: flex; flex-direction: column; align-items: stretch; gap: 0.375rem; border: 0; background: transparent; padding: 0; min-width: 0; text-align: center; }
.id { font-size: 1.25rem; }
.sel .id { color: var(--accent); }
.mach { font-size: var(--text-min); color: var(--label); overflow-wrap: anywhere; min-height: 2.4em; }
.eq { width: 100%; height: 1.5rem; }
.eq path { fill: none; stroke: var(--accent); stroke-width: 2; vector-effect: non-scaling-stroke; }
.ins { font-size: var(--text-min); border: 1px solid var(--control-border); border-radius: 0.375rem; padding: 0.125rem 0.25rem; overflow-wrap: anywhere; }
.body { display: flex; justify-content: center; align-items: flex-end; gap: 0.5rem; height: 8rem; }
.vfader { writing-mode: vertical-lr; direction: rtl; width: var(--target); height: 8rem; min-height: 0; padding: 0; border: 0; background: transparent; accent-color: var(--text); }
.lev { text-align: center; font-size: 1.0625rem; }
</style>
