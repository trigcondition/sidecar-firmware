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
      "/status": { target, changeOrigin: true },
      "/pair": { target, changeOrigin: true, headers: { Origin: target } },
    },
  },
  build: { target: "es2020", assetsInlineLimit: 0, sourcemap: false },
});
