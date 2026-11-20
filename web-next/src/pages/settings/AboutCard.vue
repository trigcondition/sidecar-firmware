<!-- Settings › About (the old page's About tab). -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { uptimeText } from "../../lib/status";
import { WEB_BUILD, buildState, buildText } from "../../lib/build";
import { collectBlackbox } from "../../lib/blackbox";

const { status, hello, api } = useSidecar();

// the black box as one file for a bug report (lib/blackbox.ts): read in chunks, secrets masked, saved through a Blob
const bbBusy = ref(false);
const bbLine = ref("");
const bbErr = ref(false);
const kb = (n: number) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`);
async function downloadBlackbox() {
  bbBusy.value = true;
  bbErr.value = false;
  bbLine.value = "Reading the black box…";
  const s = (status.value ?? {}) as Record<string, any>;
  const r = await collectBlackbox(api, {
    version: String(s.version ?? hello.value?.version ?? ""),
    commit: s.commit ? `${s.commit}${s.dirty ? "-dirty" : ""}` : undefined,
    onProgress: (label, done, size) => (bbLine.value = `Reading ${label}: ${kb(done)} of ${kb(size)}`),
  });
  bbBusy.value = false;
  if (r.kind === "error") {
    bbErr.value = true;
    bbLine.value = r.text;
    return;
  }
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([r.text], { type: "text/plain" }));
  a.download = r.name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
  bbLine.value = `Saved ${r.name} (${kb(r.bytes)}).`;
}
const rows = computed(() => {
  const s = (status.value ?? {}) as Record<string, any>;
  return [
    ["Version", s.version ? `${s.version} (${s.commit}${s.dirty ? "-dirty" : ""})` : hello.value?.version ?? "–"],
    ["Web app", `${buildText(WEB_BUILD)} · ${({ match: "matches the Sidecar", "page-older": "older than the Sidecar", "page-newer": "newer than the Sidecar", unknown: "–" } as const)[buildState(WEB_BUILD.version, hello.value?.version)]}`],
    ["Host", `${s.host ?? "sidecar"}.local`],
    ["Address", s.ip ?? "–"],
    ["Uptime", uptimeText(s.uptime)],
    ["Link", s.link ?? "–"],
    ["USB-C", s.gadget ?? "–"],
    ["Built", s.build ?? "–"],
    ["Circle", s.circle ?? "–"],
    ["Web", s.web ? `${s.web.requests} requests · slowest ${s.web.maxus} us · ${s.web.commands} commands` : "–"],
    ["Protocol", hello.value ? `/ws ${hello.value.proto} · topics ${hello.value.topics.join(", ")}` : "–"],
  ] as [string, string][];
});
</script>

<template>
  <Card label="About" tone="plain">
    <dl class="kv">
      <template v-for="[k, v] in rows" :key="k">
        <dt class="label">{{ k }}</dt>
        <dd>{{ v }}</dd>
      </template>
    </dl>
    <div class="bb">
      <button type="button" :disabled="bbBusy" @click="downloadBlackbox">{{ bbBusy ? "Downloading…" : "Download black box" }}</button>
      <p v-if="bbLine" class="line" :class="{ err: bbErr }" role="status">{{ bbLine }}</p>
      <p class="help">Send this file when you report a problem; save it before restarting.</p>
    </div>
    <p class="classic"><a href="/classic">The classic page</a> (the Sidecar's first web page, still served)</p>
  </Card>
</template>

<style scoped>
.kv { display: grid; grid-template-columns: max-content 1fr; gap: 0.25rem 1rem; margin: 0; align-items: baseline; }
.kv dd { margin: 0; overflow-wrap: anywhere; }
.bb { margin: 0.75rem 0 0; }
.bb .line, .bb .help { margin: 0.4rem 0 0; }
.bb .help { opacity: 0.7; }
.bb .err { color: var(--bad, var(--warm)); }
.classic { margin: 0.75rem 0 0; }
.classic a { color: var(--accent); }
</style>
