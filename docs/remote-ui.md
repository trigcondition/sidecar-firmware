# Remote UI: the Sidecar's touch screen on another device over the USB-C cable

Status: the protocol library, the Pi's service, the USB network re-check and a small command-line client (`rmt_cli`).
Client apps are separate products, not part of this repository; the protocol below is open, anyone may write one. Step 1 ran on a Pi 5 (STATUS, INSERT FX, MASTER FX and VOICE live, on a page of its own,
its touches turn the same knobs as the Pi's touch screen). Step 2 (this branch, not yet on a Pi): the sequencer, SEQ and PAT,
and a "Not in the remote yet" panel for every page the Pi does not feed.

A client renders the real `pi/ui` pages with LVGL (no DSP); the Pi sends the views' data and receives the command lines
the pages emit. It works only over the USB-C cable: the Pi serves it only when its network device is the USB-C one, and
a client connects only to the cable's address.

## Layout

| Where | What |
|---|---|
| `pi/ui/remote/rmt_proto.*` | frames, payload reader/writer, the byte reassembler (portable C) |
| `pi/ui/remote/rmt_views.*`, `rmt_seqview.h` | the section tables of the views: one X-macro table generates encoder, decoder and the schema hash (`rmt_seqview.h`: the play head's and PAT's structs) |
| `pi/ui/remote/rmt_cli.*` | the app's end: connection state, decoded views, subscriptions, commands |
| `pi/engine/remote_views_pi.*` | the Pi's view section (what `kernel.cpp` ran inside the display block): builds the seven views from the engine's state, feeds the local display only when there is one; portable, tested on the Mac |
| `pi/ui/remote/rmt_sha256.*` | SHA-256 / HMAC for the pairing proof |
| `pi/ui/remote/test_rmt.c` | `make -C pi/ui/remote test` (also part of `make -C pi/ui/host test`) |

## Protocol v1

TCP, port 7340 (`RMT_PORT`). Little-endian. Frame: `u8 type | u8 flags | u16 len | u32 seq | payload[len]`, payload at
most 2048 bytes, no CRC (TCP has one). Everything is length-prefixed and append-only: a reader ignores trailing bytes,
unknown message types, unknown views and unknown sections.

| type | dir | payload |
|---|---|---|
| HELLO 1 | app > Pi | `"SCRM" u16 proto_min u16 proto_max u32 app_build u32 schema_hash u16 w u16 h u16 dpi` |
| WELCOME 2 | Pi > app | `u16 proto u16 flags(bit0 AUTH) u32 session u8 nonce[16] u32 schema_hash u8 page u8 fwlen fw u8 nviews {u8 view u16 schema u8 rate_hz}` |
| AUTH 3 | app > Pi | `u8 hmac[32]` = HMAC-SHA256(pairing code, nonce \|\| session LE); only when WELCOME had bit0 |
| SUB 4 / UNSUB 5 | app > Pi | `u8 view u8 rate_hz` (0: the default) / `u8 view` |
| VIEW 6 | Pi > app | `u8 view u8 section u8 instance u8 0` + the section's fields |
| CMD 7 | app > Pi | a console line, 1..63 printable ASCII; answered by OK or ERR with the same seq |
| OK 8 / ERR 9 | Pi > app | `u32 seq` / `u32 seq u16 code text` |
| PING 10 / PONG 11 | both | `u32 token`; the Pi pings every 5 s, three missed pings end the link |
| RESYNC 12 | both | the sender forgets what it sent: the Pi sends every section again |
| PAGE 13 | Pi > app | `u8 page`: the Pi's local page (sidecar_ui's numbers: 8 VOICE, 9 INSERT FX, 13 STATUS, 19 MASTER FX) |
| BYE 14 | both | `u16 code text`, then the connection closes |

Views: 1 STATUS (2 Hz), 2 VOICE (10 Hz), 3 INSERT FX (10 Hz), 4 MASTER FX (10 Hz), 5 SEQ (10 Hz), 6 SEQ PLAY HEAD (30 Hz), 7 PAT (10 Hz). A view is a list of sections
(`rmt_views.c`); the Pi keeps, per client and view, the hash of each section as last sent and sends only the ones that
changed, at the subscription's rate. Meters and traces are already 7/8-bit in the view structs (peaks 0..127, scope
columns -127..127). A section instance is at most 1 KB. Sections are the struct's fields in the table's order, never the
struct's memory.

STATUS schema 1.1 (0.5.2) appends `bpm10` (the Rytm's tempo x 10, 0 unknown) to the live section; a decoder stops at the end of its own list, so an older app ignores it and an older Pi leaves it 0.

Skew: the decoder applies the fields the data has and leaves the rest (an older Pi), ignores bytes past its last field
(a newer Pi). WELCOME lists the views this Pi has (a build without core/seq does not list SEQ, the play head and PAT; a SUB for a view it does not offer is
answered ERR 6) and their schema; the app uses a view when it knows the id and the schema's high byte matches, and greys the page otherwise (below). A `proto` mismatch is a refusal ("update the app / the Sidecar").

## The Pi's service (`pi/engine/remote_srv.*`, `remote_pi.*`)

- **Only on the cable.** `remote_pi::Start` runs from the kernel's network bring-up and opens port 7340 only when the stack's
  device is the USB-C one (`usbnet_pi::Iface () == kIfUsb`, the same rule `usbtrust` uses: the interface, never an address).
  Each accepted connection checks the interface again. On a Wi-Fi or Ethernet boot the port is never opened. `remote=0` turns
  the service off. It needs the display (the views are built in the display's loop, step 1).
- **One client.** A second connection is sent BYE (busy) and closed; the first is untouched.
- **Deadline.** From accept to ready (HELLO, and the proof when asked) a peer has 10 s in all, however it dribbles bytes.
- **Auth.** `usbtrust=1` (default): none on the cable. `usbtrust=0` (and the pairing gate on): WELCOME carries a nonce and the
  client answers HMAC-SHA256(pairing code, nonce || session). The code is never sent, logged or put in a URL; five wrong proofs
  in a row lock the proof out for a minute.
  **What this protects, and what it does not.** The proof only guards a cable you physically control. The code is 8 digits (10^8):
  anyone who can observe one exchange (nonce, session, HMAC) can try every code offline in seconds, and the Pi is not authenticated
  to the app (no mutual proof, no encryption: the traffic is readable and a man in the middle on the cable could answer as the Pi).
  The lockout only slows online guessing. There is no crypto redesign in step 1; trust the USB-C cable you plug in.
- **Commands.** `CMD` lines are checked against an allowlist: `ins=`, `out=V<1..8>:...`, `mfx=`, `vparam=T:K:V[:0]` (numbers
  range-checked: K up to 63), the SEQ page's `seqed=` and `seq=` lines (below), printable ASCII (the SEQ page's `#` `%` `/` `,` `=` included), at most 63, no NUL inside. The grammar is sub-verb by sub-verb (`remote_srv.cpp`):
  `ins=Vn:A|B:type:NAME`, `ins=Vn:A|B:pK:V`, `ins=Vn:send:B:V`, `out=Vn:pre|post|trk`, `out=Vn:lev|pan|wid:V`, `mfx=byp|src|lev`,
  `mfx=S:type|pair|mix|pK`; a reset, `scene=` (no page of the remote emits it yet) and everything else is answered ERR 2 and goes nowhere
  (`out=main`, `reboot`, `keys=...` included). A rate limit (120 lines/s, burst 150) answers ERR 7, but never for the lift of a drag that is held (the final `vparam=T:K:V`). Accepted lines go to the
  console's parser exactly as a touch on the Pi's screen (`CKernel::UiCommand`), but do not wake the Pi's screen.
- **Pages of its own.** Each view's gate is a reference count (`remote_pi::Gate`): the local display holds one while its page is
  shown, the client while subscribed. The kernel builds a view while anyone holds it; the local setter is fed only while the Pi's
  own page shows, and `remote_pi::Offer` hands every build to the client. The Pi's page changes arrive as PAGE frames, so the
  app can follow them (the 'follow Sidecar' switch) or not.
- **Change-driven views (SEQ, PAT).** Core 3 publishes the SEQ view only when the pattern changes, so the kernel does not rebuild it at a rate: it offers
  the view when it changed, or when `remote_pi::Due` says the client has no snapshot (a new subscription, a RESYNC, a frame the ring dropped), and
  `remote_pi::Offer` answers FALSE while the subscription's rate holds it back, so the offer is repeated until it is taken (an edit is never lost to
  the rate). The play head (view 6) is offered every loop, quantised to 1/32 step on the Pi: a still position sends nothing, a moving one at most 30 Hz.
  The encoder costs 36 us (SEQ) and 38 us (PAT) a build on the Mac with sanitizers, and only on a change; the 33 KB WGVIEW, SONG, PROJECT and CHOP
  of step 2's later parts will be sent by section the same way.
- **Disconnect.** BYE, a closed socket, three missed pings (17 s) or a protocol error: the references are released, and a drag
  still held (`vparam=T:K:V:0` without its lift) gets its lift line (`vparam=T:K:V`), so nothing sticks. One log line, no content.
- **Never blocks core 0.** Frames go into a 24 KB ring that the service task drains; TCP's queue is watched
  (`GetBytesQueued`). A full ring drops the frame and the next feed sends every section again (a resync); the Pi's loop never
  waits on the client.

## No touch screen: the app is the only UI

A Pi with no display (no HDMI panel, or the frame buffer did not come up: `m_pDisplay` is 0) serves the remote UI like any other. This is the
getting-started path for someone who has just a Pi.

- **Where the views are built.** `remote_views_pi.cpp` (`remote_views::Feed::Run`, called every loop of the kernel) builds each view from the
  engine's state (`Engine`: core 3's published views, the Rytm pattern, `status_pi`, `insfx_pi`, `voice_pi`) while its gate holds, and
  `remote_pi::Offer`s it. The local screen is the other side (`Display`: the `sidecar_*_set` setters and the "is this page shown" questions);
  the kernel passes a null `Display` when it has no screen, and then nothing is "shown" locally, no `sidecar_ui_*` or LVGL function is called,
  and the client's subscription is the only hold on a view. With a screen nothing changes: a page that shows is fed from the same build.
- **STATUS.** The STATUS view copies `m_Ui`, which `PushUi` fills; `FillUi` is that fill without the screen, run from the stats line every
  second (and when a page command asks). `ui_max_us` (the screen's redraw cost) is 0 with no screen.
- **Commands.** The app's `CMD` lines (`ins=`, `mfx=`, `out=`, `vparam=`, `seqed=`, `seq=`) go to `CConsole::Command`, the console's parser, which only
  queues for core 3: they need no screen and no LVGL. The display commands that touch the screen (`brightness=`, `screensaver=`, ...) were never
  on the allowlist, and in the kernel they are guarded by `m_pDisplay != 0` anyway. A page-navigation touch only exists in the app (its own page).
- **Pages.** There is no Pi page: `remote_pi::Page` is not called, so no PAGE frame is ever sent and WELCOME says page 0 (HOME, a page the app
  does not feed, so 'Follow Sidecar' stays where it is). TRK + TRIG on the Rytm asks the local screen for SEQ (`show_req`); with no screen that
  request is dropped.
- **Cost on core 0.** With no screen and no client the feed polls core 3's publications (a few atomic reads) and builds nothing; when the
  service is not running at all (`remote=0`, Wi-Fi or Ethernet boot, no screen) it is not called. A subscribed view is built at its own rate
  (STATUS 2 Hz, VOICE / INSERT FX / MASTER FX 10 Hz, SEQ and PAT only on a change, the play head every loop, offered at 30 Hz at most); no new loop, no
  wait.
- **Late plug.** Unchanged. A computer that starts its USB network after the Pi chose Wi-Fi or Ethernet shows the "RESTART" prompt on a screen;
  a Pi with no screen never asks and never reboots by itself (`net=auto` takes the cable on the next boot).
- **Getting started with just a Pi.** Plug the client device into the Pi's USB-C port first, then power the Pi: at boot `net=auto` sees the computer's
  USB network and takes the cable (172.31.77.1), the service opens on port 7340 and the app connects. Settings: sidecar.local (the web page) over the
  same cable, or the app's pages. Pairing: `usbtrust=1` (default) needs no code on the cable; with `usbtrust=0` the pairing code is written to the
  card's root (there is no screen to show it).

## The sequencer: SEQ and PAT (step 2)

- **Views.** SEQ is `sidecar_seq_view_t` in 19 section instances: the header (everything but the lanes and notes), two lane sections, and the notes in
  16 sections of 64 (8 bytes a note; the bytes past the used count go as zeros so a stale byte never moves a hash). An edit therefore sends the header
  (the count) and the section holding the note. SEQ PLAY HEAD is one i32, steps x 32 (-1 stopped). PAT is the decoded Rytm pattern: a head, a
  text section (project, kit, "from the Rytm 3 s ago", stale), the 12 tracks and the 72 lock slots in 9 sections of 8. The Pi's own PAT fetch
  (`rytmpat_pi::Want`) is held by a remote PAT too, as VOICE's is.
- **Where the editing model runs: on the Pi.** `seq_touch.cpp` (the add / move / chord / strum / human operations and the undo history) runs on core 3,
  as for the Pi's own screen: the page only emits `seqed=` / `seq=` lines and the Pi applies them to core/seq's pattern and publishes the result.
  The app has no model, so nothing is applied twice and the history is one: UNDO and REDO are lines (`seqed=undo:T`), the Pi's 24 levels are shared
  with the touch screen and with the Rytm's NOTE EDIT, and a selection or a pinch is the page's own state (each UI keeps its own, the view only carries
  NOTE EDIT's highlight). The page draws its own guess of an edit at once, as on the Pi, and the next view replaces it; the editor redraws on a new
  `gen`, which the wire does not carry: the app makes one when the notes, lanes or length differ from the last view fed, and after every reconnect
  (a guess for an edit the cable never delivered must go).
- **Commands (exact grammar, `remote_srv.cpp`).** `seqed=add|del|move|len|vel|chord|strum|human|undo|redo|show:...` with every number a plain
  digit string in its range (track 0..11, step 0..127, note 0..127, micro -96..96, velocity 0..127, length 0..12288, chord 0..15, inversion 0..3, ...), and
  `seq=key|spd|dir|rnd|alock|rtrg|fx|lcond|slide|rndstep|seed|print|swing|poly:...` with each word from the page's own list (`seq=rtrg` one of the eight
  lines its key sends). `shot.c` runs every line the SEQ page sends in the host tests through the real allowlist, so a new control that sends a line the
  service would refuse fails `make -C pi/ui/host bounds`. Nothing else of the sequencer is reachable.
- **Drags and gestures.** A note drag, a pinch and a chord stamp send a line only when the finger lifts (the Pi's page is the same), so there is no held
  line to release on the Pi and no rate to protect: a cable pulled mid-drag cancels the finger in the app, the lift's line goes nowhere (not connected)
  and the Pi's note stays where it was; both fingers of a pinch are let go.
- **Page requests.** The Rytm's TRK + TRIG asks the Pi's own screen for SEQ (`show_req`); the app hears that as the PAGE frame, so it follows when
  'Follow Sidecar' is on and keeps its own page otherwise.

## Pages that are not in the remote (yet)

A page whose views the connected Pi does not offer (an older Pi, a build without core/seq), or that has no view at all (HOME, SONG, REC, settings, ...),
is greyed: `sidecar_ui_page_enable` puts "NOT IN THE REMOTE YET" with the page's name over its content (the navigation and the title stay), greys its
sub-page pill, and greys a section's navigation key when none of its pages is fed. Until a connection is
ready nothing is greyed (the overlay covers the picture); the Pi's own screen never calls it, so every page is fed there as before.

## Clients

This repository holds the protocol library and a command-line client (`pi/ui/remote/rmt_cli.c`) that the tests use. A
graphical client is not part of it. A client must connect to the cable's address only (default 172.31.77.1, the Pi's
`usbip=`), keep nothing of an old connection (the next one starts from a full snapshot), send the pages' command lines as
CMD, and show a "Disconnected" state when the cable is pulled.

## Tests

| What | Command |
|---|---|
| protocol library: frames, hashes, round trip of every view, deltas, skew, garbage | `make -C pi/ui/remote test` (also in `make -C pi/ui/host test`) |
| the service's portable part: allowlist (the SEQ page's grammar, 163 yes / no lines), refcounts, SEQ / PAT offers (Due, the rate, a dropped frame), views a Pi does not offer, proof and lockout, session against the real client, drag release, one client, slow reader, dead link, garbage, encoder cost | `make -C pi/engine/tests build/test_remote_srv && pi/engine/tests/build/test_remote_srv` (and the whole `make -C pi/engine/tests`) |
| the view section with no display (`remote_views_pi.cpp`): a fake engine, the real service and client; every subscribed view arrives, an edit sent as a command lands and comes back, nothing is built with no hold, a client that leaves releases the references, wrapping clocks; with a display: only the page that shows is fed, one build feeds both | `make -C pi/engine/tests build/test_remote_views && pi/engine/tests/build/test_remote_views` (links no LVGL: a call into a setter could not link) |
| `usbnet_cfg.h`'s re-check on plug | `pi/engine/tests/build/test_usbnet_cfg` |

Not yet: the other views (HOME's status, XY, MORPH, REC, SCOPE, SONG, PROJECT, CHOP, SAMPLE EDIT, PRESET, WGVIEW), Retina layouts, MIDI-SysEx transport.
