# web-next: the Sidecar's next web UI

Vue 3 + Vite + TypeScript (no Nuxt), talking to the Pi engine over one WebSocket (`/ws`, the protocol in
[docs/web-ws.md](../docs/web-ws.md)). Today it is the plumbing only: a plain page with the connection, the pairing,
the live status and one harmless command (`version`). The visual design comes later. The old page
(`pi/engine/www/index.html`) is untouched and still what the Pi serves.

## Layout

- `src/lib/protocol.ts`: the message types (shared with a later Tauri app: no Vue, no DOM).
- `src/lib/client.ts`: `SidecarClient`: reconnect with exponential backoff and jitter, the hello's version check,
  the pairing token as the first request (never in the URL), subscriptions kept across reconnects, requests with ids
  and timeouts, the acks the server's flow control needs, an application ping. The WebSocket class can be passed in
  (`socket:`), so it runs in a browser, in Node (`ws`) and in tests.
- `src/lib/pairing.ts`: the pairing code over HTTP as the old page does it (same `localStorage` keys, so a browser
  paired there is paired here on the same origin).
- `src/lib/useSidecar.ts`: the client as Vue state, shared by the pages; `src/router.ts`: Home, Voices, Mix, Morph,
  Seq, Settings (hash history: the Pi needs no fallback route); `src/pages/`: Home (the plumbing page) and Settings
  are real, the rest placeholders.
- `src/App.vue`: the frame: skip link, the header (the "Docked" logo `components/Logo.vue`, the SIDECAR wordmark, the
  connection in words, the slot for the signal tide `components/SignalTide.vue`, fed by the `levels` topic), the tabs.
- `mock/server.mjs`: a stand-in Sidecar in Node: `/status`, the pairing endpoints and `/ws` with the same rules as the
  Pi (window, coalescing, rate limit, strict origin check). It prints a pairing code.

## Design tokens and accessibility

The look ("OLED Studio") drops in through `src/styles/tokens.css`: every colour, radius and size is a custom property,
in two themes (`standard`, `contrast`: black, white text, 2 px white control borders, no textures, no glow) on
`<html data-theme>`. `src/styles/base.css` holds the floor every page inherits:

- sizes in rem, and a text-size setting (100 / 125 / 150 %, `<html data-text-size>`); no text under 13 px;
- a 3 px #ffd166 focus ring on everything that takes focus; a skip link; the page heading takes focus on navigation;
  radio groups follow the ARIA pattern (arrow keys inside, Tab out);
- touch targets at least 44 px; selection never by colour alone (a check mark and a thicker outline: `.pill` with
  `aria-current` / `aria-checked` / `aria-pressed`); values as numbers as well as drawn;
- `prefers-reduced-motion` stops every animation and the glow (the logo's breathing ripple, later the signal tide);
- theme and text size kept per device (`src/lib/usePrefs.ts`, localStorage in a try/catch; the system's
  `prefers-contrast: more` picks the high-contrast theme on a first visit).

`src/styles/contrast.test.ts` reads tokens.css and checks every text pair at 4.5:1 and every control / focus pair at
3:1 in both themes, so a token change cannot quietly regress it. `--border` (#1f242b, 1.25:1) is decorative only.

Fonts: Space Grotesk (UI, 400/500/700) and Doto (dot matrix, 400/700, for larger numbers and titles; at control size
it reads poorly, so controls stay in Space Grotesk), bundled from `@fontsource` as woff2, latin subset only
(`src/styles/fonts.css`, ~47 KB). Both SIL OFL 1.1: the texts ship in `public/licenses/`, and `THIRD_PARTY_LICENSES`
needs an entry once the bundle goes into a release. The favicon is `public/favicon.svg`, made from the logo's
geometry (`node scripts/favicon.mjs`; a test checks it is current).

## Run

```bash
npm install
```

```bash
npm run mock
```

```bash
npm run dev
```

Open http://localhost:5173/next/ and pair with the code the mock printed. The dev server proxies `/ws`, `/status`
and `/pair*` to the mock and presents the target's origin (the Sidecar refuses a WebSocket from another origin).
Against a real Sidecar: `SIDECAR=http://sidecar.local npm run dev`.

## Test and build

```bash
npm test
```

```bash
npm run build
```

`npm test` runs the client against a scripted fake socket and, end to end, against the mock on 127.0.0.1. The build
goes to `dist/` with `base: "/next/"` (about 95 KB over the wire today: 44 KB JS and 2 KB CSS gzipped, 47 KB fonts); how the Pi would serve it is in docs/web-ws.md
"Serving the new app from the Pi".
