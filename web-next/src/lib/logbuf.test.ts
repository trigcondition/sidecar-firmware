// web-next/src/lib/logbuf.test.ts
import { describe, expect, it } from "vitest";
import { LogBuffer } from "./logbuf";

describe("LogBuffer", () => {
  it("joins a line split across pushes", () => {
    const b = new LogBuffer();
    expect(b.add({ from: 0, to: 9, gap: false, text: "one\ntw" }).map((l) => l.text)).toEqual(["one"]);
    expect(b.add({ from: 9, to: 15, gap: false, text: "o\nthree\n" }).map((l) => l.text)).toEqual(["two", "three"]);
    expect(b.bytes).toBe(15);
  });

  it("marks a gap and drops the half line before it", () => {
    const b = new LogBuffer();
    b.add({ from: 0, to: 4, gap: false, text: "hal" });
    const l = b.add({ from: 900, to: 910, gap: true, text: "new line\n" });
    expect(l[0].gap).toBe(true);
    expect(l[1].text).toBe("new line");
  });

  it("keeps at most max lines, and the stats lines", () => {
    const b = new LogBuffer(3);
    b.add({ from: 0, to: 1, gap: false, text: "a\nb\nc\nd\n1.0 engine: engine: fifo min 1 max 2 target 3 under 0\n" });
    expect(b.lines.map((l) => l.text)).toEqual(["c", "d", "1.0 engine: engine: fifo min 1 max 2 target 3 under 0"]);
    expect(b.stats.engine).toContain("fifo min 1");
  });
});
