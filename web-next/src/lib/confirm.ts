// web-next/src/lib/confirm.ts - "are you sure?" for the actions the old page confirmed (forget devices, Overbridge
// mode, the SD test, ...): one <dialog> in App.vue (ConfirmDialog.vue), opened with a promise.
import { shallowRef } from "vue";

export interface Ask {
  title: string;
  body: string;
  yes: string;
  no?: string;
  danger?: boolean;
  resolve: (ok: boolean) => void;
}
export const asking = shallowRef<Ask | null>(null);

export function confirmAsk(o: Omit<Ask, "resolve">): Promise<boolean> {
  asking.value?.resolve(false);
  return new Promise((resolve) => (asking.value = { ...o, resolve }));
}
