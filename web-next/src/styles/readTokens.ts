// web-next/src/styles/readTokens.ts - the custom properties of a rule in tokens.css (the tests and the favicon script
// read the palette from there, so a palette switch stays one file).
export function tokenBlock(css: string, sel: string): Record<string, string> {
  const re = /([^{}]+)\{([^{}]*)\}/g;
  for (let m; (m = re.exec(css)); ) {
    if (!m[1].includes(sel)) continue;
    const vars: Record<string, string> = {};
    for (const d of m[2].matchAll(/--([\w-]+)\s*:\s*([^;]+);/g)) vars[d[1]] = d[2].trim();
    return vars;
  }
  throw new Error(`no rule for ${sel}`);
}
