<!-- web-next/src/ui/ConfirmDialog.vue - the one confirm dialog (lib/confirm.ts): a native modal <dialog>, so focus is held
     inside and Esc cancels; the safe answer has the focus first. -->
<script setup lang="ts">
import { nextTick, ref, watch } from "vue";
import { asking } from "../lib/confirm";

const dlg = ref<HTMLDialogElement | null>(null);
const noBtn = ref<HTMLButtonElement | null>(null);
watch(asking, async (a) => {
  await nextTick();
  if (a && dlg.value && !dlg.value.open) {
    dlg.value.showModal();
    noBtn.value?.focus();
  } else if (!a && dlg.value?.open) dlg.value.close();
});
function answer(ok: boolean) {
  const a = asking.value;
  asking.value = null;
  a?.resolve(ok);
}
</script>

<template>
  <dialog ref="dlg" class="card" aria-labelledby="dlg-title" @cancel.prevent="answer(false)">
    <template v-if="asking">
      <h2 id="dlg-title" class="label">{{ asking.title }}</h2>
      <p>{{ asking.body }}</p>
      <div class="row">
        <button ref="noBtn" type="button" @click="answer(false)">{{ asking.no ?? "Cancel" }}</button>
        <button type="button" :class="{ danger: asking.danger }" @click="answer(true)">{{ asking.yes }}</button>
      </div>
    </template>
  </dialog>
</template>

<style scoped>
dialog { max-width: min(32rem, calc(100vw - 2rem)); color: var(--text); margin: auto; }
dialog::backdrop { background: var(--backdrop); }
.row { display: flex; flex-wrap: wrap; gap: 0.5rem; justify-content: flex-end; }
.danger { border-color: var(--warm); border-width: 2px; color: var(--warm); font-weight: 700; }
</style>
