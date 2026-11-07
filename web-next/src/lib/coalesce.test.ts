// web-next/src/lib/coalesce.test.ts
import { describe, expect, it } from "vitest";
import { coalescer, controlOf } from "./coalesce";

describe("coalescer", () => {
  it("finds the control of a command", () => {
    expect(controlOf("ins=V1:eq:hpf:40")).toBe("ins=V1:eq:hpf");
    expect(controlOf("mfx=trim:-12")).toBe("mfx=trim");
    expect(controlOf("out=V2:lev:100")).toBe("out=V2:lev");
    expect(controlOf("fifo=96")).toBe("fifo");
    expect(controlOf("killfx")).toBe("killfx");
  });

  it("sends the first value, keeps only the newest while it is on its way, then sends that", async () => {
    const sent: string[] = [];
    const gates: (() => void)[] = [];
    const send = coalescer((c) => (sent.push(c), new Promise<void>((ok) => gates.push(ok))));
    for (let v = 1; v <= 30; v++) void send(`ins=V1:eq:hpf:${v}`);
    void send("ins=V1:eq:lpf:5"); // another control: its own lane
    expect(sent).toEqual(["ins=V1:eq:hpf:1", "ins=V1:eq:lpf:5"]);
    gates.shift()!();
    await new Promise((r) => setTimeout(r, 0));
    expect(sent).toEqual(["ins=V1:eq:hpf:1", "ins=V1:eq:lpf:5", "ins=V1:eq:hpf:30"]);
    gates.forEach((g) => g());
    await new Promise((r) => setTimeout(r, 0));
    gates.forEach((g) => g());
    expect(sent.length).toBe(3);
  });

  it("goes on after a refusal", async () => {
    const errs: unknown[] = [];
    let n = 0;
    const send = coalescer(async () => {
      if (n++ === 0) throw new Error("429");
    }, (e) => errs.push(e));
    await send("out=V1:lev:1");
    await send("out=V1:lev:2");
    expect(errs).toHaveLength(1);
    expect(n).toBe(2);
  });
});
