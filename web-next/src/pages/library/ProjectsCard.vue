<!-- Library › projects (docs/BUNDLES.md, bundleweb.h; the old page's Project tab): the projects on the card, export to
     the card with its progress, cancel, delete into the trash; the bundles on the card; the trash (restore, remove). -->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from "vue";
import Card from "../../ui/Card.vue";
import { confirmAsk } from "../../lib/confirm";
import { hb } from "../../lib/fs";
import { useSidecar } from "../../lib/useSidecar";

interface Project { key: string; rytm: string; files: number; bytes: number; used: number; date?: string; current?: number | boolean; imported?: number | boolean }
interface Bundle { name: string; path?: string; size: number; date: string; incomplete?: boolean }
interface Trash { id: number; key: string; name?: string; why: string; items: number; date?: string }
const { api, paired } = useSidecar();
const projects = ref<Project[]>([]);
const bundles = ref<Bundle[]>([]);
const trash = ref<Trash[]>([]);
const job = ref<Record<string, any> | null>(null);
const msg = ref("");
const bad = ref(false);
let timer: ReturnType<typeof setTimeout> | null = null;
function say(t: string, isBad = false) { msg.value = t; bad.value = isBad; }

async function load() {
  if (!paired.value) return;
  try {
    projects.value = await api.json<Project[]>("/bundle/projects");
    bundles.value = await api.json<Bundle[]>("/bundle/list?src=card");
    const t = await api.json<Trash[]>("/bundle/trash");
    trash.value = Array.isArray(t) ? t : [];
  } catch (e) {
    say((e as Error).message, true);
  }
}
async function poll() {
  try {
    job.value = await api.json("/bundle/status");
  } catch { /* again later */ }
  const running = job.value && job.value.phase && job.value.phase !== "IDLE" && job.value.phase !== "DONE" && job.value.phase !== "FAILED";
  timer = setTimeout(poll, running ? 500 : 3000); // as the old page: 500 ms while a job runs
  if (!running && job.value?.phase === "DONE") load();
}
onMounted(() => {
  load();
  poll();
});
onUnmounted(() => timer && clearTimeout(timer));

const sorted = computed(() => [...projects.value].sort((a, b) => Number(!!b.current) - Number(!!a.current) || b.used - a.used || a.key.localeCompare(b.key)));
const phase = computed(() => {
  const s = job.value;
  if (!s || !s.phase || s.phase === "IDLE") return "";
  if (s.phase === "DONE") return "Done: the bundle is on the card.";
  if (s.phase === "FAILED") return `Failed${s.msg ? `: ${s.msg}` : ""}.`;
  const f = s.total ? Math.floor((100 * s.done) / s.total) : null;
  let t = s.phase + (f != null && s.phase !== "SERVE" ? ` ${f} %` : "");
  if (s.total > 4096) t += ` · ${hb(s.done)} of ${hb(s.total)}`;
  if (s.waiting) t += ` · waiting (${s.waiting})`;
  return t;
});
async function exportCard(p: Project) {
  const r = await api.post<{ ok?: boolean; error?: string; msg?: string }>("/bundle/export", { key: p.key, dest: "card", vol: "", takes: 0 }).catch((e): { ok?: boolean; error?: string; msg?: string } => ({ ok: false, error: (e as Error).message }));
  say(r.ok === false ? (r.error ?? r.msg ?? "refused") : `Exporting ${p.rytm} to the card…`, r.ok === false);
  poll();
}
async function cancel() {
  await api.post("/bundle/cancel").catch(() => undefined);
  poll();
}
async function del(p: Project) {
  if (!(await confirmAsk({ title: "Delete a project", body: `Move the Sidecar files of ${p.rytm} (${p.files} files) to the trash? Its samples stay on the card; restore it from the trash below.`, yes: "Move to the trash", danger: true }))) return;
  const r = await api.post<{ ok?: boolean; error?: string }>("/bundle/delete", { key: p.key, confirm: p.key });
  say(r.ok === false ? (r.error ?? "refused") : `${p.rytm}: to the trash`, r.ok === false);
  setTimeout(load, 1200);
}
async function removeBundle(b: Bundle) {
  if (!(await confirmAsk({ title: "Delete a bundle", body: `Delete ${b.name} from the card?`, yes: "Delete", danger: true }))) return;
  const r = await api.post<{ ok?: boolean; error?: string }>("/bundle/remove", { path: b.path ?? `/Bundles/${b.name}` });
  say(r.ok === false ? (r.error ?? "refused") : `${b.name} deleted`, r.ok === false);
  load();
}
async function restore(t: Trash) {
  const r = await api.post<{ ok?: boolean; error?: string }>("/bundle/trash/restore", { id: t.id });
  say(r.ok === false ? (r.error ?? "refused") : `Restoring ${t.name || t.key}…`, r.ok === false);
  setTimeout(load, 1200);
}
async function removeTrash(t: Trash) {
  if (!(await confirmAsk({ title: "Remove for good", body: `Remove trash entry ${t.id} (${t.name || t.key}) for good? This cannot be undone.`, yes: "Remove for good", danger: true }))) return;
  await api.post("/bundle/trash/remove", { id: t.id, confirm: t.id });
  load();
}
</script>

<template>
  <Card label="Projects">
    <p v-if="!paired">The projects need the pairing.</p>
    <template v-else>
      <div v-if="phase" class="job" role="status">
        <span>{{ phase }}</span>
        <template v-if="job?.phase !== 'DONE' && job?.phase !== 'FAILED'">
          <progress :value="job?.done ?? 0" :max="job?.total || 1" aria-label="Bundle job" />
          <button type="button" @click="cancel">Cancel</button>
        </template>
      </div>
      <div class="scroller">
        <table>
          <thead><tr><th scope="col">Project</th><th scope="col">Files</th><th scope="col">Size</th><th scope="col">Last used</th><th scope="col"><span class="visually-hidden">Actions</span></th></tr></thead>
          <tbody>
            <tr v-for="p in sorted" :key="p.key">
              <td>{{ p.rytm }}<b v-if="p.current"> (current)</b><span v-if="p.imported" class="small"> · imported</span><span v-if="p.rytm !== p.key" class="small"> ({{ p.key }})</span></td>
              <td class="num">{{ p.files }}</td>
              <td>{{ hb(p.bytes) }}</td>
              <td class="small">{{ p.date ? p.date.slice(0, 16).replace("T", " ") : p.used ? `#${p.used}` : "–" }}</td>
              <td class="acts">
                <button type="button" @click="exportCard(p)">Export to the card</button>
                <button v-if="!p.current" type="button" class="danger" @click="del(p)">Delete</button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <p class="small">Download to this computer, import and USB sticks: on the old page for now.</p>

      <span class="label">Bundles on the card</span>
      <ul class="list">
        <li v-for="b in bundles" :key="b.name"><span class="name">{{ b.name }}<span v-if="b.incomplete" class="small"> (incomplete)</span></span><span class="small">{{ hb(b.size) }} · {{ b.date }}</span><button type="button" class="danger" @click="removeBundle(b)">Delete</button></li>
        <li v-if="!bundles.length" class="small">None yet.</li>
      </ul>

      <span class="label">Trash</span>
      <ul class="list">
        <li v-for="t in trash" :key="t.id"><span class="name">{{ t.name || t.key }}</span><span class="small">{{ t.why }} · {{ t.items }} items · {{ (t.date || "").replace("T", " ").slice(0, 16) }}</span>
          <button type="button" @click="restore(t)">Restore</button><button type="button" class="danger" @click="removeTrash(t)">Remove…</button></li>
        <li v-if="!trash.length" class="small">Empty.</li>
      </ul>
      <p v-if="msg" role="status" class="small" :class="{ warm: bad }">{{ msg }}</p>
    </template>
  </Card>
</template>

<style scoped>
.job { display: flex; flex-wrap: wrap; align-items: center; gap: 0.5rem 1rem; }
.scroller { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; }
th, td { text-align: left; padding: 0.375rem 0.5rem; border-bottom: 1px solid var(--border); }
th { font-size: var(--label-size); color: var(--label); text-transform: uppercase; letter-spacing: var(--label-tracking); font-weight: 500; }
.acts { display: flex; flex-wrap: wrap; gap: 0.25rem; }
.list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.25rem; }
.list li { display: flex; flex-wrap: wrap; align-items: center; gap: 0.25rem 0.75rem; border-bottom: 1px solid var(--border); padding: 0.25rem 0; }
.name { flex: 1 1 10rem; overflow-wrap: anywhere; }
.danger { border-color: var(--warm); color: var(--warm); }
progress { width: 10rem; accent-color: var(--accent); }
</style>
