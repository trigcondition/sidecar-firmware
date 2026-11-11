<!-- web-next/src/ui/Toasts.vue - where lib/notify.ts's toasts appear: bottom right (bottom centre on a phone), in the
     page's colours (tokens.css, so the high-contrast theme too), each with a close button. vue-sonner 2.0.9 (MIT). -->
<script setup lang="ts">
import { Toaster } from "vue-sonner";
import "vue-sonner/style.css";
</script>

<template>
  <Toaster position="bottom-right" theme="dark" close-button :duration="6000" :visible-toasts="4" />
</template>

<style>
/* vue-sonner's own variables over the tokens */
[data-sonner-toaster][data-sonner-theme] {
  font-family: inherit;
  --normal-bg: var(--card);
  --normal-border: var(--control-border);
  --normal-text: var(--text);
  --border-radius: var(--radius-control);
  --width: min(24rem, calc(100vw - 2rem));
}
[data-sonner-toaster] [data-sonner-toast] { font-size: 0.9375rem; border-width: 2px; }
[data-sonner-toaster] [data-sonner-toast] [data-description] { color: var(--label); }
[data-sonner-toaster] [data-sonner-toast][data-type="error"],
[data-sonner-toaster] [data-sonner-toast][data-type="warning"] { border-color: var(--warm); }
[data-sonner-toaster] [data-sonner-toast][data-type="error"] [data-icon],
[data-sonner-toaster] [data-sonner-toast][data-type="warning"] [data-icon] { color: var(--warm); }
[data-sonner-toaster] [data-sonner-toast][data-type="success"] [data-icon] { color: var(--accent); }
[data-sonner-toaster] [data-sonner-toast] [data-close-button] {
  background: var(--card); color: var(--text); border-color: var(--control-border);
}
</style>
