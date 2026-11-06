// web-next/vite.config.ts - the dev server proxies the Sidecar's API to the mock (npm run mock) or, with
// SIDECAR=http://<address>, to a real Sidecar. The Sidecar refuses a WebSocket from another origin (docs/web-ws.md
// "Auth"), so the proxy presents the target's own origin. base "/next/": where the Pi would serve the build.
import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

const target = process.env.SIDECAR || "http://127.0.0.1:8787";

export default defineConfig({
  base: "/next/",
  plugins: [vue()],
  server: {
    proxy: {
      "/ws": { target, ws: true, changeOrigin: true, headers: { Origin: target } },
      // every HTTP endpoint of the Sidecar (pi/engine/web.cpp), with its origin (several need the same-origin check)
      ...Object.fromEntries(
        ["/status", "/health", "/log", "/pair", "/time", "/wifi", "/midibridge", "/um", "/lists", "/pool", "/presets", "/bundle", "/usb", "/fs", "/blackbox", "/cmd"].map((p) => [
          p,
          { target, changeOrigin: true, headers: { Origin: target, Referer: `${target}/` } },
        ]),
      ),
    },
  },
  build: { target: "es2020", assetsInlineLimit: 0, sourcemap: false },
});
