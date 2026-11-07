<!-- Settings › Display (the touchscreen): screensaver and brightness. -->
<script setup lang="ts">
import { computed, ref, watch } from "vue";
import Card from "../../ui/Card.vue";
import Fader from "../../ui/Fader.vue";
import Segmented from "../../ui/Segmented.vue";
import SaveState from "../../ui/SaveState.vue";
import { useSetting } from "../../lib/pending";
import { useSidecar } from "../../lib/useSidecar";

const { status, cmd, paired } = useSidecar();
const st = computed(() => (status.value ?? {}) as Record<string, any>);
const saver = useSetting<number>(status, { read: () => st.value.screensaver, toCmd: (v) => `screensaver=${v}`, send: cmd });
const bright = ref<number>(st.value.brightness ?? 80);
let dragging = false;
watch(() => st.value.brightness, (b) => { if (!dragging && typeof b === "number") bright.value = b; });
const brightMsg = ref("");
async function commit(v: number) {
  dragging = false;
  brightMsg.value = "";
  try {
    await cmd(`brightness=${v}`);
  } catch (e) {
    brightMsg.value = (e as Error).message;
  }
}
</script>

<template>
  <Card label="Display">
    <p v-if="st.display === false">No display is attached.</p>
    <template v-else>
      <span class="label">Screensaver</span>
      <Segmented
        label="Screensaver"
        :options="[{ value: 0, text: 'Off' }, { value: 15, text: '15 min' }, { value: 60, text: '1 h' }, { value: 240, text: '4 h' }]"
        :model-value="saver.value.value ?? 0"
        :disabled="!paired"
        @update:model-value="saver.set"
      />
      <SaveState :state="saver.state.value" :msg="saver.msg.value" />
      <p v-if="st.blank" class="small">The screen is black now (a touch wakes it).</p>
      <Fader v-model="bright" label="Brightness" :min="5" :max="100" unit="%" :disabled="!paired" @update:model-value="dragging = true" @commit="commit" />
      <p v-if="brightMsg" class="small warm" role="status">{{ brightMsg }}</p>
    </template>
  </Card>
</template>
