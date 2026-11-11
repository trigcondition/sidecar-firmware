// web-next/src/lib/tiles.test.ts - the stats lines Home reads and the box's name, against real lines and /status rows.
import { describe, expect, it } from "vitest";
import { STATS_LINE, boxName, shortBoxName, takeStats } from "./tiles";
import { ENGINE, IDLE, OB, SC } from "./stats.fixture";

describe("tiles", () => {
  it("keeps the newest stats line of each source", () => {
    const s = takeStats(["x", ENGINE, IDLE, OB, "noise", SC]);
    expect(s).toEqual({ engine: ENGINE, idle: IDLE, rytmob: OB, sidecar: SC });
    expect(STATS_LINE.test(ENGINE) && STATS_LINE.test(OB) && STATS_LINE.test(SC) && STATS_LINE.test(IDLE) && !STATS_LINE.test("web: command :x")).toBe(true);
  });

  it("names the attached box from its USB product string, not its count", () => {
    // as pi/engine/box_list.h writes a row: n is how many of that type, not a name (the Pi showed "1")
    expect(boxName({ boxes: [{ key: "2", id: 2, n: 1, name: "Elektron Analog Rytm MKII", role: "host", on: 1 }] })).toBe("Rytm");
    expect(boxName({ boxes: [{ id: 3, n: 1, name: "Elektron Digitakt II", on: 0 }, { id: 4, n: 1, name: "Elektron Analog Four MKII", on: 1 }] })).toBe("Analog Four");
    expect(boxName({ boxes: [{ id: 2, n: 1, name: "", on: 1 }] })).toBe("Rytm");
    expect(boxName(null)).toBe("Rytm");
    expect(shortBoxName("Elektron Digitone")).toBe("Digitone");
  });
});
