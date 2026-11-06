// web-next/src/lib/tiles.test.ts - the Home tiles against real stats lines and /status members.
import { describe, expect, it } from "vitest";
import { STATS_LINE, boxName, buildTiles, takeStats } from "./tiles";

const ENGINE = "12.345 engine: engine: fifo min 0 max 118 target 96 under 3 (restart 0, streams 1) | blocks 3000 max 210 us | cpu c1 41.5 c2 33.0 c3 19.2 % | pass max 260 us | midi 0 B (lost 0) sysex 0 notes 0 | to-host sent 0 B 0 msgs fail 0 dropped 0 | peak 512/1000 | 58 C";
const OB = "12.400 rytmob: 1000ms IN 1000/s gap 2 lost 0 late 0";
const SC = "12.401 sidecar: sidecar alt 1/1 · late 0/0/1";

describe("tiles", () => {
  it("keeps the newest stats line of each source", () => {
    const s = takeStats(["x", ENGINE, OB, "noise", SC]);
    expect(s.engine).toBe(ENGINE);
    expect(s.rytmob).toBe(OB);
    expect(s.sidecar).toBe(SC);
    expect(STATS_LINE.test(ENGINE) && STATS_LINE.test(OB) && STATS_LINE.test(SC) && !STATS_LINE.test("web: command :x")).toBe(true);
  });

  it("reads CPU and FIFO from the engine line, the rest from /status", () => {
    const t = buildTiles(
      { streaming: true, link: "usb", rytmMidi: true, gadget: "sidecar", mac: true, usbHost: true, temp: 61, underruns: 0, restartUnderruns: 0, streams: 1, cpu: { mhz: 1800, budgetUs: 340 }, power: { known: true, warn: "", maxCurrent: 3000 }, web: { requests: 9, maxus: 800, commands: 2 } },
      takeStats([ENGINE, OB, SC]),
    );
    const by = Object.fromEntries(t.map((x) => [x.key, x]));
    expect(by.cpu.value).toBe("41.5 / 33.0 / 19.2 %");
    expect(by.cpu.detail).toContain("1800 MHz, voice budget 340 us");
    expect(by.fifo.value).toBe("0..118");
    expect(by.fifo.tone).toBe("bad"); // min 0: the FIFO ran dry
    expect(by.ob.detail).toEqual(["IN 1000/s", "gap 2", "lost 0", "link usb"]);
    expect(by.ob.tone).toBe("bad"); // a gap
    expect(by.gadget.detail).toEqual(["alt 1/1", "computer audio on", "late 0/0/1"]);
    expect(by.temp.value).toBe("61 °C");
    expect(by.power.tone).toBe("good");
    expect(by.under.tone).toBe("good");
  });

  it("works with an empty status and no log yet", () => {
    const t = buildTiles(null, {});
    expect(t.map((x) => x.key)).toEqual(["cpu", "fifo"]);
    expect(t[0].value).toBe("–");
  });

  it("names the attached box", () => {
    expect(boxName({ boxes: [{ n: "Digitakt", on: true }] })).toBe("Digitakt");
    expect(boxName(null)).toBe("Rytm");
  });
});
