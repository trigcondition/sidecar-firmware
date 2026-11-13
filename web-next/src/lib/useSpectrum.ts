// web-next/src/lib/useSpectrum.ts - a strip's live spectrum for the page that shows it (the mixer's POST EQ): the
// "spectrum" topic subscribed only while the component is mounted, the tab visible and the server serves it, with the
// strip named in the sub; the bands eased with spectrum.ts's ballistics, one animation frame at a time while they move.
// Reduced motion: one frame a second, shown as it comes (no animation).
import { onScopeDispose, shallowRef, watch, type ShallowRef } from "vue";
import { Ballistics, FLOOR_DB, parseSpectrum } from "./spectrum";
import { useSidecar } from "./useSidecar";

export interface Spectrum {
  /** the bands as shown, dB (empty: nothing to show) */
  levels: ShallowRef<number[]>;
  hz: ShallowRef<[number, number]>;
}

export function useSpectrum(strip: () => string): Spectrum {
  const { client, hello } = useSidecar();
  const levels = shallowRef<number[]>([]);
  const hz = shallowRef<[number, number]>([20, 20000]);
  const b = new Ballistics();
  const reduced = typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches;
  let off: (() => void) | null = null;
  let raf = 0, last = 0;

  const frame = (t: number) => {
    raf = 0;
    const moving = b.step(last ? Math.min(100, t - last) : 16);
    last = t;
    levels.value = b.shown.slice();
    if (moving) raf = requestAnimationFrame(frame);
    else last = 0;
  };
  const onFrame = (p: unknown) => {
    const f = parseSpectrum(p);
    if (!f) return;
    hz.value = f.hz;
    // another browser picked another strip (one strip on the Sidecar): this one's bands fall away
    b.set(f.strip === strip() ? f.bands : f.bands.map(() => FLOOR_DB));
    if (reduced) {
      b.jump();
      levels.value = b.shown.slice();
    } else if (!raf) raf = requestAnimationFrame(frame);
  };
  const stop = () => {
    off?.();
    off = null;
    if (raf) cancelAnimationFrame(raf);
    raf = 0;
    last = 0;
    levels.value = [];
  };
  let on = ""; // the strip subscribed ("" none): a sync that changes nothing sends nothing
  const sync = () => {
    const visible = typeof document === "undefined" || document.visibilityState === "visible";
    const want = visible && hello.value?.topics.includes("spectrum") ? strip() : "";
    if (want === on) return;
    stop();
    on = want;
    if (want) off = client.subscribe("spectrum", onFrame, { ms: reduced ? 1000 : 50, strip: want });
  };

  watch([strip, hello], sync, { immediate: true });
  if (typeof document !== "undefined") document.addEventListener("visibilitychange", sync);
  onScopeDispose(() => {
    if (typeof document !== "undefined") document.removeEventListener("visibilitychange", sync);
    stop();
  });
  return { levels, hz };
}
