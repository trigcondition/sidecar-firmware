// web-next/src/lib/pending.test.ts
import { describe, expect, it } from "vitest";
import { Pending } from "./pending";

function rig(refuse = false) {
  let t = 0, dev = "main";
  const sent: string[] = [];
  const p = new Pending<string>({
    read: () => dev,
    toCmd: (v) => `out=${v}`,
    send: async (c) => {
      sent.push(c);
      if (refuse) throw new Error("403 refused");
    },
    now: () => t,
  });
  return { p, sent, setDev: (v: string) => (dev = v), at: (ms: number) => (t = ms) };
}

describe("Pending", () => {
  it("shows the choice at once, saved when the status agrees, then idle", async () => {
    const r = rig();
    await r.p.set("tracks");
    expect(r.sent).toEqual(["out=tracks"]);
    expect(r.p.state).toBe("pending");
    expect(r.p.value).toBe("tracks");
    r.setDev("tracks");
    r.at(300);
    r.p.check();
    expect(r.p.state).toBe("saved");
    r.at(2000);
    r.p.check();
    expect(r.p.state).toBe("idle");
    expect(r.p.value).toBe("tracks");
  });

  it("falls back to what the Sidecar has after the timeout", async () => {
    const r = rig();
    await r.p.set("tracks");
    r.at(2100);
    r.p.check();
    expect(r.p.state).toBe("failed");
    expect(r.p.value).toBe("main");
    expect(r.p.msg).toMatch(/did not take it/);
  });

  it("a refused command fails at once with its reason", async () => {
    const r = rig(true);
    await r.p.set("tracks");
    expect(r.p.state).toBe("failed");
    expect(r.p.msg).toContain("403 refused");
    expect(r.p.value).toBe("main");
  });
});
