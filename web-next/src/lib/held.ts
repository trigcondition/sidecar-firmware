// web-next/src/lib/held.ts - a control shows the value it just sent at once, not the Sidecar's old one until the next
// status. It holds that value until the status reports the same, and ignores a status that still has the old one
// (the first push after a click was often built before the command ran). It lets go after holdMs, or at once when a
// command is refused (notify.error counts refusals), and then shows the Sidecar's value again.
import { computed, onScopeDispose, ref, watch } from "vue";
import { refusals } from "./notify";

export const HOLD_MS = 2500;

export function useHeld<T>(source: () => T, holdMs = HOLD_MS) {
  const held = ref<{ v: T } | null>(null);
  let timer: ReturnType<typeof setTimeout> | undefined;
  const drop = () => {
    held.value = null;
    clearTimeout(timer);
  };
  watch(source, (m) => {
    if (held.value && Object.is(m, held.value.v)) drop();
  });
  watch(refusals, drop);
  onScopeDispose(() => clearTimeout(timer));
  return {
    /** what the control shows: the held value, else the Sidecar's */
    shown: computed(() => (held.value ? (held.value.v as T) : source())),
    /** the user chose v (it is sent by the caller) */
    hold(v: T) {
      clearTimeout(timer);
      if (Object.is(v, source())) return drop();
      held.value = { v };
      timer = setTimeout(drop, holdMs);
    },
  };
}
