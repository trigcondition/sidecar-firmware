import { describe, expect, it, vi } from "vitest";
import { effectScope, nextTick, ref } from "vue";
import { useHeld } from "./held";
import { refusals } from "./notify";

vi.mock("vue-sonner", () => ({ toast: Object.assign(() => 0, { success: () => 0, error: () => 0, warning: () => 0 }) }));

function rig(start: number) {
  const src = ref(start);
  const scope = effectScope();
  const h = scope.run(() => useHeld(() => src.value, 1000))!;
  return { src, h, scope };
}

describe("useHeld", () => {
  it("shows the sent value at once and keeps it through a stale status", async () => {
    const { src, h } = rig(1);
    h.hold(2);
    expect(h.shown.value).toBe(2);
    src.value = 3; // an in-between value: still held
    await nextTick();
    expect(h.shown.value).toBe(2);
    src.value = 2; // confirmed: the Sidecar's value from now on
    await nextTick();
    src.value = 5;
    await nextTick();
    expect(h.shown.value).toBe(5);
  });
  it("lets go after the hold time", async () => {
    vi.useFakeTimers();
    const { h } = rig(1);
    h.hold(2);
    vi.advanceTimersByTime(999);
    expect(h.shown.value).toBe(2);
    vi.advanceTimersByTime(2);
    expect(h.shown.value).toBe(1);
    vi.useRealTimers();
  });
  it("lets go on a refusal", async () => {
    const { h } = rig(1);
    h.hold(2);
    refusals.value++;
    await nextTick();
    expect(h.shown.value).toBe(1);
  });
  it("holds nothing when the value is the Sidecar's already", () => {
    const { src, h } = rig(4);
    h.hold(4);
    src.value = 6;
    expect(h.shown.value).toBe(6);
  });
});
