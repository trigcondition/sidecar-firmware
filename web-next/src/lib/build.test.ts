// web-next/src/lib/build.test.ts
import { describe, expect, it } from "vitest";
import { WEB_BUILD, buildNote, buildState, buildText, semver } from "./build";

describe("build", () => {
  it("reads versions", () => {
    expect(semver("0.4.2-mock")).toEqual([0, 4, 2]);
    expect(semver("v1")).toBeNull();
    expect(semver(undefined)).toBeNull();
  });
  it("compares the page with the Sidecar", () => {
    expect(buildState("0.4.2", "0.4.2-mock")).toBe("match");
    expect(buildState("0.4.2", "0.5.0")).toBe("page-older");
    expect(buildState("0.4.10", "0.4.9")).toBe("page-newer");
    expect(buildState("0.4.2", "")).toBe("unknown");
  });
  it("says so", () => {
    const b = { version: "0.4.2", commit: "abc1234", dirty: true };
    expect(buildText(b)).toBe("0.4.2 (abc1234-dirty)");
    expect(buildNote(b, "0.4.2")).toBe("");
    expect(buildNote(b, "0.5.0")).toMatch(/runs 0\.5\.0\. Put the 0\.5\.0 web app on the card/);
    expect(buildNote(b, "0.4.1")).toMatch(/newer than the Sidecar \(0\.4\.1\)/);
    expect(buildNote(b, undefined)).toBe("");
  });
  it("is stamped at build time", () => {
    expect(semver(WEB_BUILD.version)).not.toBeNull();
    expect(WEB_BUILD.commit).toMatch(/^[0-9a-f]{7}$|^unknown$/);
  });
});
