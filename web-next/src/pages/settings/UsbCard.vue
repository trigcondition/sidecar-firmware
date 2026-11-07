<!-- Settings › USB-C mode: the Sidecar's own device (class compliant) or the box itself (Overbridge); the old page's
     confirm and warning (docs/usb-mirror.md). usbmode=overbridge needs the strict same-origin check: /ws has it. -->
<script setup lang="ts">
import { computed } from "vue";
import Card from "../../ui/Card.vue";
import Segmented from "../../ui/Segmented.vue";
import SaveState from "../../ui/SaveState.vue";
import { useSetting } from "../../lib/pending";
import { confirmAsk } from "../../lib/confirm";
import { useSidecar } from "../../lib/useSidecar";
import { boxName } from "../../lib/tiles";

const { status, cmd, paired, trusted } = useSidecar();
const um = computed(() => ((status.value ?? {}) as Record<string, any>).usbMode as Record<string, any> | undefined);
const box = computed(() => boxName(status.value));
const mode = useSetting<string>(status, { read: () => um.value?.set, toCmd: (v) => `usbmode=${v}`, send: cmd, timeoutMs: 6000 });
const modes = computed<string[]>(() => um.value?.modes ?? ["class", "overbridge"]);
const shown = computed(() => {
  const s = um.value?.shown;
  return s === "sidecar" ? "the Sidecar's own device" : s === "box" ? `the ${box.value} itself` : s === "switching" ? "replugging…" : "nothing";
});
async function pick(v: string) {
  if (v === "overbridge") {
    const ok = await confirmAsk({
      title: "Overbridge mode",
      body: `The computer then talks to the ${box.value} itself through the Sidecar, with nothing filtered: an OS update can reach the ${box.value} this way. The computer sees the USB-C device leave and come back.${trusted.value ? " This page reaches the Sidecar over that USB-C cable: its network goes with class compliant mode: set it back on the Rytm (SIDECAR MENU > USB-C) or from Wi-Fi or Ethernet." : ""}`,
      yes: "Switch to Overbridge",
    });
    if (!ok) return;
  }
  await mode.set(v);
}
</script>

<template>
  <Card label="USB-C mode">
    <Segmented
      label="USB-C mode"
      :options="[{ value: 'class', text: 'Class compliant', disabled: !modes.includes('class') }, { value: 'overbridge', text: 'Overbridge', disabled: !modes.includes('overbridge') }]"
      :model-value="mode.value.value ?? 'class'"
      :disabled="!paired || !um"
      @update:model-value="pick"
    />
    <SaveState :state="mode.state.value" :msg="mode.msg.value" />
    <p v-if="um">Now the computer sees {{ shown }}<template v-if="um.from === 'cmdline'"> (from cmdline.txt usb=)</template>.</p>
    <p v-if="mode.value.value === 'overbridge'" class="small warm"><span aria-hidden="true">⚠ </span>Overbridge: the {{ box }}'s requests pass unfiltered, OS updates included.</p>
    <p v-if="!um" class="small">This Sidecar has no USB-C device (the Pi 5 without the gadget build).</p>
  </Card>
</template>
