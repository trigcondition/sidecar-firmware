# The web API over WebSocket: /ws

The Pi engine's live API for the next web UI (`web-next/`, Vue 3 + Vite + TypeScript) and, later, a desktop editor
(Tauri) that wraps the same app. One WebSocket instead of the old page's polling: the Sidecar pushes what changed, the
client sends commands as requests with ids. The old page (`pi/engine/www/index.html`) and every HTTP endpoint stay as
they are; `/ws` sits beside them.

Code: `pi/engine/ws_frame.h` (handshake, frames), `pi/engine/ws_json.h` (reading the client's JSON),
`pi/engine/ws_session.h` (the protocol below), `pi/engine/web_cmd.h` (which commands the web may send, shared with
`POST /cmd`), `pi/engine/web_ws.{h,cpp}` (the socket loop), `CWebServer::UpgradeConnection` in `pi/engine/web.cpp`,
`pi/circle/circle-http-ws.patch` (Circle: the hook in the HTTP daemon and the socket's send-queue size). Tests: `pi/engine/tests/test_ws.cpp`
(`make -C pi/engine/tests`), `web-next/src/lib/*.test.ts` (`npm test` in `web-next/`).

## Connection

- `GET /ws` with `Upgrade: websocket`, `Sec-WebSocket-Version: 13` and a valid `Sec-WebSocket-Key` (RFC 6455). The answer
  is `101 Switching Protocols`; no subprotocol and no extension (no compression) are negotiated.
- Refusals are plain HTTP, no body, the connection closed (each logs one `ws: refused` line with the reason):
  `503` the web UI is off (SIDECAR MENU > WEB UI) or `MaxWs` (4) connections are open; `400` a bad Upgrade or key;
  `426` another version; `403` the strict same-origin check failed.
- **Same origin, strict:** `Host` must be one of the Pi's names (`<hostname>.local`, its IP) and `Origin` (else
  `Referer`) `http://<that host>`, as for `POST /midibridge/set`. A browser always sends `Origin` on a WebSocket, and a
  WebSocket is not covered by the browser's same-origin policy, so without this any site the owner visits could open
  one to `sidecar.local`. A non-browser client (the Tauri app's Rust side, a test tool) sends the Pi's own origin.
- Limits: at most 4 connections (`CWebServer::MaxWs`), counted apart from the 4 HTTP workers (`MaxWorkers`), so the
  old page keeps its connections; each connection is one Circle task on core 0, the HTTP worker's own.
- Frames: text only (binary closes with 1003), masked (RFC), a message up to 4096 bytes (fragments allowed, ping/pong/close
  between them; more closes with 1009), valid UTF-8 (1007). The server's frames are not fragmented.
- Keepalive: the server pings when it sent nothing for 15 s; nothing at all from the client for 45 s closes with 1001
  ("idle"). Browsers answer pings by themselves. The client also sends an application `ping` request when it heard
  nothing for 15 s and reconnects after 30 s of silence.
- The web UI switched off while connected: closed with 1001.

## The Circle patch

`pi/circle/circle-http-ws.patch`, applied by `build.sh` after `circle-http-case.patch` (marker `circle-http-ws` in
`httpdaemon.h`; `lib/net` is rebuilt as before):

- `CHTTPDaemon` keeps the `Upgrade`, `Sec-WebSocket-Key` and `Sec-WebSocket-Version` headers and, for a GET with an
  `Upgrade` header, calls the new virtual `UpgradeConnection (path, socket)` before `GetContent`. TRUE: the derived
  class served the connection (in the worker's task) and deleted the socket. The default returns FALSE, so every other
  request is served exactly as before.
- `CSocket::GetBytesQueued ()` (through `CTransportLayer` and a virtual `CNetConnection::GetBytesQueued`, which
  `CTCPConnection` answers from its TX queue): the backpressure measure above. Only lib/net derives from
  `CNetConnection`, and lib/net is rebuilt with the patch.

`web.cpp` overrides `UpgradeConnection` and `web_ws.cpp` calls `GetBytesQueued`, so a tree without the patch does not
build (as with `circle-http-auth.patch`).

## Auth

The pairing code's session token (`paircode.h`, docs/pi-engine.md "Pairing code"), the same token the old page sends
in `X-Sidecar-Auth`. A browser cannot set a header on a WebSocket, and a token in the URL would land in history, proxy
logs and `Referer`s, so it goes in the first request after the hello:

```json
{"type":"auth","id":1,"payload":{"token":"<32 hex>"}}
```

The client gets the token over HTTP as the old page does: `POST /pair` with the code once (keeping the client id and
secret in `localStorage`, under the old page's keys), `POST /pair/resume` at each load. Without a token the connection
still works for what `/status` shows (that endpoint is open too); `cmd` answers 401 with `"pair":true`. The token is
checked again at every command, so a session unpaired elsewhere (forget, revoke all, a reboot) stops at once. Five
wrong tokens close the connection (1008). The token is kept in the connection's memory only, wiped when it ends, never
echoed or logged.

## Envelope

JSON text messages. Every client request has a `type` and a positive integer `id`; its answer is a `reply` with the
same `id`. A `payload` is an object.

| from | type | members | what |
|---|---|---|---|
| server | `hello` | `payload` | first message: see Versioning |
| server | `reply` | `id`, `ok`, `payload` or `error` | the answer to a request |
| server | `pub` | `topic`, `seq`, `payload` | a topic's current state |
| server | `error` | `error` | a message that had no usable `id` |
| client | `auth` | `id`, `payload.token` | see Auth |
| client | `sub` | `id`, `payload.topics` [, `payload.ms`] | subscribe; `ms` = the push interval, clamped to 250..10000 (default 1000) |
| client | `unsub` | `id` [, `payload.topics`] | unsubscribe (no list: all) |
| client | `cmd` | `id`, `payload.c` | one console command (below) |
| client | `ping` | `id` | `reply` with `payload.t` (the connection's ms clock) |
| client | `ack` | `seq` | flow control (below); no `id`, no reply |

`error` is `{"code":N,"msg":"..."}` with HTTP-like codes: 400 malformed, 401 not paired (`"pair":true`), 403 refused
command, 404 unknown type, 429 too many requests, 500 the server's own fault. Ten malformed messages close the
connection (1008). Requests are limited to a burst of 20, then 20 a second (429 beyond; `ack` is not counted).

`sub`'s reply: `{"topics":[served],"later":[reserved names not served yet],"unknown":[...],"ms":N}`.

## Topics

| topic | payload | pushed |
|---|---|---|
| `status` | exactly what `GET /status` answers (the same function, `CWebServer::StatusJson`) | when it differs from the last push, at most every `ms`; unchanged for 10 s: sent anyway |
| `levels` | master and 12 strip peaks (below) | every `ms` (33..1000, default 40); unchanged: at least once a second |
| `seq` | the touch pattern editor's view of the shown seq track (below) | when it changed, at most every `ms` (100..10000, default 250) |
| `seqpos` | `{"pos":steps x 1000 (-1 stopped),"track":T,"transport":0 stop 1 play 2 pause,"pattern":N,"bpm10":tempo x 10}` | every `ms` (33..1000, default 50) when it moved |
| `log` | `{"from":N,"to":M,"gap":bool,"text":"..."}`: the console log's new bytes, as `GET /log` | when there are new bytes, at most every `ms` (250..10000, default 500), up to 8 KB a push |
| `voices` | the touchscreen's VOICE page for the selected track (below) | when it changed, at most every `ms` (100..10000, default 200) |

Each topic's range is in the hello (`"rates":{"status":[250,1000],"levels":[33,40],...}`, fastest and default), and
the hello's `topics` lists only what this build serves (`seq` / `seqpos` need core/seq in the tree; the rest are
`later`). The window of 4 is shared by all topics. `log` is open, as `GET /log` is; a new subscriber gets the last
8 KB first, and `gap` says bytes were lost (the 64 KB ring overwrote them).

A command taken by the Sidecar (from any connection, or `POST /cmd`) pushes `status` early: 40 ms after it, and once
more 250 ms after that (each only if it changed), so a page shows its own click without waiting for the interval.

### `seq` (the touch editor's view)

`seqed_pi::View` (the same view the touchscreen's SEQ page draws, `pi/ui/sidecar_seqed.h`) as JSON,
`pi/engine/seqview_json.h`: `track` (the Rytm track shown, 0..11), `tracks` (the 4 seq slots), `pattern`, `machine`,
`length`, `pages`, `defaults`, `transport`, `bpm10`, `undo`/`redo`, `gen`, `chords` (names), `swing`, `poly`, `speed`,
`dir`, `key`, `scale`, `qnt`, `rnd`, `seed`, `seedFixed`, `editPage`, `noteEdit`, `notes` as
`[step, micro, pitch, vel, len, flags]` (micro and len in 1/96 step) and `steps` (only the steps with anything) as
`[step, locks, cond, extras, ratchet count, ratchet mode, fx cmd, fx value, lock cond, arp locks]`. 1024 notes fit in
40 KB (test_ws). Edits are the touch editor's own command lines (`seqed=add:T:S:P:M:V:L`, `del`, `move`, `len`, `vel`,
`chord`, `strum`, `human`, `undo`, `redo`; docs/TOUCH-EDITOR.md) sent as `cmd`. `seqed=show:T` picks the shown track:
it is one shown track for the touchscreen and the web together.

### `voices` (the touchscreen's VOICE page)

`voice_pi::Fill` (the view the VOICE page draws, `pi/ui/sidecar_voice.h`) as JSON, `pi/engine/voiceview_json.h`:
`track` (0..11), `machine`, `sidecar` (a Sidecar machine, else the Rytm's own sound), `name`, `val` and `label` (P1..P8
with the Rytm's labels; an unused one has an empty label), `pattern`, `locks` / `trig` (per step; empty on the Pi for
now: the Rytm's pattern dump is the display's), `mode` (`plain`, `fm`, `multi`, `daisy`) with that machine's panel
(`fm`: patch name, algorithm, feedback, the operators' grid places, modulation bits, carriers, output levels and
frequencies; `multi`: instrument, key zones, keys sounding; `daisy`: model, the modes' ratios x 100 and ring times),
`gen`. No scope and no level: the input tap is the display's own (reading it would steal its entries), and the
meters are `levels`. The track is the Rytm's selected one, or the one `voicepick=T` picked (`-1`: follow the Rytm
again), one pick for the touchscreen and the web together.

Reserved for later (a `sub` lists them under `later`): `usbMode`, `scenes`.

### `levels`

What the header's "signal tide" and the Mix page's meters draw:

```json
{"type":"pub","topic":"levels","seq":812,"payload":{"master":[0.62,0.58],"strips":[0.1,0,0,0.4,0,0,0,0,0.2,0,0,0.6]}}
```

- `master`: the L/R peak since the last push, linear 0..1 (1 = full scale); `strips`: 12 peaks in the Mix page's
  order V1..V8, B1..B3, M. Three decimals (~170 bytes a push).
- Peaks are **held between pushes** (max since the last one), so a short transient is never lost to the coalescing;
  the client draws its own decay.
- Rate: its own interval range, 33..1000 ms, default 40 ms (25 Hz). The hello will carry it per topic
  (`"rates":{"levels":[33,40]}`) when the Pi serves the topic; until then `minMs`/`maxMs` are the status topic's.
- Source on the Pi: the output stage's own peaks (`Output::TakeVoicePeaks`, `TakePeak`), which reset when read. So that
  the web does not steal the touchscreen's meters, whoever reads them feeds `g_PeakFan` (`pi/engine/peak_fan.h`) and
  takes its own share: the display, the stats line and each of the two web connections. Core 0 only; the render path
  is unchanged; nothing is read while no client subscribes.
- A voice's strip is the larger of its OUT pair and its own TRK output (`Output::TakeTrkPeaks`, only the web reads
  it). B1 and B2 are each send FX's own return, B3 the dry bus's input (`sendfx_pi::TakeBusPeaks`, written on core 2
  after its block). Both go through `g_PeakFan` as the rest. Cost (host bench): the TRK peaks +0.01..0.04 % of a core
  (output unchanged bit for bit), the bus peaks ~0.012 % (an estimated 0.03..0.05 % on the Pi 5). A build without send
  FX would send `null` for B1..B3. The master is one value (L = R).
- Flow control as for every topic: within the window of 4 the newest peaks go; a slow client simply sees fewer frames.
  At 25 Hz the window is the bottleneck only above ~160 ms round trip, where fewer frames is the right answer.
- Not changed-only: silence is pushed too (as zeros), at most once a second while everything is 0.

## Commands

`{"type":"cmd","id":7,"payload":{"c":"notes=1"}}` runs the serial console's parser, as `POST /cmd` does
(`CConsole::Command`), logged as `command :notes=1 (ws)` (`wifi=join:` redacted). The refusals are `POST /cmd`'s
(`web_cmd.h`, one list for both): `midibridge=`, `usbmidi=`, `osupdate=`, `link=tcp:<host>` while usbmidi=os: is on,
`wdtest`, an empty command. A `/ws` message counts as a POST body (`wifi=` allowed), and the upgrade passed the strict
same-origin check, so `usbmode=overbridge` is taken as from the page. Commands answer `{}` when taken; their effect
shows in the next status push (and the log), as on the old page. At most 199 bytes.

## Push rates and backpressure

Circle's TCP send with `MSG_DONTWAIT` never refuses: it queues whatever it is given. So the server bounds what it
sends itself:

- every `pub` has a `seq` (1, 2, ... per connection); the client sends `{"type":"ack","seq":N}` once it has handled
  push N (the web client does this after the handlers ran);
- at most 4 pushes may be unacked (`window` in the hello); while the window is full nothing is built or sent, and when
  it opens the topic's **newest** state goes out: older states are never queued (coalesced; counted, and logged in the
  `ws: closed` line);
- a client can ack without reading (a tool, not a browser), so the server also reads its TCP send queue
  (`CSocket::GetBytesQueued`, from `circle-http-ws.patch`: bytes sent and not acknowledged, plus unsent): above 48 KB no
  push is built (it waits like a full window), and a frame that would take it past 128 KB drops the connection
  (logged as a policy close, 1008);
- so a stalled client costs the Pi one open socket and at most 128 KB of queue, and building a push (the expensive
  part on core 0) only happens when the client can take it;
- pings from the client are charged to the same request bucket (a pong costs a send): a ping flood closes with 1008;
- replies are not windowed; the rate limit bounds them.

A topic is built at most every `ms` per connection; an unchanged one (FNV-1a of the payload) is not sent. With two
clients at the default 1 s that is two `/status` builds a second, as the old page's polling costs today.

## Versioning

The server's first message:

```json
{"type":"hello","payload":{"proto":1,"server":"sidecar","version":"0.4.2",
 "topics":["status","levels","seq","seqpos","log","voices"],"later":["usbMode","scenes"],
 "caps":["auth","sub","unsub","cmd","ping","ack"],"auth":"token","window":4,"maxMsg":4096,"minMs":250,"maxMs":10000,
 "rates":{"status":[250,1000],"levels":[33,40],"seq":[100,250],"seqpos":[33,50],"log":[250,500],"voices":[100,200]}}}
```

- `proto` is the major: a change that an old client would misread (a renamed member, another meaning) raises it, and
  a client refuses a major it does not know (the web client stops and shows "incompatible", no reconnect loop).
- Additions within a major are announced, not versioned: new topics appear in `topics`, new request types in `caps`,
  new members in payloads are ignored by old clients. A client checks `topics` / `caps` before relying on one (the
  Tauri app especially, as it may meet older firmware).

## Serving the new app from the Pi

From the card: `GET /next/<file>` reads `SD:/.rytm/www-next/<file>` (`pi/engine/webnext_path.h`; `/next` and `/next/`
are `index.html`). No kernel growth, and an app update needs no kernel.

- The `.gz` sibling is sent when there is one (`Content-Encoding: gzip`), else the file itself.
- `/next/assets/*` (Vite's hashed names) are cached for good (`immutable`); everything else is `no-cache`, so a new
  build shows on the next load.
- Names: `A-Z a-z 0-9 - _ . /` only, no part starting with a dot, 120 characters at most; anything else is 404. A file
  larger than the answer buffer (192 KB) is a 500 and a log line. `/fs` cannot reach `/.rytm`, so nothing can be put
  there over the network.
- `/next` is in the open set (`web.cpp` `Open`): it is static and holds nothing of the device. `/ws` is reached from
  `/next/` on the same origin. The app uses hash routes (`/next/#/seq`), so the Pi needs no fallback to `index.html`.

Putting it on the card (the card in a reader, or the Pi's own card while it is off):

```
cd web-next && npm run build && npm run card -- /Volumes/<the card>
```

`scripts/card.mjs` replaces `.rytm/www-next/` as a whole, gzips the text files (about 164 KB in all, the largest file
51 KB) and stops on a name the Pi would refuse. The fonts' and Vue's licences ship in `licenses/` (THIRD_PARTY_LICENSES).
