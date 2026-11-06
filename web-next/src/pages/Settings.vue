<!-- web-next/src/pages/Settings.vue - the Sidecar's settings (web-next/PARITY.md "Settings"), one card each. -->
<script setup lang="ts">
import AboutCard from "./settings/AboutCard.vue";
import AccessCard from "./settings/AccessCard.vue";
import AudioCard from "./settings/AudioCard.vue";
import BridgeCard from "./settings/BridgeCard.vue";
import DisplayCard from "./settings/DisplayCard.vue";
import LookCard from "./settings/LookCard.vue";
import StorageCard from "./settings/StorageCard.vue";
import UsbCard from "./settings/UsbCard.vue";
import WifiCard from "./settings/WifiCard.vue";
</script>

<template>
  <h1 class="title" tabindex="-1">Settings</h1>
  <div class="grid">
    <div class="col">
      <LookCard />
      <AudioCard />
      <UsbCard />
      <DisplayCard />
      <StorageCard />
    </div>
    <div class="col">
      <WifiCard />
      <AccessCard />
      <BridgeCard />
      <AboutCard />
    </div>
  </div>
</template>

<style scoped>
.title { font-size: 1.75rem; margin: 0.5rem 0 0; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 26rem), 1fr)); gap: 0 1.25rem; align-items: start; }
.col { min-width: 0; }
</style>
