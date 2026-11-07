// web-next/src/lib/useSend.ts - a section's command sender: dials and faders coalesced (the newest value of a control
// wins while one is on its way, lib/coalesce.ts), a refusal as a toast (lib/notify.ts).
import { coalescer } from "./coalesce";
import { notify } from "./notify";
import { useSidecar } from "./useSidecar";

export function useSend(): (c: string) => void {
  const { cmd } = useSidecar();
  const latest = coalescer(cmd, (e) => notify.error(e));
  return (c: string) => void latest(c);
}
