# web-next: parity with the old page

Every feature of the current page (`pi/engine/www/index.html`), where it lives in web-next, how it gets its data, and
whether it is done. The old page keeps working throughout; nothing here changes an endpoint it uses.

**Pages.** The six named pages, plus two (a question for the owner): **Rec** (the recorder, takes, the samplers) and
**Library** (pool, files, projects). The touchscreen has the same split (REC, PAT). Old tab → new page:

| Old tab | New page |
|---|---|
| Status | Home (tiles, SRC, box strip), Settings (Wi-Fi, access, MIDI bridge, storage) |
| Sound | Settings › Audio (notes, out, IN L/R, computer audio, USB-C mode, FIFO, display), Voices (machines, drone), Seq (poly, seqtop, step editor) |
| Send FX | Mix |
| LFO | Voices › Modulation |
| Record | Rec |
| User machines | Voices › User machines |
| Pool | Library › Pool |
| Presets | Voices › Presets |
| Project | Library › Projects |
| Files | Library › Files |
| Log & console | Home › Log and console |
| About | Settings › About |

**Data paths.** `ws:status` = the status topic (`/status`); `ws:levels`, `ws:seq`, `ws:seqpos`, `ws:log` = the new
topics (docs/web-ws.md); `cmd X` = the `/ws` cmd request (the same list as `POST /cmd`); `GET /x` = the existing HTTP
endpoint with the pairing header (`src/lib/api.ts`). Status: ✅ done · 🔶 partial · ⬜ to do · ➖ not carried over.

## Every page (frame)

| Feature | Data | Status |
|---|---|---|
| Connection state, host, uptime | ws hello + `ws:status` `host`, `uptime` | ✅ |
| Pattern · BPM · play state pill | `ws:seqpos` (`pattern`, `bpm10`, `transport`) | ✅ (Pi topic new: needs the device test) |
| Signal tide | `ws:levels` master | 🔶 (Pi topic new) |
| Pairing: resume, code entry, stale pairing, "show it on the Rytm" | `POST /pair`, `/pair/resume`, `/pair/status`, `/pair/show` | ✅ (and the 5 s re-check while unpaired; the gate off needs no code) |
| Box strip (boxes, roles) and the box name instead of "Rytm" | `ws:status` `boxes[]` (`feat` bits gate pages) | ✅ (the strip on Home with two boxes or more; the name from the USB product string; `feat` hides send FX, POOL, projects, user slots, IN L/R, "show on the Rytm") |
| Power warning banner | `ws:status` `power.warn` | ✅ |
| Clock sync at load | `POST /time` | ✅ (once paired) |
| Build check (page older than the kernel) | ws hello `version` | ✅ the build is stamped with VERSION and its commit; a note when the Sidecar's release differs (Settings › About shows both) |

## Home

| Feature | Data | Status |
|---|---|---|
| Tiles: CPU c1-c3, FIFO, underruns, Overbridge, Rytm, USB-C, temperature, power, this page | `ws:status`; CPU / FIFO / OB gap / USB-C alt from the stats lines in `ws:log`, core 0 from the 10 s idle line | ✅ redrawn: a health row, then Engine / Connections / System with meters (`src/lib/health.ts`) |
| Rytm tracks (SRC chips) | `ws:status` `src[]` | ✅ |
| Recorder at a glance (state, time) | `ws:status` `rec` | ✅ |
| Log: live tail, follow, hide stats lines, highlight errors | `ws:log` | ✅ (`src/lib/logbuf.ts`) |
| Console line | `cmd` | ✅ |

## Voices

| Feature | Data | Status |
|---|---|---|
| Machine per voice (BD..CY/CB), "now" | `cmd notemach=V:NAME`; `ws:status` `machines[]` | ✅ |
| Drone voice / all / off | `cmd drone=`; `ws:status` `drone` | ✅ |
| Voice knobs (the touchscreen's VOICE page): machine, P1..P8, the FM / DAISYSP / MULTISAMPLE panel, track pick | `ws:voices`; `cmd voicepick=T` | ✅ (shown, turned on the Rytm; p-locks: not on the Pi yet) |
| LFO 1-8: target, parameter, wave, sync, retrig, rate, depth, DESIGN wave, live phase | `cmd lfo=...`; `ws:status` `lfo` | 🔶 (all but drawing the DESIGN steps: presets only; `web/lfo.js` shared) |
| User machines: list, slots U1-U5 assign / clear, memory, rescan, upload, delete, empty project, export / import slot list | `GET /um`; `cmd um=...`; `/fs/put` (chunked) | 🔶 (slots, assign, clear, packages, rescan; upload / delete / memory / empty / slot lists ⬜) |
| Presets: JUNO and FM lists (rename, move, delete, save current), user machine presets | `GET /lists`; `cmd juno= / fm= / um=save`; `/fs/ls`, `/fs/mv`, `/fs/del` | 🔶 (JUNO / FM lists; user machine presets ⬜) |

## Mix

| Feature | Data | Status |
|---|---|---|
| 12 strips V1-V8, B1-B3, M: machine, level number, meter | `ws:status` `machines`, `ins`; `ws:levels` (each voice: OUT pair or own TRK; B1-B3: the send FX buses) | ✅ (voice level `out=Vn:lev`, master `mfx=lev`) |
| Insert EQ curve per strip, insert FX chip | `ws:status` `ins.c[]` (`s`, `peq`) | ✅ editable: POST EQ, inserts A/B with P1-P4, route, pan, width, sends (`ins=`, `out=`) |
| Send FX 1/2: type, PRE/POST, 12 sends; kill tails | `ws:status` `fx[]`; `cmd killfx` | ✅ |
| Master FX: bypass, source, level, trim, 4 slots (type, P1-P4, mix, pair), limiter GR | `cmd mfx=...`; `ws:status` `ins.mfx` | ✅ (pair ⬜) |
| Sidechain | placeholder until 0.5.0 | ✅ placeholder |

## Seq (new: the old page had only the forms)

| Feature | Data | Status |
|---|---|---|
| Piano roll of the shown seq track, pages, play head | `ws:seq` (the touch editor's view), `ws:seqpos` | ✅ (keyboard cursor, spoken cell, large cells; a phone: 44 px cells both ways, the roll scrolls sideways) |
| Track chips (the seq tracks), choose the shown one | `cmd seqed=show:T` (also moves the touchscreen's shown track) | ✅ |
| Add / delete / move / length / velocity, undo / redo | `cmd seqed=add|del|move|len|vel|undo|redo:...` | ✅ (drag a note to move it, by its end to stretch it, while drawing; keys; the note panel) |
| Chord stamp, strum, humanise | `cmd seqed=chord|strum|human:...` | ✅ (strum and humanise over the steps on screen) |
| Poly per track: voices, alloc, unison, detune, spread | `cmd seq=poly:...`; `ws:status` `poly`, `inst` | ✅ |
| Pattern: key, scale, QNT, seed | `cmd seq=key|seed`; `ws:status` `seqx` | ✅ |
| Per track: speed, direction, random notes, seed mod | `cmd seq=spd|dir|rnd|smod`; `ws:status` `seqx.t[]` | ✅ |
| Step editor: ratchet / arp, FX 1/2, random notes, lock condition, slide | `cmd seq=rtrg|fx|rndstep|lcond|slide`; `ws:status` `seqx.t[].x[]` | ✅ (follows the selected note) |
| Swing | `cmd seq=swing:T:N`; `ws:seq` `swing` | ✅ |

## Morph

| Feature | Data | Status |
|---|---|---|
| Scenes and MORPH: 12 pads, A / B, the fader, a scene's locks (value, add, remove), copy, clear, EDIT | `ws:status` `scene` (0.5.0, core/mod/scene.h); `cmd scene=ab|fader|sel|lock|unlock|clear|copy|edit` | ✅ against the mock and the 0.5.0 spec; a 0.4.x Sidecar shows "comes with 0.5.0" |

## Rec

| Feature | Data | Status |
|---|---|---|
| Record / arm / stop, timer, free space, mode, bits, destination, take name | `cmd rec=...`; `ws:status` `rec`, `stor` | ✅ (the Sidecar's settings are the truth: no page-side copy) |
| 25 channels with peak meters, select all / none, card load | `cmd rec=ch:`; `ws:status` `rec.ch`, `rec.peak` | ✅ (the A4's channel names ⬜) |
| Warnings (drops, silence, not mounted) and the card line | `ws:status` `rec` | ✅ (and the 70 % confirm) |
| Takes list (SD and drives), sort, play on MAIN, download, delete | `GET /fs/ls`, `/fs/get`; `POST /fs/del`; `cmd play=...`; `ws:status` `play` | ✅ |
| Sampler and auto-sampler | `cmd samp= / asamp=`; `ws:status` `samp`, `asamp` | ✅ |

## Library

| Feature | Data | Status |
|---|---|---|
| Pool: list, bank, RAM bar, rows, accept / replace / unload / select unused / undo | `GET /pool`; `cmd pool=...`; the answer from `ws:log` | ✅ (replace takes a typed path; the old page's mini file picker ⬜) |
| JUNO / FM pool: add or set a preset from the index | `GET /presets`; `cmd preset=...` | ⬜ |
| Files: browse, sort, hidden, download, new folder, rename, delete, upload files and folders (48 KB chunks, CRC-32, resume) | `/fs/ls`, `/fs/get`, `/fs/mkdir`, `/fs/mv`, `/fs/del`, `/fs/put` | ✅ (drag and drop ⬜) |
| +SMP / +WT / +INST into a pool bank, fill bank | `cmd pool=<k>:add|fill:...` | ✅ |
| Projects: table, export (browser via the service worker / card), progress, cancel, delete, Rytm file | `/bundle/*`, `/bundle-sw.js` | 🔶 (table, export to the card, progress, cancel, delete; to the browser and the Rytm file ⬜) |
| Bundles on the card and USB sticks: import (plan, slot, skip machines), delete, eject | `/bundle/*`, `/usb/*` | 🔶 (the card's list, delete; import and sticks ⬜) |
| Trash: restore, remove, empty | `/bundle/trash*`, `/bundle/prjslot` | 🔶 (restore where it was, remove; to another slot, empty ⬜) |

## Settings

| Feature | Data | Status |
|---|---|---|
| Theme, text size | per device | ✅ |
| Audio: MIDI notes, output main / tracks, IN L/R (FX return), computer audio, FIFO target | `cmd notes= out= fxin= mac= fifo=`; `ws:status` | ✅ (pending → saved, as the old page) |
| USB-C mode (class / Overbridge, with the warning) | `cmd usbmode=`; `ws:status` `usbMode` | ✅ |
| Display: screensaver, brightness | `cmd screensaver= brightness=`; `ws:status` | ✅ |
| Wi-Fi: status, on/off, scan, join (follows the restart), forget | `/wifi/status`, `/wifi/scan`, `POST /wifi/join|forget|power` | ✅ (following the Pi to a new address after the join: ⬜) |
| Access: gate, devices, forget this device, forget every device | `/pair/status`; `POST /pair/forget`, `/pair/revoke` | ✅ |
| MIDI bridge: state, token, on/off, OS upgrades, drive writes | `ws:status` `midibridge`; `GET /midibridge`, `POST /midibridge/set` | ✅ |
| Storage: volumes, eject, bench, SD speed test, sample library | `ws:status` `stor`, `sdtest`, `library`; `cmd stor= sdtest=` | ✅ |
| About: version, commit, build, Circle, address, link | `ws:status` | ✅ |

## Not carried over, or not in the old page either

- Firmware / OS updates: not in the old page (sdboot over TCP 7310, the USB-C MIDI or the MIDI bridge). ➖ for now.
- The diagnostics-only `/status` members (`stall`, `gaps`, `mb`, …): a raw view in Settings › About may do. ➖
- The 503 retry and "two requests at once" rules of the old fetch wrapper: kept in `src/lib/api.ts` for the HTTP paths.
