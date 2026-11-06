// web-next/src/components/logo.ts - the "Docked" mark's geometry (viewBox 0 0 46 40): a 7 x 7 dot grid faded out
// from a centre, and a solid disc docked beside it. Shared by Logo.vue and the favicon (scripts/favicon.mjs).
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

/** The favicon: fixed colours (a file cannot read the page's tokens) on a dark rounded tile, so it shows on light tabs too. */
export function faviconSvg(): string {
  const dots = logoDots(true).map((p) => `<circle cx="${p.x}" cy="${p.y}" r="${p.r}"/>`).join("");
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${LOGO_VIEWBOX}"><rect width="46" height="40" rx="8" fill="#050607"/><g fill="#e8edf2">${dots}</g><circle cx="${LOGO_DISC.cx}" cy="${LOGO_DISC.cy}" r="${LOGO_DISC.r}" fill="#6cc2ff"/></svg>\n`;
}
