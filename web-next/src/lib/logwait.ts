// web-next/src/lib/logwait.ts - some commands answer in the log, not in their reply (pool=: "pool: smp C005 x: SET").
// While a page uses it, it follows the "log" topic; waitFor(match) resolves with the first new line that matches, or
// null after ms (the old page's logAnswer, 6 s).
import { onScopeDispose } from "vue";
import type { SidecarClient } from "./client";
import type { LogPush } from "./logbuf";

export function useLogWait(client: SidecarClient) {
  const waits: { match: (l: string) => unknown; done: (a: unknown) => void }[] = [];
  let partial = "";
  const off = client.subscribe<LogPush>("log", (p) => {
    const lines = (partial + p.text).split("\n");
    partial = lines.pop() ?? "";
    for (const l of lines)
      for (const w of [...waits]) {
        const a = w.match(l);
        if (a) {
          w.done(a);
          break;
        }
      }
  });
  onScopeDispose(off);
  return function waitFor<T>(match: (line: string) => T | null, ms = 6000): Promise<T | null> {
    return new Promise((ok) => {
      const w = {
        match,
        done: (a: unknown) => {
          const i = waits.indexOf(w);
          if (i >= 0) waits.splice(i, 1);
          clearTimeout(t);
          ok(a as T | null);
        },
      };
      const t = setTimeout(() => w.done(null), ms);
      waits.push(w);
    });
  };
}
