// pi/engine/www/bundle-sw.js - the project bundle download's service worker (docs/BUNDLES.md "Web"). The engine
// answers one buffer a request (64 KB, Circle's HTTP daemon), so a bundle of any size downloads as a stream of
// /bundle/zip ranges: a navigation to /bundle-dl/<name>?size=N is answered with a ReadableStream that fetches the
// ranges in order (two in flight), retries a failed range 5 times, waits while the Sidecar pauses (an empty answer),
// and the browser's own download manager saves it at constant memory. Browsers run service workers in secure
// contexts only (https, or localhost through a tunnel); on plain http://sidecar.local the page falls back to a Blob.
const PIECE = 65536;
// the pairing code's session token (pi/engine/paircode.h): /bundle/zip is gated and this worker has none of the
// page's headers, so the page posts the token here before it starts a download. It is never stored.
let AUTH = "";
self.addEventListener("message", (e) => { if (e.data && typeof e.data.sidecarAuth === "string") AUTH = e.data.sidecarAuth; });

async function range(off, len, signal) {
  for (let tries = 0; ; ) {
    try {
      const r = await fetch(`/bundle/zip?off=${off}&len=${len}`,
                            { cache: "no-store", signal, headers: AUTH ? { "X-Sidecar-Auth": AUTH } : {} });
      // the pairing code (pi/engine/paircode.h): a refusal is HTTP 200 with the code in the body, so without this
      // the refusal's text would be taken for zip bytes. Not retried: the page has to pair again first.
      if (r.headers.get("X-Sidecar-Pair")) throw new Error("the Sidecar needs its pairing code again (reload the page)");
      if (r.ok) {
        const b = new Uint8Array(await r.arrayBuffer());
        if (b.length) return b;
        await new Promise((ok) => setTimeout(ok, 500));   // paused (the recorder records): ask again
        continue;
      }
      if (r.status === 404) throw new Error("the export is gone (export again)");
    } catch (e) {
      if (signal && signal.aborted) throw e;
      if (String(e.message).startsWith("the export") || String(e.message).startsWith("the Sidecar needs")) throw e;
    }
    if (++tries > 5) throw new Error(`range ${off} failed 5 times`);
    await new Promise((ok) => setTimeout(ok, 300 * tries));
  }
}

function stream(size) {
  const ctl = new AbortController();
  let off = 0;
  const ahead = [];
  const ask = () => {
    while (ahead.length < 2 && off < size) {
      const len = Math.min(PIECE, size - off);
      ahead.push(range(off, len, ctl.signal));
      off += len;
    }
  };
  return new ReadableStream({
    pull: async (c) => {
      ask();
      if (!ahead.length) return c.close();
      try {
        let b = await ahead.shift();
        c.enqueue(b);
      } catch (e) {
        c.error(e);
      }
    },
    cancel: () => ctl.abort(),
  });
}

self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));
self.addEventListener("fetch", (e) => {
  const u = new URL(e.request.url);
  if (!u.pathname.startsWith("/bundle-dl/")) return;
  const size = Number(u.searchParams.get("size") || 0);
  const name = decodeURIComponent(u.pathname.slice("/bundle-dl/".length)) || "project.sidecar.zip";
  e.respondWith(
    new Response(stream(size), {
      headers: {
        "Content-Type": "application/zip",
        "Content-Length": String(size),
        "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(name)}`,
      },
    })
  );
});
if (typeof module !== "undefined") module.exports = { range, stream, PIECE };   // the page's tests
