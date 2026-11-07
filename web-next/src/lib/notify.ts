// web-next/src/lib/notify.ts - short notifications (toasts, vue-sonner; drawn by ui/Toasts.vue, read out by screen
// readers). For what happened, not for what a control is doing: a control keeps its own state next to it (SaveState).
import { toast } from "vue-sonner";
import { RequestError } from "./client";

/** A failed command or request as one line: the Sidecar's refusal with its code, or the error's message. */
export function errText(e: unknown): string {
  if (e instanceof RequestError) return `Refused (${e.code}): ${e.message}`;
  if (e instanceof Error) return e.message;
  return String(e);
}

export const notify = {
  info: (title: string, description?: string, id?: string) => toast(title, { description, id }),
  ok: (title: string, description?: string, id?: string) => toast.success(title, { description, id }),
  warn: (title: string, description?: string, id?: string) => toast.warning(title, { description, id }),
  /** What a finished action did (a card's result line, also shown next to it): ok or not */
  result: (text: string, bad = false) => (bad ? toast.error(text, { id: `err:${text}` }) : toast.success(text)),
  /** A failed command: one toast per kind of failure (the same text replaces the last one, no pile of copies). */
  error: (e: unknown, title = "The Sidecar did not take that") => {
    const d = errText(e);
    toast.error(title, { description: d, id: `err:${d}` });
  },
};
