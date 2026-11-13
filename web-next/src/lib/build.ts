// web-next/src/lib/build.ts - does this page belong to the Sidecar it talks to? The app is served from the card
// (/next, docs/web-ws.md), so a kernel update leaves the old build there until it is copied again. The page knows
// the release it was built for (vite.config.ts, from VERSION); the Sidecar says its own in the hello. Same
// MAJOR.MINOR.PATCH: fine; anything else: a note saying which side is older. Pure (build.test.ts).
export interface WebBuild { version: string; commit: string; dirty: boolean }
export const WEB_BUILD: WebBuild = typeof __WEB_BUILD__ === "object" ? __WEB_BUILD__ : { version: "0.0.0", commit: "unknown", dirty: false };

/** MAJOR.MINOR.PATCH at the front of a version ("0.4.2-mock" -> [0, 4, 2]); null when there is none. */
export function semver(v: string | undefined | null): [number, number, number] | null {
  const m = /^(\d+)\.(\d+)\.(\d+)/.exec(v ?? "");
  return m ? [+m[1], +m[2], +m[3]] : null;
}
export type BuildState = "match" | "page-older" | "page-newer" | "unknown";
export function buildState(page: string, sidecar: string | undefined | null): BuildState {
  const a = semver(page), b = semver(sidecar);
  if (!a || !b) return "unknown";
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] < b[i] ? "page-older" : "page-newer";
  return "match";
}
export const buildText = (b: WebBuild) => `${b.version} (${b.commit}${b.dirty ? "-dirty" : ""})`;
/** The note for a mismatch ("" when there is none to make). */
export function buildNote(b: WebBuild, sidecar: string | undefined | null): string {
  const s = buildState(b.version, sidecar);
  if (s === "page-older") return `This page is from Sidecar ${b.version}; the Sidecar runs ${sidecar}. Put the ${semver(sidecar)!.join(".")} web app on the card (npm run card), or some controls may not work.`;
  if (s === "page-newer") return `This page is from Sidecar ${b.version}, newer than the Sidecar (${sidecar}). Some controls may not work until the Sidecar is updated.`;
  return "";
}
