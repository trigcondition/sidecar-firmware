// web-next/src/lib/loop.test.ts
import { describe, expect, it } from "vitest";
import { balGains, balLine, calLine, fadeLine, grabLine, keepLine, levLine, liveLine, livePercent, loopView, muteLine, playLine, positionText, stopLine } from "./loop";

describe("loop", () => {
  it("reads /status's loop", () => {
    expect(loopView({})).toBeNull();
    expect(loopView({ loop: { on: 0 } })!.on).toBe(false);
    const v = loopView({
      loop: { on: 1, state: "playing", ring_s: 41, bal: 90, lev: 100, fade: 3, mute: 1, live: 4, post_lat: -12, pending: 1, bar: 2.5, bars: 4, msg: "KEPT take", msg_n: 3,
        grabs: [{ id: 9, hb: 8, bpm: 120.4, kept: 0, gap: 1, copied: 1, playing: 1 }, { id: 8, hb: 1, bpm: 98, kept: 1, gap: 0, copied: 0, playing: 0 }] },
    })!;
    expect([v.on, v.state, v.ringSeconds, v.bal, v.fade, v.mute, v.live, v.postLat, v.pending]).toEqual([true, "playing", 41, 90, 3, true, 4, -12, true]);
    expect(v.grabs[0]).toMatchObject({ id: 9, label: "4", bpm: 120.4, kept: false, gap: true, playing: true });
    expect(v.grabs[1]).toMatchObject({ label: "1/2", kept: true, copied: false });
    expect(positionText(v)).toBe("2.5 / 4 bars");
    expect(loopView({ loop: { on: 1, bar: 0.2, bars: 0.5 } })).not.toBeNull();
    expect(positionText(loopView({ loop: { on: 1 } })!)).toBe("");
  });
  it("writes the console's lines", () => {
    expect(grabLine(1)).toBe("loop=grab:0.5");
    expect(grabLine(32)).toBe("loop=grab:16");
    expect(grabLine(8)).toBe("loop=grab:4");
    expect(playLine(12)).toBe("loop=play:#12");
    expect(playLine(-3)).toBe("loop=play:#0");
    expect(stopLine()).toBe("loop=stop");
    expect(stopLine(true)).toBe("loop=stop:cut:now");
    expect(keepLine(9)).toBe("loop=keep:#9");
    expect(balLine(200)).toBe("loop=bal:127");
    expect(levLine(-4)).toBe("loop=lev:0");
    expect(fadeLine(0)).toBe("loop=fade:1/4");
    expect(fadeLine(4)).toBe("loop=fade:4");
    expect(muteLine(true)).toBe("loop=mute:on");
    expect(liveLine(0)).toBe("loop=live:off");
    expect(liveLine(12)).toBe("loop=live:perf12");
    expect(calLine).toBe("loop=cal");
  });
  it("shows the balance as spec 3.8 plays it", () => {
    expect(balGains(0)).toEqual({ loop: 0, own: 1 });
    expect(balGains(64)).toEqual({ loop: 1, own: 1 });
    expect(balGains(127)).toEqual({ loop: 1, own: 0 });
    const g = balGains(32);
    expect(g.loop ** 2).toBeCloseTo(0.5, 2);
    expect(livePercent(64)).toBe(100);
    expect(livePercent(96)).toBe(50); // the macro's 63 of 127
    expect(livePercent(127)).toBe(0);
  });
});
