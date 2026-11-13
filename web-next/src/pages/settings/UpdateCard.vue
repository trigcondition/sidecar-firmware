<!-- Settings › Update (docs/web-update.md): the release's update file (.scu) picked here, checked in the page, sent to
     the Sidecar, installed as a trial and, once the trial has run half a minute, made permanent by the Sidecar itself;
     a trial that does not start goes back to the old firmware on its own. "Back to the previous version" the same way.
     The page never fetches anything from the internet: the file comes from the release page, downloaded by hand. -->
<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import Card from "../../ui/Card.vue";
import { confirmAsk } from "../../lib/confirm";
import { useSidecar } from "../../lib/useSidecar";
import { uploadFile } from "../../lib/upload";
import { loadPrevBoot, type PrevBoot } from "../../lib/prevboot";
import { Follower, HEADER, afterReboot, asksForce, stateFromStatus, checkParts, parseHeader, verdict, type After, type UpdateHeader, type UpdateReply, type UpdateState } from "../../lib/update";

const { api, paired } = useSidecar();

const st = ref<UpdateState | null>(null);
const stErr = ref("");
const file = ref<File | null>(null);
const head = ref<UpdateHeader | null>(null);
const fileErr = ref("");
const force = ref(false);
const busy = ref(false);
const phase = ref<"idle" | "checking" | "sending" | "installing" | "restarting" | "done">("idle");
const frac = ref(0);
const line = ref("");
const after = ref<After | null>(null);
const target = ref<{ version: string; commit: string } | null>(null);
let alive = true;

async function loadState(): Promise<UpdateState | null> {
  try {
    const s = await api.json<UpdateState>("/update/state");
    st.value = s;
    stErr.value = "";
    return s;
  } catch (e) {
    // an engine before 0.5.0 has no /update: 404
    stErr.value = /HTTP 404/.test((e as Error).message) ? "" : (e as Error).message;
    st.value = /HTTP 404/.test((e as Error).message) ? {} : null;
    return null;
  }
}
onMounted(async () => {
  const s = await loadState();
  // the page opened again during a trial (a reload, another device): follow it to its end
  if (s?.state === "trying" || s?.state === "committing") follow(null, 0);
});
onBeforeUnmount(() => (alive = false));

const v = computed(() => (head.value && st.value ? verdict(head.value, st.value) : null));
const running = computed(() => (st.value?.version ? `${st.value.version} (${st.value.commit})` : "–"));
const canInstall = computed(() => !!v.value?.ok && (!("force" in v.value) || !v.value.force || force.value) && !busy.value && paired.value);

async function pick(ev: Event) {
  const input = ev.target as HTMLInputElement;
  const f = input.files?.[0] ?? null;
  input.value = "";   // the same file can be picked again (no change event otherwise); the File object is kept
  file.value = f;
  head.value = null;
  fileErr.value = "";
  force.value = false;
  after.value = null;
  if (!f) return;
  busy.value = true;
  phase.value = "checking";
  try {
    const h = parseHeader(new Uint8Array(await f.slice(0, HEADER).arrayBuffer()), f.size);
    await checkParts(f, h, (x) => {
      frac.value = x;
      line.value = `Checking the file: ${Math.floor(100 * x)} %`;
    });
    head.value = h;
    await loadState();
  } catch (e) {
    fileErr.value = (e as Error).message;
  } finally {
    busy.value = false;
    phase.value = "idle";
    line.value = "";
  }
}

// A poll that cannot hang: a request to a Sidecar that is rebooting may never answer (and would hold a slot of the
// page's request gate), so every poll has its own time limit
function within(ms: number): RequestInit {
  const c = new AbortController();
  setTimeout(() => c.abort(), ms);
  return { signal: c.signal };
}

// polls /update/state until the restarted Sidecar has a final answer (committed, reverted, failed); 5 minutes at most.
// Shows "Trying x: permanent in N s" and then "Updated to x" / "Failed: back on y", and then the card is idle again.
async function follow(t: { version: string; commit: string } | null, since: number) {
  phase.value = "restarting";
  busy.value = true;
  target.value = t;
  const t0 = since || Date.now();
  const f = new Follower(t, since === 0);   // a trial already running (the page opened during it): its answers count at once
  after.value = afterReboot(null, t);
  while (alive && Date.now() - t0 < 5 * 60_000) {
    await new Promise((ok) => setTimeout(ok, 2000));
    let s: UpdateState | null = null;
    try {
      s = await api.json<UpdateState>("/update/state", within(4000));
    } catch {
      s = null;
    }
    if (!s?.api) s = stateFromStatus(await api.json<Record<string, unknown>>("/status", within(4000)).catch(() => null));
    const step = f.step(s, (Date.now() - t0) / 1000);
    after.value = step.after;
    if (step.state) st.value = step.state;
    if (step.after.done) break;
  }
  if (alive && !after.value?.done) after.value = { phase: "failed", text: "The Sidecar has not answered for 5 minutes: switch it off and on. If it still does not come back, see docs: Updating, 'When it goes wrong'.", done: true };
  // the result stays on the card (with its Reload button), the rest is idle again: the picked file is spent
  phase.value = "idle";
  busy.value = false;
  line.value = "";
  head.value = null;
  file.value = null;
  force.value = false;
}

function rebootAnswer(r: UpdateReply, t: { version: string; commit: string } | null) {
  if (r.reboot === "none") {
    line.value = r.ok ? r.msg : `Not done: ${r.msg}`;
    phase.value = "idle";
    busy.value = false;
    return false;
  }
  follow(t, Date.now());
  return true;
}

async function install() {
  const f = file.value, h = head.value;
  if (!f || !h || !v.value?.ok) return;
  const ok = await confirmAsk({
    title: "Install the update",
    body: `${v.value.text} The file goes to the Sidecar's card, then the Sidecar restarts into the new firmware on trial: audio stops for about a minute. Leave it switched on until this page says it is done; if the new firmware does not start, the Sidecar goes back to ${running.value} by itself.`,
    yes: "Install and restart",
    danger: "force" in v.value && v.value.force,
  });
  if (!ok) return;
  busy.value = true;
  after.value = null;
  phase.value = "sending";
  try {
    await uploadFile(api, f, "update.scu", (p) => {
      frac.value = p.frac;
      line.value = p.phase === "checking" ? `Preparing: ${p.text}` : `Sending: ${p.text}`;
    }, () => !alive, { put: "/update/put", cleanup: (a) => a.post("/update/discard") });
    phase.value = "installing";
    line.value = "Checking it on the Sidecar…";
    const r = await api.post<UpdateReply>("/update/install", { force: force.value ? 1 : 0 });
    if (!rebootAnswer(r, { version: h.version, commit: h.commit })) await api.post("/update/discard").catch(() => undefined);
  } catch (e) {
    line.value = `Not installed: ${(e as Error).message.replace(/^\d{3} /, "")}`;
    phase.value = "idle";
    busy.value = false;
  }
}

async function back() {
  const s = st.value;
  if (!s?.prev) return;
  const ok = await confirmAsk({
    title: "Back to the previous version",
    body: `The Sidecar restarts into ${s.prevVersion || "the previous firmware"} (from ${running.value}), on trial as an update is: audio stops for about a minute.`,
    yes: "Go back and restart",
    danger: true,
  });
  if (!ok) return;
  busy.value = true;
  after.value = null;
  phase.value = "installing";
  try {
    let r = await api.post<UpdateReply>("/update/previous", {});
    if (asksForce(r)) {
      const sure = await confirmAsk({
        title: "No safety net for this one",
        body: `${r.msg} It is swapped in at once, without a trial: if it does not start, the card has to be fixed in a computer.`,
        yes: "Swap it in anyway",
        danger: true,
      });
      if (!sure) {
        phase.value = "idle";
        busy.value = false;
        return;
      }
      r = await api.post<UpdateReply>("/update/previous", { force: 1 });
    }
    rebootAnswer(r, null);
  } catch (e) {
    line.value = (e as Error).message;
    phase.value = "idle";
    busy.value = false;
  }
}

const reload = () => location.reload();

// the last crash's report (/prevboot): read on demand, shown here, saved as a file for a bug report
const crash = ref<PrevBoot | null>(null);
const crashBusy = ref(false);
async function readCrash() {
  crashBusy.value = true;
  crash.value = await loadPrevBoot(api);
  crashBusy.value = false;
}
function saveCrash() {
  if (crash.value?.kind !== "ok") return;
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([crash.value.text], { type: "text/plain" }));
  a.download = "sidecar-prevboot.txt";
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
}
</script>

<template>
  <Card label="Update">
    <dl class="kv">
      <dt class="label">Running</dt>
      <dd>{{ running }}<template v-if="st?.board"> · Raspberry Pi {{ st.board }}</template></dd>
      <template v-if="st?.prev">
        <dt class="label">Previous</dt>
        <dd>{{ st.prevVersion || "kept on the card" }}</dd>
      </template>
    </dl>

    <p v-if="stErr" class="warn">{{ stErr }}</p>
    <p v-else-if="st && !st.api" class="note">This Sidecar's firmware cannot update from the page: update it once from a card in a computer.</p>
    <p v-else-if="st && st.enabled === false" class="note">Updates from the page are off: {{ st.why }}.</p>
    <p v-else-if="!after && (st?.state === 'reverted' || st?.state === 'failed')" class="warn">{{ st.msg }}</p>

    <template v-if="st?.api && st.enabled">
      <p class="note">Download the update file (<code>sidecar-&lt;version&gt;.scu</code>) from the release page, then pick it here. It holds both boards' firmware and the web app.</p>
      <label class="pick">
        <span>Update file</span>
        <input type="file" accept=".scu" :disabled="busy || !paired" @change="pick" />
      </label>
      <p v-if="!paired" class="note">Pair this device first (Settings › Access).</p>
      <p v-if="fileErr" class="warn" role="alert">{{ fileErr }}</p>
      <template v-if="head && v">
        <p :class="v.ok ? 'ok' : 'warn'">{{ v.text }}</p>
        <label v-if="v.ok && v.force" class="force"><input v-model="force" type="checkbox" :disabled="busy" /> Install it anyway</label>
        <div class="row">
          <button type="button" :disabled="!canInstall" @click="install">Install {{ head.version }}</button>
        </div>
      </template>
      <div v-if="busy && (phase === 'checking' || phase === 'sending')" class="row">
        <progress :value="frac" max="1" :aria-label="line" />
      </div>
      <p v-if="line" aria-live="polite">{{ line }}</p>
      <div v-if="after" class="after" :class="after.phase" aria-live="polite">
        <p>{{ after.text }}</p>
        <button v-if="after.done" type="button" @click="reload">Reload the page</button>
      </div>
      <div v-if="st.prev && !busy" class="row">
        <button type="button" class="danger" :disabled="!paired" @click="back">Back to {{ st.prevVersion || "the previous version" }}</button>
      </div>
    </template>

    <div class="crash">
      <div class="row">
        <button type="button" :disabled="crashBusy || !paired" @click="readCrash">Read the last crash report</button>
        <button v-if="crash?.kind === 'ok'" type="button" @click="saveCrash">Save as a file</button>
      </div>
      <p v-if="!paired" class="note">Pair this device first (Settings › Access): the report is for paired devices only.</p>
      <p v-else-if="crash?.kind === 'none'" class="note">No crash report on the card: the last boots ended normally.</p>
      <p v-else-if="crash?.kind === 'error'" class="warn">{{ crash.text }}</p>
      <details v-else-if="crash?.kind === 'ok'" open>
        <summary>The report and the last seconds of the log of the boot that crashed ({{ crash.bytes }} bytes)</summary>
        <pre>{{ crash.text }}</pre>
      </details>
    </div>
  </Card>
</template>

<style scoped>
.kv { display: grid; grid-template-columns: max-content 1fr; gap: 0.25rem 1rem; margin: 0; }
.kv dd { margin: 0; }
.note { color: var(--label); margin: 0; }
.danger { border-color: var(--warm); color: var(--warm); }
.warn { color: var(--warm); margin: 0; }
.ok { margin: 0; }
.pick { display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; }
.force { display: flex; gap: 0.5rem; align-items: center; }
.row { display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; }
progress { width: 100%; max-width: 24rem; accent-color: var(--accent); }
.crash { margin-top: 0.75rem; display: grid; gap: 0.5rem; }
.crash pre { max-height: 20rem; overflow: auto; font-size: 0.75rem; white-space: pre-wrap; overflow-wrap: anywhere; }
.after p { margin: 0 0 0.5rem; }
.after.reverted p, .after.failed p { color: var(--warm); }
</style>
