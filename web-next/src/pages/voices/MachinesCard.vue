<!-- Voices › user machines (core/user/um.h; the old page's User machines tab): the project's slots U1..U5 (assign a
     package that fits, clear), the packages on the card, rescan. Upload and delete go through Library › Files. -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { confirmAsk } from "../../lib/confirm";

interface Row { t: string; [k: string]: any }
const { api, cmd, paired } = useSidecar();
const rows = ref<Row[]>([]);
const err = ref("");
let timer: ReturnType<typeof setInterval> | null = null;
async function load() {
  if (!paired.value) return;
  try {
    rows.value = await api.json<Row[]>("/um");
    err.value = "";
  } catch (e) {
    err.value = (e as Error).message;
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 2000); // as the old page, on this page only
});
onUnmounted(() => timer && clearInterval(timer));
const plugins = computed(() => rows.value.filter((r) => r.t === "plugin"));
const slots = computed(() => rows.value.filter((r) => r.t === "slot"));
const info = computed(() => rows.value.find((r) => r.t === "plugins"));
const SRC: Record<string, string> = { saved: "saved", sounds: "from the sounds", inherited: "inherited", imported: "imported", empty: "empty", none: "not set" };

async function run(c: string) {
  try {
    await cmd(c);
    setTimeout(load, 400);
  } catch (e) {
    err.value = (e as Error).message;
  }
}
async function clear(n: number) {
  if (await confirmAsk({ title: `Clear U${n}`, body: `U${n} plays nothing until a machine is assigned again.`, yes: "Clear" })) run(`um=clear:${n}`);
}
</script>

<template>
  <Card label="User machines">
    <p v-if="!paired">The user machines need the pairing.</p>
    <template v-else>
      <p v-if="info" class="small">Project {{ info.project }} · {{ info.n }} packages in {{ info.dir }}<template v-if="info.scanning"> · scanning…</template></p>
      <ul class="slots">
        <li v-for="s in slots" :key="s.slot">
          <span class="num id">U{{ s.slot }}</span>
          <span>
            <b>{{ s.name || "—" }}</b>
            <span class="small"> · {{ s.state === "READY" ? "ready" : s.state === "MISSING" ? "⚠ missing: not on the card" : "empty" }} · {{ SRC[s.src] ?? s.src }}</span>
          </span>
          <label :for="`u${s.slot}`" class="visually-hidden">U{{ s.slot }} machine</label>
          <select :id="`u${s.slot}`" :value="s.id" @change="run(`um=assign:${s.slot}:${($event.target as HTMLSelectElement).value}`)">
            <option value="" disabled>Assign…</option>
            <option v-for="p in plugins" :key="p.id" :value="p.id" :disabled="!p.fit">{{ p.name }} {{ p.ver }}{{ p.fit ? "" : ` (does not fit: ${p.why})` }}</option>
          </select>
          <button type="button" :disabled="!s.id" @click="clear(s.slot)">Clear</button>
        </li>
      </ul>
      <details>
        <summary>Packages on the card ({{ plugins.length }})</summary>
        <ul class="pk">
          <li v-for="p in plugins" :key="p.id"><b>{{ p.name }}</b> {{ p.id }} {{ p.ver }} · <span class="num">{{ Math.round(p.size / 1024) }}</span> KB · CPU <span class="num">{{ p.cpu }}</span> us/voice<template v-if="p.slots?.length"> · in U{{ p.slots.join(", U") }}</template></li>
        </ul>
      </details>
      <button type="button" @click="run('um=rescan')">Rescan the card</button>
    </template>
    <p v-if="err" class="small warm" role="status">{{ err }}</p>
  </Card>
</template>

<style scoped>
.slots { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.375rem; }
.slots li { display: flex; flex-wrap: wrap; align-items: center; gap: 0.25rem 0.75rem; border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.375rem 0.625rem; }
.slots li > span:nth-child(2) { flex: 1 1 10rem; }
.id { font-size: 1.125rem; }
.pk { margin: 0.5rem 0 0; padding-left: 1.25rem; }
button { align-self: flex-start; }
</style>
