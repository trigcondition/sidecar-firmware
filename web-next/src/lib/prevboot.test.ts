import { describe, expect, it } from "vitest";
import { Api } from "./api";
import { loadPrevBoot, maskSecrets } from "./prevboot";

const api = (r: () => Response) => new Api({ token: () => "t", fetchFn: (async () => r()) as unknown as typeof fetch });

describe("the crash report", () => {
  it("fetches /prevboot with the pairing token (never a bare link)", async () => {
    let seen: { url: string; auth: string | null } | null = null;
    const a = new Api({ token: () => "tok", fetchFn: (async (url: string, init?: RequestInit) => { seen = { url, auth: new Headers(init?.headers).get("X-Sidecar-Auth") }; return new Response("report\nlog\n"); }) as unknown as typeof fetch });
    expect(await loadPrevBoot(a)).toEqual({ kind: "ok", text: "report\nlog\n", bytes: 11 });
    expect(seen).toEqual({ url: "/prevboot", auth: "tok" });
  });
  it("says none for a card without one, and why when refused", async () => {
    expect(await loadPrevBoot(api(() => new Response("", { status: 404 })))).toEqual({ kind: "none" });
    expect(await loadPrevBoot(api(() => new Response("", { status: 401 })))).toMatchObject({ kind: "error", text: expect.stringMatching(/Pair this device/) });
    expect(await loadPrevBoot(api(() => new Response("", { status: 500 })))).toMatchObject({ kind: "error" });
  });
  it("masks a fixed pairing code if a line ever has one", () => {
    expect(maskSecrets("cmdline sdport=0 paircode=12345678 x")).toBe("cmdline sdport=0 paircode=******** x");
    expect(maskSecrets("the pairing code is 12345678")).toBe("the pairing code is ********");
    expect(maskSecrets("pairing code 1234 5678")).toBe("pairing code ********");   // the form the engine shows it in
    expect(maskSecrets("pairing code: 1234-5678 ok")).toBe("pairing code: ******** ok");
    expect(maskSecrets("uptime 12345678 s")).toBe("uptime 12345678 s");
  });
});
