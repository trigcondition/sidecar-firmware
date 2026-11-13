// web-next/src/lib/scene.test.ts
import { describe, expect, it } from "vitest";
import { SUGGEST, abLine, clearLine, copyLine, editLine, faderLine, lockLabel, lockLine, morphed, sceneView, selLine, suggest, targetName, unlockLine, valueText } from "./scene";

describe("scene", () => {
  it("reads /status's scene (1-based there)", () => {
    expect(sceneView({})).toBeNull();
    const v = sceneView({ scene: { a: 3, b: 0, fader: 8192, played: 8000, sel: 3, locked: 0, edit: 1, s: [{ n: 3, l: [["V1:P3", 6400]] }], src: 3, rscn: 0 } })!;
    expect([v.a, v.b, v.sel, v.edit, v.played, v.src, v.rscn]).toEqual([2, -1, 2, true, 8000, 3, 0]);
    expect(v.locks[2]).toEqual([{ name: "V1:P3", v: 6400 }]);
    expect(v.locks[0]).toEqual([]);
  });
  it("writes the console's lines", () => {
    expect(abLine(2, -1)).toBe("scene=ab:3:0");
    expect(faderLine(20000)).toBe("scene=fader:16383");
    expect(selLine(11)).toBe("scene=sel:12");
    expect(clearLine(0)).toBe("scene=clear:1");
    expect(copyLine(0, 4)).toBe("scene=copy:1:5");
    expect(editLine(true)).toBe("scene=edit:on");
    expect(lockLine(2, "V1:P3", 6400)).toBe("scene=lock:3:V1:P3:50");
    expect(lockLine(0, "MFX:LEV", 16383)).toBe("scene=lock:1:MFX:LEV:127.99");
    expect(unlockLine(2, "V1:P3")).toBe("scene=unlock:3:V1:P3");
    expect(valueText(64)).toBe("0.5");
  });
  it("checks a target name and morphs", () => {
    expect(targetName(" v3:p5 ")).toBe("V3:P5");
    expect(targetName("RYTM:PERF3")).toBe("RYTM:PERF3");
    expect(targetName("V3")).toBeNull();
    expect(targetName("V3:P5:1")).toBeNull();
    expect(morphed(0, 16383, 16383)).toBe(16383);
    expect(morphed(1000, 3000, 8192)).toBe(2000);
    expect(morphed(1000, undefined, 0)).toBeNull();
  });

  it("names a locked param as its machine does, and lists the targets by name (2026-11)", () => {
    const v = sceneView({ scene: { a: 1, b: 2, fader: 0, played: 0, sel: 1, locked: 0, edit: 0, s: [{ n: 1, l: [["V1:P2", 640], ["MFX:LEV", 0]] }],
      pl: { "V1:P2": "V1:HRM" }, vl: [["ENG", "HRM", "", ...Array(13).fill("")]] } });
    expect(lockLabel(v, "V1:P2")).toBe("V1:HRM");
    expect(lockLabel(v, "MFX:LEV")).toBe("MFX:LEV");
    expect(lockLabel(null, "V1:P2")).toBe("V1:P2");
    const t = suggest(v?.vl);
    expect(t.slice(0, 2)).toEqual([{ value: "V1:P1", label: "V1:P1 ENG" }, { value: "V1:P2", label: "V1:P2 HRM" }]);
    expect(t.find((o) => o.value === "V1:P3")).toBeUndefined(); // unused on its machine: not offered
    expect(suggest().filter((o) => o.value[0] === "V").length).toBe(64); // an older Pi: P1..P8 by number
    expect(SUGGEST).not.toContain("FX1:P1"); // send FX params are P2..P5
  });
});
