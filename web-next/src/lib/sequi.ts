// web-next/src/lib/sequi.ts - the Seq page's small view helpers: a tab list's keys, the note inspector's value texts,
// the steps on screen. Pure (no Vue), so they are tested on their own (sequi.test.ts).
import { SUB } from "./seq";

/** A tab list's keys (the ARIA tabs pattern): the arrows move round, Home and End to the ends; null for other keys. */
export function tabMove(i: number, key: string, n: number): number | null {
  if (n <= 0) return null;
  if (key === "ArrowRight" || key === "ArrowDown") return (i + 1) % n;
  if (key === "ArrowLeft" || key === "ArrowUp") return (i - 1 + n) % n;
  if (key === "Home") return 0;
  if (key === "End") return n - 1;
  return null;
}

/** A note's length in steps (96 sub-steps a step): "1 step", "0.25 steps", "2.5 steps". */
export const lenText = (len: number) => {
  const s = +(len / SUB).toFixed(2);
  return `${s} ${s === 1 ? "step" : "steps"}`;
};

/** A note's micro timing, signed, in 96ths of a step: "+12/96", "0/96", "-5/96". */
export const microText = (m: number) => `${m > 0 ? "+" : ""}${m}/96`;

/** The steps on screen, 1-based: "1–16", or "7" for one. */
export const stepsText = (first: number, count: number) => (count <= 1 ? `${first + 1}` : `${first + 1}–${first + count}`);
