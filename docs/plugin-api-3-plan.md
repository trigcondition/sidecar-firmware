# Plugin API 3: the implementation plan

The proposal is the docs site's developers/plugin-api-3 page (not changed by this work). This file says what the engine
implements of it, in which order, what is deferred and why, and where the engine differs from the page. The header is
`sdk/rmp/rmp_api3.h`; API 2 is `sdk/rmp/rmp_api.h` and docs/plugins.md.

Branch `feature/plugin-api-3` off `dev` (0.4.0). For 0.5.0 ("the new ABI spec implemented").

## The parts, by dependency and value

| # | Part | Needs | Value | Tonight |
|---|---|---|---|---|
| 1 | `rmp_api3.h`, discovery (`reserved[RMP_H_V3]`, `reserved[RMP_P_V3]`), capability flags | - | everything else hangs on it | yes |
| 2 | The loader: dual packages and API 3 only packages next to API 2, the table checked, `needs` / `wants` | 1 | ports can start | yes |
| 3 | Guarded control calls (`init`, `setup`, `voice_init`, `voice_attach`, `voice_enter` / `leave`, `params`, `note_on` / `off`, `load_entry`): a fault mutes the slot, the engine plays on | 2 | safety: the gate for third-party code | yes |
| 4 | The slot arena, reserved at load, `setup`, `voice_attach` | 2, 3 | memory: delays, loopers, granular | yes |
| 5 | Host services: the transport snapshot per block | 2 | tempo-synced effects and machines | yes |
| 6 | Stereo output (`out_channels = 2`) | 2 | PRE / POST voices in stereo | yes |
| 7 | Effects: `RMP3_KIND_EFFECT`, the UFX1..UFX4 slots, the send FX position | 2..6 | user effects | yes: the ABI, the slots, the send FX position, an example |
| 8 | Effects on INSERT, bus and master positions | 7 | more places | deferred |
| 9 | Audio input (`aux[]`), the 12-channel input ring | 7 | vocoders, sidechains | deferred |
| 10 | Block-rate parameters (`b->p`), the manifest's `mod` hints (RX31) | packer, modulation layer | cheaper modulation | deferred |
| 11 | The signal bus | 6, routes UI | cross-slot signals | deferred |
| 12 | Declared pages (RX31 `pages`, `view`) | packer, remote UI, touch UI | own pages | deferred |
| 13 | Endless-loop recovery: core 0 watching core 3, the boot skip (`!CRASH`) | 3, kernel | robustness | deferred |

Why deferred: 8 to 12 each touch a subsystem the 0.5.0 work also changes (the INSERT chains, the looper's input ring,
the modulation layer with morph scenes, the remote pages) and need the owner's choices first (the open questions on
the page: 6, 7, 8, 10, 11, 12). 13 is kernel work that only hardware can test (a timer interrupt or a heartbeat
watchdog on core 0), and agents do not build kernels.

## What is in the engine

### Phase 1: the foundation

- **The header**, `sdk/rmp/rmp_api3.h`, as the proposal has it (every struct, the sizes asserted on AArch64 and the
  offsets in tools/tests/test_rmp_api.c). `rmp_api.h` keeps `RMP_API 2` (a plugin still writes `.api = 2u`) and adds
  `RMP_API_RMPA 3`: the newest API an RMPA package may have. The Teensy's loader stays at 2 for RMP1 packages.
- **The packer** (tools/rmp_pack.py) takes an `api` up to 3 for RMPA, checks the `rmp3_plugin` that `reserved[4]`
  points at (inside the image, magic, size, kind; an effect must be API 3 only) and sets the header flags
  `RMP_HF_API3` and `RMP_HF_EFFECT` (rmp_format.h).
- **The loader** (core/user/um.cpp) loads API 1, 2, dual and API 3 only packages. Before any plugin code runs it checks
  the API 3 table: inside the image's code part, magic, size, kind, every function pointer of both tables inside the
  code, the signal and channel counts, `states` and `latency_frames` in range, and `needs` against the host's caps
  (a slot then shows BAD, "needs signal bus"). An API 3 plugin renders through `rmp3_plugin.render` with one
  `rmp3_block`; an API 2 one exactly as before. The host table: `rmp_host.reserved[RMP_H_V3]` holds the
  `rmp3_host` for every API 2 or 3 plugin (an API 2 plugin never reads it); an API 3 only plugin gets `host->api` 3.
  Caps offered now: `TRANSPORT`, `GUARD`, `ARENA` (when the board has a pool).
- **Host services:** `um::pre_block()`, called by the control core before each block (the Pi's `bridge::seq_block`,
  the Mac lib's render loop), writes the block's `rmp3_transport` from the LFOs' clock (core/lfo.h `clock_state`)
  into one of two snapshots and publishes it with a release store; the render cores read the newest. Flags RUNNING,
  CLOCKED, LOCKED, START and STOP for one block, JUMP on a song position message or a position that went back.
  `lead_frames` is the proposal's 385.
- **Guarded control calls:** every plugin call on the control core goes through `gcall` (`init`, `voice_init`,
  `voice_attach`, `voice_enter`, `voice_leave`, `params`, `note_on`, `note_off`, `load_entry`; `setup`). The board
  supplies `Exec::guard`: on the Pi, core 3's own jump buffer in pi/engine/guard.cpp (the exception handler returns
  into it as it does for a render core); on the Mac, `sigsetjmp` and a signal handler (core/host/um_host.cpp). A fault
  mutes the slot (silent, no further calls, the JSON's `"muted":true` and `why` "muted: crashed in params at +0x84")
  and the engine plays on; `init` or `setup` faulting refuses the package (BAD). A fault also mutes every slot that
  shares the faulting slot's image (its globals may be what the fault damaged): a control call's, a voice's render
  (the board's render guard tells um: `um::render_fault`) and an effect's on core 2; a load never shares a muted
  slot's image. Late calls and slow effects mute only their own slot. API 3 plugins are also held to the
  time limits: a control call over 500 us three times in a row mutes the slot, and one over 2 ms (the FIFO: it has
  already been a dropout of every track) mutes it at once; `init`, and each `setup` call, over
  1 ms refuses the package (one `setup` call up to 2 ms is forgiven: an interrupt in the wall time) (decision 1: `setup` may return `RMP3_SETUP_MORE` and is called again from the next idle
  step, so heavy work goes in pieces). `um=reload:<slot>` loads a slot again.
- **The slot arena:** `Exec::arena` gives um one pool at boot (the Pi: `plugram=` MB from the heap before the pool
  RAM, default 64 / 128 / 256 MB on 2 / 4 / 8 GB, none under 2 GB; the Mac: 64 MB). A slot's arena is reserved when
  it loads: the shared part and `states` voice parts, each the plugin's `want`, else its `min`, at most a quarter of
  the pool a slot; zeroed in 256 KB pieces, one an idle step, the slot LOADING meanwhile; then `setup`. Voice `vi`
  gets part `vi`, poly instance `i` part `8 + i`. `states` defaults to 8, 40 for a poly machine (`RMP_HF_INSTANCES`),
  2 for an effect. A plugin with no voice part (no arena) never runs out of states. A state past `states` is refused
  in words (the log once, the slot's `why`: "no room: poly instance 3 past the arena's 8 states"). Two slots of one
  package share the image and get an arena and a `setup` each. A slot's block comes back when the slot loads again,
  empties or takes another plugin: to the pool's top, or to a short list of free blocks any load it fits reuses.
  Only when the free space is there but in pieces does a load reload every slot (a full reload packs the pool).
  Arena parts are not cleared between occupants (a voice's part keeps the last track's data: clear what you read
  before writing) and have no guard words: an overrun into the next part is not caught.

### Phase 2: stereo

A plugin of `out_channels = 2` on a voice routed OUT PRE or POST gets `out[1]` and writes both channels (a new hook,
`user_render2`, beside `user_render`; core/voices.cpp keeps such a voice's R instead of copying L). On TRK, `out[1]`
is null and `RMP3_B_MONO_OUT` set: the plugin writes its mono mix. A mono plugin is unchanged (R = L). The NaN and
level clip runs per channel. Not yet: poly instances of a stereo plugin render mono (their `out[1]` is null).

### Phase 2: user effects (the send FX position)

One ABI for machines and effects: an effect is an API 3 only package (`api 3`, `RMP3_KIND_EFFECT`); the same loader,
checks, arena and guard as a machine. The load targets UFX1..UFX4 sit next to U1..U5 in core/user/um.cpp (indices 5..8,
never in the Rytm's slot protocol). `um=fx:<n>:<id>` puts an installed effect in UFX`n` (kept per project, item 11; the TYPE list's
name and tag in `fx::user[]`); `um=fx:<n>:-` empties it, `um=reload:f<n>` loads it again. A machine slot refuses an
effect and an effect slot a machine (BAD, in words). An effect gets two states, send FX 1 and send FX 2, set up on the
control core before core 2 may run them (`voice_init`, `voice_attach` with place `RMP3_AT_SEND`, voice -1, instance
0 / 1, an arena part each).

The send FX (core/fx/send_fx.h) plays TYPE 64..67 through two hooks (`fx::user_ready`, `fx::user_run`): mono in
(stems x send levels), stereo wet out, `RMP3_B_WET_ONLY`, in blocks of up to 16 frames, on core 2 after its block.
The knobs: the send slot's P2..P5 are the effect's P1..P4, handed in `b->p` with `p_changed`. TYPE changes fade as for
the built-in effects (the host's). The render is guarded on core 2 too (the Pi's guard takes core 2's calls outside a
voice): a fault mutes the effect. `fx::state` says READY for a loaded effect whose tag matches the kit's.

The Rytm's labels: when a UFX slot loads, the host link sends the Rytm a 0x24 SENDFX LABELS for its TYPE (64..67)
built from the package's manifest (core/user/um.cpp `fx_labels_build`; the knobs' short names, a list knob's names as
zones, `#` for a number inside a zone, adjacent equal names merged: docs/plugins.md 12b). The Rytm mod (0029) already
took 0x24 for any TYPE id, so no flash is needed; before, a user effect's P2..P5 read P2..P5.

The effects: plugins/ufx_phaser (PHASER) and plugins/ufx_bbd (BBD), docs/plugins.md 12b.

The example: plugins/ufx_pingpong, PINGPONG, a tempo-synced stereo ping-pong delay (MIT, written for this repo):
TIME in the Rytm's tempo from the transport (1/16 .. 1 BAR), FDBK, TONE, WIDTH; its delay lines in the arena (1 s a
side at least, 4 s wanted). core/tests/test_api3.cpp runs it through `SendFx::Process` on the host: no output until
the echo, the first echo on the left at the tempo's 1/8, the feedback's on the right at twice that.

The effect's budget (as the voices' guard): its render is timed on core 2; 16 calls of one state (send slot) in a row
over 120 us a 12-frame block (scaled by n), or one call over 2 ms, mute it ("muted: render ran 2400 us, over its
budget"). All of core 2's effect calls in a block are summed too: 16 blocks in a row over 150 us mute the dearest
effect of the last one. `um=fxbudget:<call>:<total>` sets both (tests, a bench on the Pi). A state not called for
more than two blocks (its slot played another TYPE) takes no older edges. The send FX's
transport snapshot is read once a call; its edges (START, STOP, JUMP) reach each of an effect's states exactly once,
in the first sub-block, however core 2's send FX falls against the blocks (each snapshot carries the edges' running
counts). A kit made for another effect (its tag) plays silence in that slot, not the effect UFX`n` holds; a user TYPE
that goes away (unloaded, muted) fades in again when it comes back. A rescan follows the UFX slots as it does U1..U5
(a package changed reloads them, the package indices follow the new list). Changing a UFX first takes it off core 2
with a bounded wait (5 ms): an effect that never returns is left behind (its memory never reused) and core 3 goes on,
so its own watchdog sees core 2 stuck and the board restarts, as for a hung voice.

Not yet: the INSERT, bus and master positions (part 8), a fresh state on a TYPE change back to the same effect.

Tests: core/tests/test_api3.cpp (the probe plugin plugins/api3_test in six variants, kept built in
core/tests/api3_pkgs), core/tests/test_api3_compat.cpp (below), tools/tests/test_rmp_api.c,
tools/tests/test_rmp_pack.py `Api3Flags`.

## Proof that API 2 is unchanged

`core/tests/test_api3_compat.cpp` plays every RMPA package built under plugins/ (16 of them; `make -C plugins/<p>
a64`, the rytm-circle Docker image when no AArch64 compiler is installed) on voice 0 through the engine: a trig, a knob
change with fractions mid-note, a note off, a second trig, 800 blocks, and hashes the voice's int16 samples.
`core/tests/golden/api2_compat.txt` was written by the API 2 host (this branch's base, `dev` e2de6c5, with only the
test added); the API 3 host must give the same hash for every package. Two details make the runs repeatable: a fake
clock (a block is 250 us) and the user machines' heap at a fixed address (plugins/hush1 seeds its noise from its
state's address). plugins/fault_test is left out (it faults on purpose). Without built packages the test passes with
a line saying it compared nothing (CI has no AArch64 compiler).

## Decisions and divergences from the proposal

Each: what the page says, what the engine does, why. Listed for the owner.

1. **`init` and `setup` run on the control core, not a core 0 task, with 1 ms a call and `setup` in pieces.** The page
   moves them to a setup task on core 0 with 100 ms of room. The engine runs both where `init` always ran, in the
   load's idle step on core 3, guarded and timed. Core 3 renders no block while a call runs and the FIFO holds 2 ms, so
   100 ms would be a dropout of every track at each load: the limit is 1 ms (an API 3 plugin's `init`, and each
   `setup` call), past it the package is refused, and `setup` may return `RMP3_SETUP_MORE` (rmp_api3.h: "MORE") to be
   called again from the next idle step, the engine playing in between (4000 calls at most). The page's "nonzero =
   refuse" keeps holding for every other value. Why not core 0: a guarded task there needs a second guarded context
   on the core that runs USB, the card and the web, and the page's open question 2 says its cost is not measured.
2. **The arena is zeroed on the control core**, in 256 KB pieces (one an idle step), not by core 0. Same reason;
   256 KB of `memset` is tens of microseconds on a Pi 4. The slot stays LOADING meanwhile, as the page says.
3. **Late calls mute API 3 plugins only.** The page's three-late-calls rule would newly mute existing API 2 machines
   whose `params` happen to be slow (they were never told the limit). Their late calls are counted (`"late"` in the
   JSON); faults are caught for every plugin (strictly better than a reboot).
4. **No MUTED slot state on the wire.** The page says the slot shows MUTED. The Rytm's slot protocol (19..1E) has no
   such state, and adding one is a Rytm mod change (a flash). The slot stays READY to the Rytm, silent; the web's
   JSON and the log say `muted` and why. A Rytm-side MUTED is for a later mod release.
5. **How the scan knows an effect:** the page does not say. The packer sets `RMP_HF_API3` / `RMP_HF_EFFECT` in the
   header from the table; the scan keeps effects out of the Rytm's machine list; the loader checks the table itself.
6. **`rmp3_voice.instance` is -1 for a voice's own state** (the page says "0.. on a poly track" only), and the arena
   part of instance `i` is `8 + i`. The page's default of 8 states would silence a ported poly machine's instances:
   the default is 40 for a package with `RMP_HF_INSTANCES`, and a plugin with no voice part needs no states at all.
7. **`cpu_mhz` is 0** in `rmp3_host` for now (the engine has no clock call in the portable core yet); `board` is 4 or
   5 on the Pi, 0 on the Mac.
8. **The endless-loop recovery** (core 0 watching core 3, the slot skipped at the next boot, `!CRASH`) is not built:
   kernel work that only hardware can test (the plan's part 13). A plugin that loops in a control call on core 3 does
   what it does today (core 3 stops serving blocks, the stream stops); an effect that hangs on core 2 trips core 3's
   20 ms block watchdog (a restart). Faults and slow effects are caught; a hang is not, yet.
9. **Also refused at load:** an API 1 / 2 table with a null `init` or `render` (it crashed before; now BAD), and
   function pointers outside the image's code part (an API 3 table only: an API 2 table is not checked further, so
   nothing that loaded before is refused).
10. **Effects get `b->p` without `RMP3_CAP_BLOCK_P`.** The page makes block-rate parameters a capability for
    modulation. A send-slot effect runs on core 2, where no control call can reach it, so its knobs come in the
    block (the send slot's P2..P5 as P1..P4) and `params` is not called there. The host does not offer `BLOCK_P` to
    machines yet (they still get `params`). This is the page's own design for modulation, used for one more reason.
11. **UFX assignments are kept per project** in `/.rytm/fxslots/<project>.txt` (the file the send FX already reads for
    the kits' tags): a line a UFX, `<tag 4 hex> <name>` as before, and for an assignment of `um=fx` the package
    after a tab: `<tag> <name>\t<id> <a.b.c>` (the slot map's id and version fields, the same exact-version-then-newest
    resolution as U1..U5). Not in `/.rytm/slots/<project>.txt`: an older build fails that whole map on a line it does
    not know, and would lose U1..U5. `um=fx` (set or empty) writes the file at once, atomically (as the slot map);
    it is read where the slot map is read (boot, a project change) and follows a rename or SAVE AS (`um::rekey`). A
    package that is not installed: the slot MISSING (logged), the file as it is, so the assignment returns when the
    package is installed; a later change of another slot keeps that line. A line that does not parse: logged, every
    UFX empty, the file never written over (as the slot map). A project with no file keeps what is loaded until
    `um=fx` changes a slot (which writes its file). A line without a tab (a hand-made or bundled `<tag> <name>`)
    assigns nothing and is kept as it is. Core/tests/test_ufx_persist.cpp restarts the program on the same card.
12. **An effect's state is not fresh on a TYPE change back.** The page says a TYPE change starts a fresh state.
    `voice_init` is a control call and the send FX's TYPE changes on core 2; a fresh state needs a request from core 2
    to core 3 and a fade that waits for it. For now an effect keeps its state (a delay's tail can come back).
