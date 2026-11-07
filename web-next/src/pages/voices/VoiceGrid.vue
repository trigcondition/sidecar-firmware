<!-- Voices › the eight voices: each one's machine (notemach=V:NAME, the old page's Sound tab), and the drone. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import SaveState from "../../ui/SaveState.vue";
import { useSetting } from "../../lib/pending";
import { useSidecar } from "../../lib/useSidecar";

const MACHINES = ["plaits", "juno", "daisy", "dsps", "dd", "fm"];
const TRACKS = ["BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"];
const { status, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const mach = TRACKS.map((_, v) =>
  useSetting<string>(status, { read: () => (st.value.machines?.[v] as string | undefined)?.toLowerCase(), toCmd: (m) => `notemach=${v}:${m}`, send: cmd }),
);
const drone = useSetting<number>(status, { read: () => st.value.drone, toCmd: (d) => (d < 0 ? "drone=off" : d === 8 ? "drone=all" : `drone=${d}`), send: cmd });
const sel = ref(0);
</script>

<template>
  <Card label="Voices">
    <ul class="voices">
      <li v-for="(t, v) in TRACKS" :key="t" :class="{ sel: sel === v }">
        <span class="num id">V{{ v + 1 }}</span>
        <span class="label">{{ t }}</span>
        <label :for="`m${v}`" class="visually-hidden">V{{ v + 1 }} machine</label>
        <select :id="`m${v}`" :value="mach[v].value.value ?? ''" :disabled="!paired" @change="mach[v].set(($event.target as HTMLSelectElement).value)" @focus="sel = v">
          <option v-if="!MACHINES.includes(mach[v].value.value ?? '')" :value="mach[v].value.value ?? ''">{{ (mach[v].value.value ?? "–").toUpperCase() }}</option>
          <option v-for="m in MACHINES" :key="m" :value="m">{{ m.toUpperCase() }}</option>
        </select>
        <SaveState :state="mach[v].state.value" :msg="mach[v].msg.value" />
      </li>
    </ul>
    <div class="row">
      <label for="drone" class="label">Drone</label>
      <select id="drone" :value="drone.value.value ?? -1" :disabled="!paired" @change="drone.set(+($event.target as HTMLSelectElement).value)">
        <option :value="-1">Off</option>
        <option v-for="(t, v) in TRACKS" :key="t" :value="v">V{{ v + 1 }} {{ t }}</option>
        <option :value="8">All voices</option>
      </select>
      <SaveState :state="drone.state.value" :msg="drone.msg.value" />
    </div>
  </Card>
</template>

<style scoped>
.voices { list-style: none; margin: 0; padding: 0; display: grid; grid-template-columns: repeat(auto-fill, minmax(10rem, 1fr)); gap: 0.5rem; }
.voices li { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem; display: flex; flex-direction: column; gap: 0.25rem; }
.voices li.sel { border-color: var(--accent); }
.id { font-size: 1.25rem; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
</style>
