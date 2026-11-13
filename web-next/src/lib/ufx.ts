// web-next/src/lib/ufx.ts - the user effects (Plugin API 3) UFX1..UFX4 on the Voices › User machines card, from the
// host's /um rows (core/user/um.cpp publish): "plugin" rows with effect:true are the installed effects, "ufx" rows the
// slots that have one assigned, the "plugins" row's ufx_slots says this build has the slots. A change is the console's
// um=fx:<n>:<id|-> (the Rytm's USER SLOTS page sends the same).
export interface UmRow { t: string; [k: string]: any }
export interface EffectChoice { id: string; name: string; ver: string }
export interface UfxSlot { n: number; id: string; name: string; state: string; missing: boolean; muted: boolean; why: string }

const UFX = 4;

/** The host has the user effects (older builds do not say: the section stays hidden). */
export function ufxAvailable(rows: UmRow[]): boolean {
  const i = rows.find((r) => r.t === "plugins");
  return !!i && Number(i.ufx_slots) > 0;
}
/** The installed effects a UFX slot can play: loadable, the newest version of each id. */
export function effectChoices(rows: UmRow[]): EffectChoice[] {
  return rows
    .filter((r) => r.t === "plugin" && r.effect && r.fit && !r.older)
    .map((r) => ({ id: String(r.id), name: String(r.name ?? r.id), ver: String(r.ver ?? "") }));
}
/** The packages a machine slot (U1..U5) can play: not the effects. */
export function machineChoices(rows: UmRow[]): UmRow[] {
  return rows.filter((r) => r.t === "plugin" && !r.effect);
}
/** UFX1..UFX4 as the host reports them; a slot with nothing assigned has no row: EMPTY. */
export function ufxSlots(rows: UmRow[]): UfxSlot[] {
  const out: UfxSlot[] = [];
  for (let n = 1; n <= UFX; ++n) {
    const r = rows.find((x) => x.t === "ufx" && Number(x.ufx) === n);
    if (!r || !r.id) {
      out.push({ n, id: "", name: "", state: "EMPTY", missing: false, muted: false, why: "" });
      continue;
    }
    const id = String(r.id);
    const p = rows.find((x) => x.t === "plugin" && x.effect && x.id === id && !x.older);
    out.push({
      n,
      id,
      name: p ? String(p.name ?? id) : id.slice(id.lastIndexOf(".") + 1),
      state: String(r.state ?? ""),
      missing: r.state === "MISSING",
      muted: !!r.muted,
      why: String(r.why ?? ""),
    });
  }
  return out;
}
/** The console command for UFX n (1..4): id "" empties it; null when n or the id cannot be one (never sent). */
export function ufxCommand(n: number, id: string): string | null {
  if (!Number.isInteger(n) || n < 1 || n > UFX) return null;
  if (id === "") return `um=fx:${n}:-`;
  return /^[A-Za-z0-9._-]{1,31}$/.test(id) ? `um=fx:${n}:${id}` : null;
}
