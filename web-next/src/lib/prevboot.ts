// The last crashed boot's report and log tail (GET /prevboot = /.rytm/prevboot.txt on the card, kernel.cpp CrumbToCard):
// paired devices only (unlike /log, which is open) - the api adds the pairing token as a header, so a plain link would be refused and
// the page fetches it. The file holds no pairing code or Wi-Fi key (the engine never logs them); fixed `paircode=`
// values and long digit runs after "code" are masked here as well, in case a log line ever carries one.
import type { Api } from "./api";

export type PrevBoot = { kind: "ok"; text: string; bytes: number } | { kind: "none" } | { kind: "error"; text: string };

export const maskSecrets = (t: string) =>
  t.replace(/(paircode\s*[=:]\s*)\d{4,}/gi, "$1********").replace(/(pairing code[^\n\d]{0,40})(?:\d{6,}|\d{4}[ -]?\d{4})/gi, "$1********");

export async function loadPrevBoot(api: Pick<Api, "request">): Promise<PrevBoot> {
  try {
    const r = await api.request("/prevboot");
    if (r.status === 404) return { kind: "none" };
    if (!r.ok) return { kind: "error", text: r.status === 401 || r.status === 403 ? "Pair this device first (Settings › Access)." : `The Sidecar answered ${r.status}.` };
    const t = maskSecrets(await r.text());
    return { kind: "ok", text: t, bytes: t.length };
  } catch (e) {
    return { kind: "error", text: (e as Error).message };
  }
}
