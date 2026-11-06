<!-- Settings › Storage: the card and drives (eject), the SD card speed test, the sample library's index. -->
<script setup lang="ts">
import { computed, ref } from "vue";
import Card from "../../ui/Card.vue";
import { useSidecar } from "../../lib/useSidecar";
import { confirmAsk } from "../../lib/confirm";

const { status, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const vols = computed(() => (st.value.stor?.vols ?? []) as Record<string, any>[]);
const sd = computed(() => st.value.sdtest as Record<string, any> | undefined);
const lib = computed(() => st.value.library as Record<string, any> | undefined);
const msg = ref("");
const gb = (mb: number) => (mb / 1024).toFixed(1);

async function eject(v: string) {
  if (!(await confirmAsk({ title: `Eject ${v}`, body: `Recordings stop going to ${v}; pull it out when it says it is gone.`, yes: "Eject" }))) return;
  await run(`stor=eject:${v}`);
}
async function sdTest() {
  if (!(await confirmAsk({ title: "SD card speed test", body: "About a minute of writes and reads on the card. Not while you play or record.", yes: "Run the test" }))) return;
  await run("sdtest=start!");
}
async function run(c: string) {
  msg.value = "";
  try {
    await cmd(c);
  } catch (e) {
    msg.value = (e as Error).message;
  }
}
</script>

<template>
  <Card label="Storage">
    <ul class="vols">
      <li v-for="v in vols" :key="v.name">
        <b>{{ v.name }}</b>
        <span>{{ v.state }}<template v-if="v.fs"> · {{ v.fs }}</template></span>
        <span v-if="v.totalMB"><span class="num">{{ gb(v.freeMB) }}</span> of {{ gb(v.totalMB) }} GB free</span>
        <span v-if="v.model" class="small">{{ v.model }}</span>
        <button v-if="v.name !== 'SD' && v.state === 'ok'" type="button" :disabled="!paired" @click="eject(v.name)">Eject</button>
      </li>
    </ul>
    <div v-if="sd">
      <span class="label">SD card speed</span>
      <p v-if="sd.state === 'running'" role="status">Testing… {{ sd.phase }} <span class="num">{{ sd.progress }}</span> %</p>
      <p v-else-if="sd.have">
        {{ sd.verdict }} · write <span class="num">{{ sd.writeMBps }}</span> MB/s · read <span class="num">{{ sd.readMBps }}</span> MB/s
        · random <span class="num">{{ sd.rand32Iops }}</span> IOPS (p99 {{ sd.rand32P99Ms }} ms)
      </p>
      <p v-else class="small">Not measured yet.</p>
      <button type="button" :disabled="!paired || sd.state === 'running'" @click="sdTest">Test the card</button>
      <button v-if="sd.state === 'running'" type="button" @click="run('sdtest=cancel')">Cancel</button>
    </div>
    <p v-if="lib" class="small">
      Sample library: {{ lib.files }} files, {{ lib.pct }} % indexed<template v-if="lib.dups">, {{ lib.dups }} duplicates</template><template v-if="lib.missing">, {{ lib.missing }} missing</template>.
    </p>
    <p v-if="msg" class="small warm" role="status">{{ msg }}</p>
  </Card>
</template>

<style scoped>
.vols { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 0.5rem; }
.vols li { display: flex; flex-wrap: wrap; align-items: center; gap: 0.25rem 1rem; border: 1px solid var(--border); border-radius: var(--radius-control); padding: 0.5rem 0.75rem; }
</style>
