// web-next/scripts/favicon.mjs - writes public/favicon.svg from the logo's geometry (src/components/logo.ts) in the
// standard theme's colours from src/styles/tokens.css. node scripts/favicon.mjs (Node 22.6+: runs the .ts directly);
// src/components/logo.test.ts checks the file is current.
import { readFileSync, writeFileSync } from "node:fs";
import { faviconSvg } from "../src/components/logo.ts";
import { tokenBlock } from "../src/styles/readTokens.ts";

const t = tokenBlock(readFileSync(new URL("../src/styles/tokens.css", import.meta.url), "utf8"), ':root[data-theme="standard"]');
writeFileSync(new URL("../public/favicon.svg", import.meta.url), faviconSvg({ bg: t.bg, accent: t.accent, logo: [1, 2, 3, 4].map((n) => t[`logo-${n}`]) }));
