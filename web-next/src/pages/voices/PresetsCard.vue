<!-- Voices › presets (core/user/lists.h; the old page's Presets tab): the JUNO and 6-OP FM lists - rename, move up,
     delete, save what a voice plays now. -->
<script setup lang="ts">
import { onMounted, onUnmounted, ref } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { confirmAsk } from "../../lib/confirm";

interface Entry { slot: number; name: string; used: boolean; found?: boolean }
interface Lists { juno?: { total: number; list: Entry[] }; fm?: { total: number; rom?: boolean; list: Entry[] }; voices?: { v: number; m: string }[] }
const { api, cmd, paired } = useSidecar();
const lists = ref<Lists>({});
const err = ref("");
const editing = ref<{ k: string; slot: number; name: string } | null>(null);
let timer: ReturnType<typeof setInterval> | null = null;
async function load() {
  if (!paired.value || editing.value) return; // (not while a name is being typed)
  try {
    lists.value = await api.json<Lists>("/lists");
  } catch (e) {
    err.value = (e as Error).message;
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 2000);
});
onUnmounted(() => timer && clearInterval(timer));
async function run(c: string) {
  err.value = "";
  try {
    await cmd(c);
    setTimeout(load, 300);
  } catch (e) {
    err.value = (e as Error).message;
  }
}
async function rename() {
  const e = editing.value;
  editing.value = null;
  if (e && e.name.trim()) await run(`${e.k}=ren:${e.slot}:${e.name.trim().slice(0, 16)}`);
}
async function del(k: string, x: Entry) {
  if (await confirmAsk({ title: "Delete a preset", body: `Delete "${x.name}" from the ${k.toUpperCase()} list?`, yes: "Delete", danger: true })) run(`${k}=del:${x.slot}`);
}
const saveVoice = ref<{ juno: number; fm: number }>({ juno: -1, fm: -1 });
const saveName = ref<{ juno: string; fm: string }>({ juno: "", fm: "" });
function save(k: "juno" | "fm") {
  const v = saveVoice.value[k];
  if (v < 0) return;
  run(`${k}=save:${v}${saveName.value[k] ? `:${saveName.value[k].slice(0, 16)}` : ""}`);
  saveName.value[k] = "";
}
</script>

<template>
  <Card label="Presets">
    <p v-if="!paired">The preset lists need the pairing.</p>
    <template v-else>
      <section v-for="k in (['juno', 'fm'] as const)" :key="k" :aria-label="k === 'juno' ? 'JUNO list' : '6-OP FM list'">
        <span class="label">{{ k === "juno" ? "JUNO" : "6-OP FM" }} · PAT</span>
        <p v-if="k === 'fm' && lists.fm?.rom" class="small">The 6-OP FM list is its ROM voices (none of your own yet).</p>
        <ol class="list">
          <li v-for="(x, i) in lists[k]?.list ?? []" :key="x.slot">
            <span class="num">{{ x.slot + 1 }}</span>
            <form v-if="editing && editing.k === k && editing.slot === x.slot" class="row" @submit.prevent="rename">
              <label :for="`ren-${k}`" class="visually-hidden">New name</label>
              <input :id="`ren-${k}`" v-model="editing.name" maxlength="16" autocomplete="off" />
              <button type="submit">Save</button>
              <button type="button" @click="editing = null">Cancel</button>
            </form>
            <template v-else>
              <span class="name" :class="{ warm: x.found === false }">{{ x.name }}<template v-if="x.found === false"> (⚠ missing)</template></span>
              <button type="button" @click="editing = { k, slot: x.slot, name: x.name }">Rename</button>
              <button type="button" :disabled="i === 0" :aria-label="`Move ${x.name} up`" @click="run(`${k}=swap:${x.slot - 1}:${x.slot}`)">Up</button>
              <button type="button" @click="del(k, x)">Delete</button>
            </template>
          </li>
        </ol>
        <form class="row" @submit.prevent="save(k)">
          <label :for="`sv-${k}`">Save what</label>
          <select :id="`sv-${k}`" v-model.number="saveVoice[k]">
            <option :value="-1" disabled>a voice…</option>
            <option v-for="v in (lists.voices ?? []).filter((x) => x.m === (k === 'juno' ? 'JUNO' : 'FM'))" :key="v.v" :value="v.v">V{{ v.v + 1 }} plays</option>
          </select>
          <label :for="`sn-${k}`">as</label>
          <input :id="`sn-${k}`" v-model="saveName[k]" maxlength="16" placeholder="a name" autocomplete="off" />
          <button type="submit" :disabled="saveVoice[k] < 0">Save current</button>
        </form>
      </section>
    </template>
    <p v-if="err" class="small warm" role="status">{{ err }}</p>
  </Card>
</template>

<style scoped>
section { display: flex; flex-direction: column; gap: 0.5rem; border-top: 1px solid var(--border); padding-top: 0.75rem; }
section:first-of-type { border-top: 0; padding-top: 0; }
.list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.25rem; }
.list li { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.name { flex: 1 1 8rem; }
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
</style>
