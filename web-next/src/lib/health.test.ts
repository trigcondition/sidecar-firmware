// web-next/src/lib/health.test.ts - Home's model: the health pills and the tiles' figures, healthy and warning.
import { describe, expect, it } from "vitest";
import { pills, pushSample, readEngine, readOb, readPower, readUsbC, sparkPoints } from "./health";
import { takeStats } from "./tiles";
import { ENGINE, IDLE, OB, SC } from "./stats.fixture";

const OK = {
  streaming: true, link: "usb", rytmMidi: true, gadget: "sidecar", mac: true, usbHost: true, temp: 52, tempWarn: false, clockLow: 0,
  underruns: 0, restartUnderruns: 1, streams: 1, cpu: { mhz: 2400, budgetUs: 125 },
  power: { known: true, warn: null, maxCurrent: 5000, uvNow: false, uvBoot: false, note: null },
  boxes: [{ id: 2, n: 1, name: "Elektron Analog Rytm MKII", on: 1 }],
};
const WEAK = { ...OK, underruns: 3, temp: 81, tempWarn: true, power: { known: true, warn: "POWER TOO WEAK: use 27 W adapter", maxCurrent: 1500, uvNow: true, uvBoot: true, note: null } };

describe("health", () => {
  it("reads the four cores, block and pass times and the FIFO", () => {
    const e = readEngine(OK, takeStats([ENGINE, IDLE]));
    expect(e.cores).toEqual([12, 41.5, 33, 19.2]); // core 0: 100 - asleep 88
    expect(e.busiest).toBe(41.5);
    expect([e.blockMax, e.passMax, e.budget, e.mhz]).toEqual([210, 260, 125, 2400]);
    expect(e.fifo).toEqual({ min: 0, max: 118, target: 96 });
    const none = readEngine(null, {});
    expect(none.cores).toEqual([null, null, null, null]);
    expect(none.busiest).toBeNull();
  });

  it("says each area in a short pill, healthy", () => {
    const e = readEngine(OK, takeStats([ENGINE.replace("max 210 us", "max 96 us")]));
    const p = pills(OK, e);
    expect(p.map((x) => x.text)).toEqual(["Audio ok", "CPU 42 %", "52 °C", "Power ok", "Rytm connected"]);
    expect(p.some((x) => x.warn)).toBe(false);
  });

  it("turns the pills that need a look coral, in words too", () => {
    const e = readEngine(WEAK, takeStats([ENGINE])); // block max 210 over the 125 us budget
    const p = Object.fromEntries(pills(WEAK, e).map((x) => [x.key, x]));
    expect(p.audio).toMatchObject({ text: "3 underruns", warn: true });
    expect(p.cpu.warn).toBe(true);
    expect(p.cpu.spoken).toContain("over the voice budget");
    expect(p.temp).toMatchObject({ text: "81 °C", warn: true });
    expect(p.power).toMatchObject({ text: "Power too weak", warn: true });
    expect(p.box.warn).toBe(false);
    expect(pills({ ...OK, streaming: false, rytmMidi: false }, e).find((x) => x.key === "box")!.text).toBe("Rytm not seen");
    expect(pills({ underruns: 1 }, readEngine(null, {})).map((x) => x.text)).toEqual(["1 underrun"]);
  });

  it("reads Overbridge, USB-C and power for their tiles", () => {
    expect(readOb(OK, takeStats([OB]))).toEqual({ streaming: true, link: "usb", rate: 1000, gap: 2, lost: 0, bad: true });
    expect(readOb({}, {})).toBeNull();
    expect(readUsbC(OK, takeStats([SC]))).toEqual({ mode: "Sidecar audio", detail: "computer audio on", more: ["interface alt 1/1", "late 0/0/1"] });
    expect(readUsbC({ ...OK, usbHost: false }, {})!.detail).toBe("no computer on USB-C");
    expect(readPower(OK)).toEqual({ chip: "ok", amps: 5, uv: "no under-voltage", more: [] });
    expect(readPower(WEAK)).toMatchObject({ chip: "too weak", amps: 1.5, uv: "under-voltage now" });
    expect(readPower({ power: { known: false, maxCurrent: null } })).toMatchObject({ chip: "unknown", amps: null });
  });

  it("keeps a minute of samples and draws them", () => {
    let a: number[] = [];
    for (let i = 0; i < 70; i++) a = pushSample(a, i);
    expect(a.length).toBe(60);
    expect(a[0]).toBe(10);
    expect(sparkPoints([0, 100], 120, 28)).toBe("0,28 120,0");
    expect(sparkPoints([50], 120, 28)).toBe("0,28 120,28");
    expect(sparkPoints([0, 150, 50], 100, 10)).toBe("0,10 50,0 100,5");
  });
});
