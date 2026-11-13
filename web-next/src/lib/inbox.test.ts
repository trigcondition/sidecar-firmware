// web-next/src/lib/inbox.test.ts
import { describe, expect, it } from "vitest";
import { INBOX, isPoolAudio, takeCmd } from "./inbox";

describe("inbox", () => {
  it("takes .wav files, any case, and nothing else", () => {
    expect(isPoolAudio("Kick.WAV")).toBe(true);
    expect(isPoolAudio("a.wav")).toBe(true);
    expect(isPoolAudio("a.aif")).toBe(false);
    expect(isPoolAudio("wav")).toBe(false);
  });
  it("builds the pool command", () => {
    expect(INBOX).toBe("/Samples/Inbox");
    expect(takeCmd("/Samples/Inbox/k.wav")).toBe("pool=smp:take:/Samples/Inbox/k.wav");
  });
});
