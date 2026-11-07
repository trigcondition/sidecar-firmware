// web-next/src/lib/box.ts - the boxes on USB (/status "boxes": the Rytm, a Digitakt, an A4) and what the one in use
// can do: its feat bits gate parts of the page, as the old page's [data-needs] (remoteui 1, project 2, kits 4,
// patterns 8, sendfx 16, userslots 32, srcview 64, fxin 128, pools 256).
export const FEAT = { remoteui: 1, project: 2, kits: 4, patterns: 8, sendfx: 16, userslots: 32, srcview: 64, fxin: 128, pools: 256 } as const;
export type Feat = keyof typeof FEAT;

export interface Box { key?: string; id: string | number; name: string; n?: number; role: string; on: boolean | number; feat: number }

export function boxes(st: unknown): Box[] {
  const b = (st as { boxes?: unknown } | null)?.boxes;
  return Array.isArray(b) ? (b as Box[]) : [];
}
/** The box in use (the first "on"), or null: no box reported (an older build: everything shown). */
export function boxInUse(st: unknown): Box | null {
  const l = boxes(st);
  return l.find((x) => x.on) ?? null;
}
/** Whether the box in use has a feature; true when no box is reported, so an older build hides nothing. */
export function has(st: unknown, f: Feat): boolean {
  const b = boxInUse(st);
  return !b || (b.feat & FEAT[f]) !== 0;
}
