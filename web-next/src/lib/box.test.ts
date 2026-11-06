// web-next/src/lib/box.test.ts
import { describe, expect, it } from "vitest";
import { boxInUse, has } from "./box";

describe("box", () => {
  it("gates by the box in use's feat bits", () => {
    const st = { boxes: [{ id: "a4", name: "Analog Four", role: "not used", on: false, feat: 0 }, { id: "rytm", name: "Analog Rytm MKII", n: "Rytm", role: "in use", on: true, feat: 16 | 256 }] };
    expect(boxInUse(st)?.id).toBe("rytm");
    expect(has(st, "sendfx") && has(st, "pools")).toBe(true);
    expect(has(st, "userslots")).toBe(false);
    expect(has({}, "userslots")).toBe(true); // no boxes reported: an older build hides nothing
  });
});
