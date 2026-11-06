<!-- web-next/src/components/Logo.vue - the "Docked" mark. Decorative (the wordmark beside it carries the name).
     animated: each dot breathes 1 -> 0.62 -> 1 over 4.8 s, delayed by its distance to the centre, so a slow ripple
     runs outward; CSS only, off under prefers-reduced-motion (tokens.css). The disc stays still. -->
<script setup lang="ts">
import { computed } from "vue";
import { LOGO_DISC, LOGO_REACH, LOGO_VIEWBOX, logoDots } from "./logo";

const props = withDefaults(defineProps<{ size?: number; animated?: boolean }>(), { size: 40, animated: false });
const dots = computed(() => logoDots(props.size < 40));
</script>

<template>
  <svg
    class="logo"
    :class="{ breathe: animated }"
    :viewBox="LOGO_VIEWBOX"
    :width="(size * 46) / 40"
    :height="size"
    aria-hidden="true"
    focusable="false"
  >
    <circle
      v-for="(p, i) in dots"
      :key="i"
      :cx="p.x"
      :cy="p.y"
      :r="p.r"
      class="dot"
      :style="animated ? { animationDelay: `${((p.d / LOGO_REACH) * 1.6).toFixed(3)}s` } : undefined"
    />
    <circle :cx="LOGO_DISC.cx" :cy="LOGO_DISC.cy" :r="LOGO_DISC.r" class="disc" />
  </svg>
</template>

<style scoped>
.dot { fill: var(--text); }
.disc { fill: var(--accent); }
.breathe .dot { animation: breathe 4.8s ease-in-out infinite; }
@keyframes breathe {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.62; }
}
</style>
