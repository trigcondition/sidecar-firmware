<!-- web-next/src/pages/Home.vue - plumbing for now: the connection, the pairing, the live status, one harmless
     command. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import { RequestError } from "../lib/client";
import { useSidecar } from "../lib/useSidecar";

const { client, state, hello, paired, status, levels, lastError, pair } = useSidecar();

const code = ref("");
const pairMsg = ref("");
const cmdMsg = ref("");
const busy = ref(false);

const uptime = computed(() => {
  const s = Number(status.value?.uptime ?? 0);
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m ${s % 60}s`;
});
const masterDb = computed(() => {
  const m = levels.value ? Math.max(...levels.value.master) : 0;
  return m > 0 ? `${(20 * Math.log10(m)).toFixed(1)} dB` : "-inf dB";
});

async function doPair() {
  pairMsg.value = "";
  try {
    await pair(code.value);
    code.value = "";
    pairMsg.value = paired.value ? "Paired." : "Paired, but the Sidecar did not take the token.";
  } catch (e) {
    pairMsg.value = (e as Error).message;
  }
}

async function sendVersion() {
  busy.value = true;
  cmdMsg.value = "";
  try {
    await client.command("version");
    cmdMsg.value = "Sent: the version is in the Sidecar's log.";
  } catch (e) {
    cmdMsg.value = e instanceof RequestError ? `${e.code}: ${e.message}` : (e as Error).message;
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <h1 class="title visually-hidden" tabindex="-1">Home</h1>

  <section class="card" aria-labelledby="h-conn">
    <h2 id="h-conn" class="label">Connection</h2>
    <p>
      <span data-test="state">{{ state }}</span>
      <span v-if="hello"> · {{ hello.server }} {{ hello.version }} · protocol {{ hello.proto }}</span>
      <span> · {{ paired ? "paired" : "not paired" }}</span>
    </p>
    <p v-if="state === 'incompatible'" class="warm">This page does not speak this Sidecar's protocol version. Update the page.</p>
    <p v-if="lastError" class="warm">Last server error: {{ lastError }}</p>
  </section>

  <section v-if="!paired && state === 'open'" class="card" aria-labelledby="h-pair">
    <h2 id="h-pair" class="label">Pair this browser</h2>
    <form @submit.prevent="doPair">
      <label for="code">Code on the Rytm's screen</label><br />
      <input id="code" v-model="code" inputmode="numeric" autocomplete="off" maxlength="9" />
      <button type="submit" :disabled="code.replace(/\s/g, '').length !== 8">Pair</button>
    </form>
    <p v-if="pairMsg" role="status">{{ pairMsg }}</p>
  </section>

  <section class="card" aria-labelledby="h-status">
    <h2 id="h-status" class="label">Status</h2>
    <p v-if="!status">Waiting for the first status…</p>
    <template v-else>
      <p>{{ status.host }} · up <span class="num">{{ uptime }}</span></p>
      <p v-if="levels">Master <span class="num accent">{{ masterDb }}</span></p>
      <details>
        <summary>All of it</summary>
        <pre>{{ JSON.stringify(status, null, 2) }}</pre>
      </details>
    </template>
  </section>

  <section class="card" aria-labelledby="h-cmd">
    <h2 id="h-cmd" class="label">Command</h2>
    <button :disabled="!paired || busy" @click="sendVersion">Send "version"</button>
    <span v-if="!paired"> (needs the pairing)</span>
    <p v-if="cmdMsg" role="status">{{ cmdMsg }}</p>
  </section>
</template>
