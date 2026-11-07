// web-next/src/lib/api.test.ts - the HTTP helper's rules: the header, two at a time, 503 retries, pairing lost.
import { describe, expect, it } from "vitest";
import { Api, q } from "./api";

function resp(status: number, body = "{}", headers: Record<string, string> = {}): Response {
  return new Response(body, { status, headers });
}

describe("Api", () => {
  it("sends the token in the header with its case kept, never in the URL", async () => {
    const seen: { url: string; init: RequestInit }[] = [];
    const api = new Api({ token: () => "tok123", fetchFn: (async (url: string, init: RequestInit) => (seen.push({ url, init }), resp(200, '{"ok":true}'))) as unknown as typeof fetch });
    await api.post("/wifi/join", { ssid: "a", psk: "secret-pass" });
    expect(seen[0].url).toBe("/wifi/join");
    expect(seen[0].url).not.toContain("tok123");
    expect(seen[0].url).not.toContain("secret");
    expect((seen[0].init.headers as Record<string, string>)["X-Sidecar-Auth"]).toBe("tok123");
    expect(seen[0].init.body).toBe("ssid=a&psk=secret-pass");
  });

  it("keeps at most two requests in flight", async () => {
    let now = 0, peak = 0;
    const gates: (() => void)[] = [];
    const api = new Api({
      token: () => null,
      fetchFn: (() => {
        now++;
        peak = Math.max(peak, now);
        return new Promise<Response>((ok) => gates.push(() => (now--, ok(resp(200)))));
      }) as unknown as typeof fetch,
    });
    const all = [1, 2, 3, 4, 5].map(() => api.json("/x"));
    for (let i = 0; i < 5; i++) {
      await new Promise((r) => setTimeout(r, 0));
      gates.shift()?.();
    }
    await Promise.all(all);
    expect(peak).toBe(2);
  });

  it("tries a 503 again after 0.3, 0.6, 1.2 s, then gives up", async () => {
    const waits: number[] = [];
    let n = 0;
    const api = new Api({ token: () => null, sleep: async (ms) => void waits.push(ms), fetchFn: (async () => (n++, resp(503))) as unknown as typeof fetch });
    const r = await api.request("/x");
    expect(r.status).toBe(503);
    expect(n).toBe(4);
    expect(waits).toEqual([300, 600, 1200]);
  });

  it("reports a lost pairing", async () => {
    let lost = 0;
    const api = new Api({ token: () => "t", onPairLost: () => lost++, fetchFn: (async () => resp(200, "{}", { "X-Sidecar-Pair": "required" })) as unknown as typeof fetch });
    await api.json("/um");
    expect(lost).toBe(1);
  });

  it("q() encodes parameters", () => {
    expect(q("/fs/ls", { path: "/Samples/a b&c" })).toBe("/fs/ls?path=%2FSamples%2Fa+b%26c");
    expect(q("/x", { a: undefined })).toBe("/x");
  });
});
