// web-next/vite.config.ts - the dev server proxies the Sidecar's API to the mock (npm run mock) or, with
// SIDECAR=http://<address>, to a real Sidecar. The Sidecar refuses a WebSocket from another origin (docs/web-ws.md
// "Auth"), so the proxy presents the target's own origin. base "/next/": where the Pi would serve the build.
import { execSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

// the build's stamp (src/lib/build.ts): the release it belongs to (../VERSION, as the kernel's) and its commit, so the
// page can say when the Sidecar runs another release
const git = (a: string) => { try { return execSync(`git ${a}`, { cwd: __dirname, stdio: ["ignore", "pipe", "ignore"] }).toString().trim(); } catch { return ""; } };
const WEB_BUILD = {
  version: readFileSync(new URL("../VERSION", import.meta.url), "utf8").trim(),
  commit: git("rev-parse --short=7 HEAD") || "unknown",
  dirty: git("status --porcelain -- .") !== "",
};

const target = process.env.SIDECAR || "http://127.0.0.1:8787";

export default defineConfig({
  base: "/next/",
  plugins: [vue()],
  define: { __WEB_BUILD__: JSON.stringify(WEB_BUILD) },
  server: {
    // web/lfo.js (the LFO logic the old page and the Mini's page share) lives one level up
    fs: { allow: [".."] },
    proxy: {
      "/ws": { target, ws: true, changeOrigin: true, headers: { Origin: target } },
      // every HTTP endpoint of the Sidecar (pi/engine/web.cpp), with its origin (several need the same-origin check)
      ...Object.fromEntries(
        ["/status", "/health", "/log", "/pair", "/time", "/wifi", "/midibridge", "/um", "/lists", "/pool", "/presets", "/bundle", "/usb", "/fs", "/blackbox", "/cmd", "/update"].map((p) => [
          p,
          { target, changeOrigin: true, headers: { Origin: target, Referer: `${target}/` } },
        ]),
      ),
    },
  },
  // One JS file and one CSS file, the fonts inside the CSS: the Pi serves 4 connections at once and answers more with a
  // 503 (pi/engine/web.h MaxWorkers). Split chunks made a page's first visit a burst of files a browser doesn't retry;
  // Safari asked for them all at once, so Voices, Mix and the other pages stayed blank (a user, 2026-11-13).
  build: {
    target: "es2020",
    assetsInlineLimit: (file: string) => (file.endsWith(".woff2") ? true : undefined),
    cssCodeSplit: false,
    sourcemap: false,
    rollupOptions: { output: { inlineDynamicImports: true } },
  },
});
