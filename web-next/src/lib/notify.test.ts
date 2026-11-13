import { describe, expect, it } from "vitest";
import { errText } from "./notify";
import { RequestError } from "./client";

describe("errText", () => {
  it("names the Sidecar's refusal with its code", () => {
    expect(errText(new RequestError(401, "not paired"))).toBe("Refused (401): not paired");
  });
  it("takes an error's message", () => {
    expect(errText(new Error("timed out"))).toBe("timed out");
  });
  it("turns anything else into text", () => {
    expect(errText("busy")).toBe("busy");
  });
});
