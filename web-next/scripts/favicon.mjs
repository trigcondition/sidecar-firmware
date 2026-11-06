// web-next/scripts/favicon.mjs - writes public/favicon.svg from the logo's geometry (src/components/logo.ts).
// node scripts/favicon.mjs (Node 22.6+: runs the .ts directly); src/components/logo.test.ts checks the file is current.
import { writeFileSync } from "node:fs";
import { faviconSvg } from "../src/components/logo.ts";

writeFileSync(new URL("../public/favicon.svg", import.meta.url), faviconSvg());
