<!-- Library › files on the card (fsweb.h; the old page's Files tab): browse the content folders and the drives'
     recordings, sort, hidden files, download, new folder, rename, delete, upload files and folders (48 KB pieces with a
     CRC, resumed), and +SMP / +WT / +INST into a POOL bank, Fill bank. -->
<script setup lang="ts">
import { computed, onMounted, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import { confirmAsk } from "../../lib/confirm";
import { createFs, hb, saveBlob, type FsEntry } from "../../lib/fs";
import { useLogWait } from "../../lib/logwait";
import { notify } from "../../lib/notify";
import { BANKS, answerText, poolAnswer } from "../../lib/pool";
import { natCmp } from "../../lib/rec";
import { uploadFile, type Source } from "../../lib/upload";
import { useSidecar } from "../../lib/useSidecar";

const { api, cmd, paired, client } = useSidecar();
const fs = createFs(api);
const waitFor = useLogWait(client);
const SHORTCUTS = ["/", "/Samples", "/Plugins", "/Recordings", "/MULTISAMPLE", "/WAVETABLES", "/GRANULAR", "/PRESETS", "/Bundles"];
const path = ref("/Samples");
const entries = ref<FsEntry[]>([]);
const free = ref<number | null>(null);
const msg = ref("");
const bad = ref(false);
const sort = ref<"n" | "t">("n");
const hidden = ref(false);
const bank = ref(0);
try {
  bank.value = +(localStorage.getItem("sidecar.fsbank") || 0) & 7;
  hidden.value = localStorage.getItem("sidecar.hidden") === "1";
} catch { /* storage blocked */ }
watch([bank, hidden], ([b, h]) => { try { localStorage.setItem("sidecar.fsbank", String(b)); localStorage.setItem("sidecar.hidden", h ? "1" : "0"); } catch { /* not kept */ } });

// a result shows next to the card and as a toast (the line may be off-screen under a long list); progress only here
function say(t: string, isBad = false, toast = true) { msg.value = t; bad.value = isBad; if (toast) notify.result(t, isBad); }
const join = (a: string, b: string) => (a === "/" ? `/${b}` : `${a}/${b}`);
async function open(p: string) {
  if (!paired.value) return;
  const j = await fs.ls(p);
  if (!j) return say(`${p}: not there, or not reachable from the web`, true);
  path.value = j.path || p;
  entries.value = j.entries;
  free.value = j.free ?? null;
}
onMounted(() => open(path.value));
watch(paired, (p) => p && open(path.value));
const up = () => open(path.value.slice(0, path.value.lastIndexOf("/")) || "/");
const shown = computed(() =>
  entries.value
    .filter((x) => hidden.value || x.n[0] !== ".")
    .sort((a, b) => (!!b.d !== !!a.d ? (b.d ? 1 : -1) : sort.value === "t" ? ((b.t ?? "") < (a.t ?? "") ? -1 : (b.t ?? "") > (a.t ?? "") ? 1 : 0) : natCmp(a.n.toLowerCase(), b.n.toLowerCase()))),
);
const crumbs = computed(() => {
  const parts = path.value.split("/").filter(Boolean);
  return [{ n: "card", p: "/" }, ...parts.map((n, i) => ({ n, p: "/" + parts.slice(0, i + 1).join("/") }))];
});
const kinds = (x: FsEntry) => (x.d ? [] : /\.(wav|aif|aiff)$/i.test(x.n) ? ["smp", "wt"] : /\.(sfz|elmulti)$/i.test(x.n) ? ["inst"] : []);

async function download(x: FsEntry) {
  say(`${x.n}: 0 %`, false, false);
  try {
    saveBlob(await fs.get(join(path.value, x.n), x.s, (n) => say(`${x.n}: ${Math.round((100 * n) / Math.max(1, x.s))} %`, false, false)), x.n);
    say(`${x.n}: downloaded`);
  } catch (e) {
    say(`${x.n}: ${(e as Error).message}`, true);
  }
}
async function del(x: FsEntry) {
  if (!(await confirmAsk({ title: "Delete", body: `Delete ${join(path.value, x.n)}${x.d ? " (the folder must be empty)" : ""}?`, yes: "Delete", danger: true }))) return;
  const r = await fs.del(join(path.value, x.n));
  say(r.msg, !r.ok);
  open(path.value);
}
const renaming = ref<{ from: string; to: string } | null>(null);
async function rename() {
  const r0 = renaming.value;
  renaming.value = null;
  if (!r0 || !r0.to || r0.to.includes("/") || r0.to === r0.from) return;
  const r = await fs.post("mv", { path: join(path.value, r0.from), to: join(path.value, r0.to) });
  say(r.msg, !r.ok);
  open(path.value);
}
const newFolder = ref("");
async function mkdir() {
  const n = newFolder.value.trim();
  if (!n || n.includes("/")) return;
  const r = await fs.post("mkdir", { path: join(path.value, n) });
  newFolder.value = "";
  say(r.msg, !r.ok);
  open(path.value);
}
async function pool(k: string, x: FsEntry) {
  const p = join(path.value, x.n);
  if (p.length > 159) return say("The path is too long for the POOL (159 characters).", true);
  const w = waitFor((l) => poolAnswer(l, k));
  try {
    await cmd(`pool=${k}:add:${BANKS[bank.value]}:${p}`);
  } catch (e) {
    return say((e as Error).message, true);
  }
  const [t, b] = answerText("add", BANKS[bank.value], await w, x.n);
  say(t, b);
}
async function fill(x: FsEntry) {
  const k = (await confirmAsk({ title: "Fill bank", body: `Fill SAMPLES bank ${BANKS[bank.value]}'s empty rows with the files in ${x.n}? (WAVETABLES: use the WT button on a file.)`, yes: "Fill SAMPLES" })) ? "smp" : null;
  if (!k) return;
  const w = waitFor((l) => poolAnswer(l, k));
  await cmd(`pool=${k}:fill:${BANKS[bank.value]}:${join(path.value, x.n)}`).catch((e) => say((e as Error).message, true));
  const a = await w;
  say(a ? a.text : "sent, no answer yet (see the log)", !a);
}

// uploads: one at a time, each with its progress line
interface Up { name: string; text: string; frac: number; done: boolean; bad: boolean; cancel: boolean }
const ups = ref<Up[]>([]);
let running = false;
const queue: { f: Source; to: string; u: Up }[] = [];
function enqueue(files: FileList | File[], rel = false) {
  for (const f of Array.from(files)) {
    if (/^(\.DS_Store|Thumbs\.db)$|^\._/.test(f.name)) continue; // junk the old page leaves out too
    const sub = rel && (f as File & { webkitRelativePath?: string }).webkitRelativePath ? (f as File & { webkitRelativePath: string }).webkitRelativePath : f.name;
    const u: Up = { name: sub, text: "waiting", frac: 0, done: false, bad: false, cancel: false };
    ups.value.push(u);
    queue.push({ f, to: join(path.value, sub), u });
  }
  run();
}
async function run() {
  if (running) return;
  running = true;
  while (queue.length) {
    const { f, to, u } = queue.shift()!;
    const i = ups.value.findIndex((x) => x === u || x.name === u.name);
    const set = (t: Partial<Up>) => Object.assign(ups.value[i] ?? u, t);
    if (u.cancel) { set({ text: "cancelled", done: true, bad: true }); continue; }
    try {
      const dir = to.slice(0, to.lastIndexOf("/"));
      if (dir !== path.value) await fs.post("mkdir", { path: dir, p: "1" });
      const r = await uploadFile(api, f, to, (p) => set({ text: p.text, frac: p.frac }), () => u.cancel);
      set({ text: r, frac: 1, done: true });
    } catch (e) {
      set({ text: (e as Error).message, done: true, bad: true });
    }
  }
  running = false;
  open(path.value);
}
</script>

<template>
  <Card label="Files">
    <p v-if="!paired">The card's files need the pairing.</p>
    <template v-else>
      <nav aria-label="Folder" class="row">
        <button type="button" :disabled="path === '/'" @click="up">Up</button>
        <template v-for="(c, i) in crumbs" :key="c.p"><span v-if="i" aria-hidden="true">/</span><button type="button" class="crumb" :aria-current="i === crumbs.length - 1 ? 'location' : undefined" @click="open(c.p)">{{ c.n }}</button></template>
      </nav>
      <div class="row">
        <label for="go" class="label">Go to</label>
        <select id="go" :value="SHORTCUTS.includes(path) ? path : ''" @change="open(($event.target as HTMLSelectElement).value)">
          <option value="" disabled>a folder…</option>
          <option v-for="s in SHORTCUTS" :key="s" :value="s">{{ s }}</option>
        </select>
        <Segmented label="Order" :options="[{ value: 'n', text: 'Name' }, { value: 't', text: 'Date' }]" :model-value="sort" @update:model-value="(v: string) => (sort = v as 'n' | 't')" />
        <label class="check"><input v-model="hidden" type="checkbox" /> Hidden files</label>
        <label for="bank" class="label">POOL bank</label>
        <select id="bank" v-model.number="bank"><option v-for="(b, i) in BANKS" :key="b" :value="i">{{ b }}</option></select>
        <span v-if="free !== null" class="small">{{ hb(free) }} free</span>
      </div>

      <ul class="list" :aria-label="`Files in ${path}`">
        <li v-for="x in shown" :key="x.n">
          <button v-if="x.d" type="button" class="name dir" @click="open(join(path, x.n))"><span aria-hidden="true">▸ </span>{{ x.n }}/</button>
          <span v-else class="name">{{ x.n }}</span>
          <span class="small meta">{{ x.d ? "folder" : hb(x.s) }}{{ x.t ? ` · ${x.t}` : "" }}{{ x.ro === 1 ? " · locked" : x.ro === 2 ? " · kept" : "" }}</span>
          <span class="acts">
            <template v-if="renaming && renaming.from === x.n">
              <label :for="`rn-${x.n}`" class="visually-hidden">New name</label>
              <input :id="`rn-${x.n}`" v-model="renaming.to" autocomplete="off" @keydown.enter.prevent="rename" />
              <button type="button" @click="rename">Save</button><button type="button" @click="renaming = null">Cancel</button>
            </template>
            <template v-else>
              <button v-for="k in kinds(x)" :key="k" type="button" :title="`Add to the POOL's bank ${BANKS[bank]}`" @click="pool(k, x)">+{{ k.toUpperCase() }}</button>
              <button v-if="x.d" type="button" @click="fill(x)">Fill bank</button>
              <button v-if="!x.d" type="button" @click="download(x)">Download</button>
              <button v-if="!x.ro" type="button" @click="renaming = { from: x.n, to: x.n }">Rename</button>
              <button v-if="!x.ro" type="button" class="danger" @click="del(x)">Delete</button>
            </template>
          </span>
        </li>
        <li v-if="!shown.length" class="small">Empty.</li>
      </ul>

      <div class="row">
        <form class="row" @submit.prevent="mkdir">
          <label for="nf" class="visually-hidden">New folder name</label>
          <input id="nf" v-model="newFolder" placeholder="new folder" autocomplete="off" />
          <button type="submit" :disabled="!newFolder.trim()">New folder</button>
        </form>
        <label class="btn">Upload files<input type="file" multiple class="visually-hidden" @change="enqueue(($event.target as HTMLInputElement).files!); ($event.target as HTMLInputElement).value = ''" /></label>
        <label class="btn">Upload a folder<input type="file" webkitdirectory class="visually-hidden" @change="enqueue(($event.target as HTMLInputElement).files!, true); ($event.target as HTMLInputElement).value = ''" /></label>
      </div>
      <ul v-if="ups.length" class="ups" aria-label="Uploads">
        <li v-for="(u, i) in ups" :key="i">
          <span class="name">{{ u.name }}</span>
          <progress :value="u.frac" max="1" :aria-label="`${u.name} ${Math.round(u.frac * 100)} %`" />
          <span role="status" class="small" :class="{ warm: u.bad }">{{ u.text }}</span>
          <button v-if="!u.done" type="button" @click="u.cancel = true">Cancel</button>
        </li>
        <li><button type="button" @click="ups = ups.filter((u) => !u.done)">Clear the finished</button></li>
      </ul>
      <p v-if="msg" role="status" class="small" :class="{ warm: bad }">{{ msg }}</p>
    </template>
  </Card>
</template>

<style scoped>
.row { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem; }
.crumb { border: 0; background: transparent; padding: 0.25rem; min-width: 0; color: var(--accent); text-decoration: underline; }
.crumb[aria-current] { color: var(--text); text-decoration: none; font-weight: 700; }
.check { display: inline-flex; align-items: center; gap: 0.375rem; min-height: var(--target); }
.check input { min-width: 1.25rem; min-height: 1.25rem; }
.list, .ups { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.25rem; }
.list li, .ups li { display: flex; flex-wrap: wrap; align-items: center; gap: 0.25rem 0.75rem; border-bottom: 1px solid var(--border); padding: 0.25rem 0; }
.name { flex: 1 1 12rem; overflow-wrap: anywhere; }
.dir { text-align: left; border: 0; background: transparent; padding: 0.25rem 0; color: var(--accent); font-weight: 700; }
.meta { color: var(--label); }
.acts { display: flex; flex-wrap: wrap; gap: 0.25rem; }
.btn { display: inline-flex; align-items: center; min-height: var(--target); padding: 0.5rem 0.875rem; border: var(--control-border-width) solid var(--control-border); border-radius: var(--radius-control); cursor: pointer; }
.btn:focus-within { outline: 3px solid var(--focus); outline-offset: 2px; }
.danger { border-color: var(--warm); color: var(--warm); }
progress { width: 8rem; accent-color: var(--accent); }
</style>
