// web-next/src/styles/contrast.test.ts - WCAG 2.x contrast of the token pairs, read from tokens.css itself: text
// 4.5:1, UI components and the focus ring 3:1, in both themes. Change a token and this says whether it still reads.
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";

const css = readFileSync(new URL("./tokens.css", import.meta.url), "utf8");

/** The custom properties of the first rule whose selector contains `sel`. */
function block(sel: string): Record<string, string> {
  const re = /([^{}]+)\{([^{}]*)\}/g;
  for (let m; (m = re.exec(css)); ) {
    if (!m[1].includes(sel)) continue;
    const vars: Record<string, string> = {};
    for (const d of m[2].matchAll(/--([\w-]+)\s*:\s*([^;]+);/g)) vars[d[1]] = d[2].trim();
    return vars;
  }
  throw new Error(`no rule for ${sel}`);
}

const standard = block(':root[data-theme="standard"]');
const themes: Record<string, Record<string, string>> = {
  standard,
  contrast: { ...standard, ...block(':root[data-theme="contrast"]') },
};

function rgb(hex: string): [number, number, number] {
  const m = /^#([0-9a-f]{6})$/i.exec(hex);
  if (!m) throw new Error(`not a #rrggbb colour: ${hex}`);
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
export function luminance(hex: string): number {
  const [r, g, b] = rgb(hex).map((c) => {
    const s = c / 255;
    return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
export function ratio(a: string, b: string): number {
  const [x, y] = [luminance(a), luminance(b)].sort((p, q) => q - p);
  return (x + 0.05) / (y + 0.05);
}

const TEXT = 4.5, UI = 3;
const pairs: [string, string, number][] = [
  ["text", "bg", TEXT], ["text", "card", TEXT],
  ["label", "bg", TEXT], ["label", "card", TEXT],
  ["accent", "bg", TEXT], ["accent", "card", TEXT],
  ["warm", "bg", TEXT], ["warm", "card", TEXT],
  ["on-accent", "accent", TEXT], ["on-accent", "warm", TEXT], ["bg", "text", TEXT],
  ["control-border", "bg", UI], ["control-border", "card", UI],
  ["focus", "bg", UI], ["focus", "card", UI],
];

describe("the maths", () => {
  it("matches WCAG's reference points", () => {
    expect(ratio("#000000", "#ffffff")).toBeCloseTo(21, 5);
    expect(ratio("#777777", "#ffffff")).toBeCloseTo(4.48, 2);
    expect(ratio("#1f242b", "#0b0d10")).toBeLessThan(1.3); // why --border is decorative only
  });
});

for (const [name, t] of Object.entries(themes)) {
  describe(`theme ${name}`, () => {
    for (const [fg, bg, min] of pairs) {
      it(`--${fg} on --${bg} >= ${min}:1`, () => {
        expect(t[fg], `--${fg} missing`).toBeDefined();
        expect(ratio(t[fg], t[bg])).toBeGreaterThanOrEqual(min);
      });
    }
  });
}

describe("sizes", () => {
  it("no text token below 13px, targets at least 44px", () => {
    const rem = (v: string) => parseFloat(v) * 16;
    expect(rem(standard["text-min"])).toBeGreaterThanOrEqual(13);
    expect(rem(standard["label-size"])).toBeGreaterThanOrEqual(13);
    expect(rem(standard["target"])).toBeGreaterThanOrEqual(44);
  });
  it("high contrast: no textures, no glow, 2px control borders", () => {
    expect(themes.contrast["card-texture"]).toBe("none");
    expect(themes.contrast["accent-glow"]).toBe("none");
    expect(themes.contrast["control-border-width"]).toBe("2px");
  });
});
