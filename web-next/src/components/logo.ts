// web-next/src/components/logo.ts - the "Docked" mark's geometry (viewBox 0 0 46 40): a 7 x 7 dot grid faded out
// from a centre, and a solid disc docked beside it; the dots' colours (graded, and the animated drift). Shared by
// Logo.vue and the favicon (scripts/favicon.mjs).
export const LOGO_VIEWBOX = "0 0 46 40";
export const LOGO_CENTRE = { x: 17.8, y: 19.8 };
export const LOGO_REACH = 15.5;
export const LOGO_DISC = { cx: 37, cy: 27, r: 7.5 };

export interface LogoDot {
  x: number;
  y: number;
  r: number;
  /** distance to the centre, 0..LOGO_REACH (the breathing ripple's delay) */
  d: number;
}

/** small: an icon under 40 px, where the outer dots get a larger floor so they do not vanish */
export function logoDots(small = false): LogoDot[] {
  const base = small ? 0.8 : 0.5;
  const dots: LogoDot[] = [];
  for (let gy = 0; gy <= 6; gy++)
    for (let gx = 0; gx <= 6; gx++) {
      const x = 4 + gx * 4.6, y = 6 + gy * 4.6;
      const d = Math.hypot(x - LOGO_CENTRE.x, y - LOGO_CENTRE.y);
      if (d < LOGO_REACH) dots.push({ x: +x.toFixed(2), y: +y.toFixed(2), r: +(base + 1.7 * (1 - d / LOGO_REACH)).toFixed(3), d });
    }
  return dots;
}

/** The dots' colours are tokens --logo-1..5 (coral, magenta, accent, lavender, white; all white in high contrast). A
 * dot's still colour: graded by its distance from the centre, coral in the middle out to lavender at the rim. */
export const LOGO_COLOURS = 5;
export function gradeColour(d: number): number {
  const f = d / LOGO_REACH;
  return f < 0.3 ? 1 : f < 0.55 ? 2 : f < 0.8 ? 3 : 4;
}

/** The orders the animated dots drift through (Logo.vue's keyframes drift0..3): each starts on a different colour and
 * visits all five, so neighbours seldom match. */
export const DRIFT_ORDERS = [[1, 2, 3, 4, 5], [3, 5, 1, 4, 2], [2, 4, 1, 5, 3], [4, 1, 3, 2, 5]] as const;

/** A dot's drift: which order, how long each fade lasts (6..14 s) and where in its cycle it starts. Seeded by the dot's
 * index, so it looks random but is the same on every load (and in the shots). */
export interface Drift { order: number; fade: number; phase: number }
export function dotDrift(i: number): Drift {
  let s = (i + 1) * 0x9e3779b9;
  const rnd = () => {
    s = Math.imul(s ^ (s >>> 15), 0x2c1b3c6d) ^ Math.imul(s ^ (s >>> 13), 0x297a2d39);
    return ((s ^ (s >>> 16)) >>> 0) / 4294967296;
  };
  const order = Math.floor(rnd() * DRIFT_ORDERS.length), fade = +(6 + rnd() * 8).toFixed(2);
  return { order, fade, phase: +(rnd() * fade * LOGO_COLOURS).toFixed(2) };
}

/** The favicon (a file: it cannot read the page's tokens, so scripts/favicon.mjs passes the standard theme's from
 * tokens.css): the graded dots and the disc on a dark rounded tile, so it shows on light tabs too. Still: no drift. */
export function faviconSvg(c: { bg: string; accent: string; logo: string[] }): string {
  const dots = logoDots(true).map((p) => `<circle cx="${p.x}" cy="${p.y}" r="${p.r}" fill="${c.logo[gradeColour(p.d) - 1]}"/>`).join("");
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${LOGO_VIEWBOX}"><rect width="46" height="40" rx="8" fill="${c.bg}"/>${dots}<circle cx="${LOGO_DISC.cx}" cy="${LOGO_DISC.cy}" r="${LOGO_DISC.r}" fill="${c.accent}"/></svg>\n`;
}
