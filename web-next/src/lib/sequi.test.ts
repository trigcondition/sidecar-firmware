// web-next/src/lib/sequi.test.ts - the Seq page's view helpers.
import { describe, expect, it } from "vitest";
import { lenText, microText, stepsText, tabMove } from "./sequi";

describe("sequi", () => {
  it("moves round a tab list as the ARIA tabs pattern does", () => {
    expect(tabMove(0, "ArrowRight", 3)).toBe(1);
    expect(tabMove(2, "ArrowRight", 3)).toBe(0);
    expect(tabMove(0, "ArrowLeft", 3)).toBe(2);
    expect(tabMove(1, "ArrowDown", 3)).toBe(2);
    expect(tabMove(1, "ArrowUp", 3)).toBe(0);
    expect(tabMove(1, "Home", 3)).toBe(0);
    expect(tabMove(0, "End", 3)).toBe(2);
    expect(tabMove(0, "Enter", 3)).toBeNull();
    expect(tabMove(0, "ArrowRight", 0)).toBeNull();
  });

  it("says a note's length and micro timing", () => {
    expect(lenText(96)).toBe("1 step");
    expect(lenText(24)).toBe("0.25 steps");
    expect(lenText(240)).toBe("2.5 steps");
    expect(microText(12)).toBe("+12/96");
    expect(microText(0)).toBe("0/96");
    expect(microText(-5)).toBe("-5/96");
  });

  it("says the steps on screen", () => {
    expect(stepsText(0, 16)).toBe("1–16");
    expect(stepsText(16, 16)).toBe("17–32");
    expect(stepsText(6, 1)).toBe("7");
  });
});
