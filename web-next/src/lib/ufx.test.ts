// web-next/src/lib/ufx.test.ts
import { describe, expect, it } from "vitest";
import { effectChoices, machineChoices, ufxAvailable, ufxCommand, ufxSlots } from "./ufx";

const rows = [
  { t: "plugin", id: "rytm.nusaw", name: "NUSAW", ver: "1.0.0", fit: true },
  { t: "plugin", id: "fx.phaser", name: "PHASER", ver: "1.0.0", fit: true, effect: true },
  { t: "plugin", id: "fx.bbd", name: "BBD", ver: "1.2.0", fit: true, effect: true },
  { t: "plugin", id: "fx.bbd", name: "BBD", ver: "1.0.0", fit: true, effect: true, older: true },
  { t: "plugin", id: "fx.bad", name: "BAD", ver: "1.0.0", fit: false, effect: true, why: "api" },
  { t: "ufx", ufx: 2, id: "fx.bbd", state: "READY", muted: false },
  { t: "ufx", ufx: 3, id: "fx.gone", state: "MISSING", muted: false },
  { t: "ufx", ufx: 4, id: "fx.phaser", state: "READY", muted: true, why: "late" },
  { t: "plugins", n: 5, project: "P1", ufx_slots: 4 },
];

describe("ufx", () => {
  it("is available only when the host reports the user-effect slots", () => {
    expect(ufxAvailable(rows)).toBe(true);
    expect(ufxAvailable(rows.filter((r) => r.t !== "plugins"))).toBe(false); // an older build: no Effects section
    expect(ufxAvailable([{ t: "plugins", n: 1, ufx_slots: 0 }])).toBe(false);
    expect(ufxAvailable([])).toBe(false);
  });
  it("lists the installed effects (the newest, loadable ones), machines apart", () => {
    expect(effectChoices(rows).map((e) => e.id)).toEqual(["fx.phaser", "fx.bbd"]);
    expect(machineChoices(rows).map((p) => p.id)).toEqual(["rytm.nusaw"]);
  });
  it("gives UFX1..UFX4 with the effect each plays, empty ones included", () => {
    const s = ufxSlots(rows);
    expect(s.map((x) => x.n)).toEqual([1, 2, 3, 4]);
    expect(s[0]).toMatchObject({ id: "", name: "", state: "EMPTY", missing: false });
    expect(s[1]).toMatchObject({ id: "fx.bbd", name: "BBD", state: "READY", missing: false });
    expect(s[2]).toMatchObject({ id: "fx.gone", name: "gone", missing: true }); // not on the card: the id's last part
    expect(s[3]).toMatchObject({ id: "fx.phaser", name: "PHASER", muted: true, why: "late" });
  });
  it("words the console's command, the same path as um=fx", () => {
    expect(ufxCommand(1, "fx.phaser")).toBe("um=fx:1:fx.phaser");
    expect(ufxCommand(4, "")).toBe("um=fx:4:-");
    expect(ufxCommand(0, "fx.x")).toBeNull();
    expect(ufxCommand(5, "fx.x")).toBeNull();
    expect(ufxCommand(2, "fx.a:b")).toBeNull(); // not an id: never sent
    expect(ufxCommand(2, "a b")).toBeNull();
  });
});
