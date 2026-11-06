<!-- Rec › the takes (/Recordings on the card and each mounted drive's, through /fs): newest first or by name, play a
     channel on the Rytm's MAIN (play=start:<take>/<nn>; not while recording), download its files, delete it. -->
<script setup lang="ts">
import { computed, onMounted, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import { confirmAsk } from "../../lib/confirm";
import { createFs, hb, saveBlob, type FsEntry } from "../../lib/fs";
import { hms, natCmp, recCmd, takeFile, takeRoots } from "../../lib/rec";
import { useSidecar } from "../../lib/useSidecar";

interface Take { id: string; name: string; date: string; vol: string; dir: string; files: FsEntry[]; size: number }
const { status, api, cmd, paired } = useSidecar();
const fs = createFs(api);
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const takes = ref<Take[]>([]);
const loading = ref(false);
const msg = ref("");
const sort = ref<"t" | "n">("t");
try { if (localStorage.getItem("sidecar.recsort")?.includes('"n"')) sort.value = "n"; } catch { /* storage blocked */ }
watch(sort, (k) => { try { localStorage.setItem("sidecar.recsort", JSON.stringify({ k, d: k === "t" ? -1 : 1 })); } catch { /* not kept */ } });

async function load() {
  if (!paired.value) return;
  loading.value = true;
  try {
    const list: Take[] = [];
    for (const [vol, base, pre] of takeRoots(st.value.stor?.vols)) {
      const j = await fs.ls(base);
      if (!j) continue;
      const dirs = j.entries.filter((x) => x.d && x.n[0] !== ".").sort((a, b) => ((b.t ?? "") < (a.t ?? "") ? -1 : 1)).slice(0, 50); // the newest 50
      for (const e of dirs) {
        const k = await fs.ls(`${base}/${e.n}`);
        const files = (k?.entries ?? []).filter((x) => !x.d && /\.wav$/i.test(x.n)).sort((a, b) => natCmp(a.n, b.n));
        list.push({ id: pre + e.n, name: e.n, date: e.t ?? "", vol, dir: `${base}/${e.n}`, files, size: files.reduce((a, x) => a + x.s, 0) });
      }
    }
    takes.value = list;
  } catch (e) {
    msg.value = `Takes: ${(e as Error).message}`;
  } finally {
    loading.value = false;
  }
}
onMounted(load);
watch(paired, (p) => p && load()); // (the pairing resumes after the page opens)
// again when a take ends, or a drive comes or goes
watch(() => [st.value.rec?.state === 0, (st.value.stor?.vols ?? []).map((v: any) => v.name + v.state).join()], (now, was) => { if (was && now.join() !== was.join()) load(); });

const shown = computed(() => [...takes.value].sort((a, b) => (sort.value === "t" ? (a.date === b.date ? natCmp(a.name, b.name) : a.date < b.date ? 1 : -1) : natCmp(a.name.toLowerCase(), b.name.toLowerCase()))));
const play = computed(() => st.value.play as Record<string, any> | undefined);
const playing = computed(() => !!play.value && (play.value.state === 2 || play.value.state === 3));
const recording = computed(() => st.value.rec?.state === 2 || st.value.rec?.state === 3);
const isPlaying = (t: Take, ch: string) => playing.value && play.value!.take === t.id && play.value!.ch === ch;

async function send(c: string) {
  msg.value = "";
  try {
    await cmd(c);
  } catch (e) {
    msg.value = (e as Error).message;
  }
}
const progress = ref("");
async function download(t: Take) {
  for (const f of t.files) {
    progress.value = `${t.name}/${f.n}: 0 %`;
    try {
      const blob = await fs.get(`${t.dir}/${f.n}`, f.s, (n) => (progress.value = `${t.name}/${f.n}: ${Math.round((100 * n) / Math.max(1, f.s))} %`));
      saveBlob(blob, `${t.name}_${f.n}`);
    } catch (e) {
      msg.value = `Download: ${(e as Error).message}`;
      break;
    }
  }
  progress.value = "";
}
async function del(t: Take) {
  if (!(await confirmAsk({ title: "Delete a take", body: `Delete ${t.name} (${t.files.length} files, ${hb(t.size)})${t.vol !== "SD" ? ` on ${t.vol}` : ""}?`, yes: "Delete", danger: true }))) return;
  for (const f of t.files) {
    const r = await fs.del(`${t.dir}/${f.n}`);
    if (!r.ok) return void (msg.value = `${f.n}: ${r.msg}`);
  }
  const r = await fs.del(t.dir);
  if (!r.ok) msg.value = r.msg;
  load();
}
</script>

<template>
  <Card label="Takes">
    <p v-if="!paired">The takes need the pairing.</p>
    <template v-else>
      <div class="row">
        <Segmented label="Order" :options="[{ value: 't', text: 'Newest first' }, { value: 'n', text: 'By name' }]" :model-value="sort" @update:model-value="(v: string) => (sort = v as 't' | 'n')" />
        <button type="button" :disabled="loading" @click="load">{{ loading ? "Loading…" : "Refresh" }}</button>
      </div>
      <p role="status" class="play">
        <template v-if="playing">Playing {{ play!.take }} · {{ play!.ch }} · <span class="num">{{ hms(play!.posMs / 1000) }}</span> / {{ hms(play!.lenMs / 1000) }} · level {{ play!.level }}</template>
        <template v-else>Playback: a channel button plays it on the Rytm's MAIN{{ recording ? " (not while recording)" : "" }}.</template>
        <button type="button" :disabled="!playing" @click="send(recCmd.playStop)">Stop playback</button>
      </p>
      <p v-if="!takes.length && !loading">No takes yet.</p>
      <ul class="takes">
        <li v-for="t in shown" :key="t.id">
          <div class="head">
            <b>{{ t.name }}</b>
            <span class="small">{{ t.vol !== "SD" ? `${t.vol} · ` : "" }}{{ t.date || "no date" }} · {{ t.files.length }} files · {{ hb(t.size) }}</span>
          </div>
          <div class="row" role="group" :aria-label="`Play a channel of ${t.name}`">
            <template v-for="f in t.files" :key="f.n">
              <button v-if="takeFile(f.n)" type="button" :aria-pressed="isPlaying(t, takeFile(f.n)!.ch)" :disabled="recording" @click="send(recCmd.play(t.id, takeFile(f.n)!.nn))">
                <span aria-hidden="true">{{ isPlaying(t, takeFile(f.n)!.ch) ? "■" : "▶" }} </span>{{ takeFile(f.n)!.ch }}
              </button>
            </template>
          </div>
          <div class="row">
            <button type="button" @click="download(t)">Download</button>
            <button type="button" class="danger" @click="del(t)">Delete</button>
          </div>
        </li>
      </ul>
      <p v-if="progress" class="small" role="status">{{ progress }}</p>
    </template>
    <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.375rem; }
.play { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem 1rem; margin: 0; }
.takes { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.5rem; }
.takes li { border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem 0.75rem; display: flex; flex-direction: column; gap: 0.375rem; }
.head { display: flex; flex-wrap: wrap; gap: 0.25rem 0.75rem; align-items: baseline; }
button[aria-pressed="true"] { border: 2px solid var(--accent); background: var(--accent-tint); font-weight: 700; }
.danger { border-color: var(--warm); color: var(--warm); }
</style>
