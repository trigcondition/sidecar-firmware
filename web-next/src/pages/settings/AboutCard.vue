<!-- Settings › About (the old page's About tab). -->
<script setup lang="ts">
import { computed } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { uptimeText } from "../../lib/status";

const { status, hello } = useSidecar();
const rows = computed(() => {
  const s = (status.value ?? {}) as Record<string, any>;
  return [
    ["Version", s.version ? `${s.version} (${s.commit}${s.dirty ? "-dirty" : ""})` : hello.value?.version ?? "–"],
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
  </Card>
</template>

<style scoped>
.kv { display: grid; grid-template-columns: max-content 1fr; gap: 0.25rem 1rem; margin: 0; align-items: baseline; }
.kv dd { margin: 0; overflow-wrap: anywhere; }
</style>
