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
  and the engine plays on; `init` or `setup` faulting refuses the package (BAD). API 3 plugins are also held to the
  time limits: a control call over 500 us three times in a row mutes the slot, `init` or `setup` over 100 ms refuses
  it. `um=reload:<slot>` loads a slot again.
- **The slot arena:** `Exec::arena` gives um one pool at boot (the Pi: `plugram=` MB from the heap before the pool
  RAM, default 64 / 128 / 256 MB on 2 / 4 / 8 GB, none under 2 GB; the Mac: 64 MB). A slot's arena is reserved when
  it loads: the shared part and `states` voice parts, each the plugin's `want`, else its `min`, at most a quarter of
  the pool a slot; zeroed in 256 KB pieces, one an idle step, the slot LOADING meanwhile; then `setup`. Voice `vi`
  gets part `vi`, poly instance `i` part `8 + i`; a state past `states` stays silent. Two slots of one package share
  the image and get an arena and a `setup` each. The arenas come back with the code's at a full reload.

### Phase 2: stereo

A plugin of `out_channels = 2` on a voice routed OUT PRE or POST gets `out[1]` and writes both channels (a new hook,
`user_render2`, beside `user_render`; core/voices.cpp keeps such a voice's R instead of copying L). On TRK, `out[1]`
is null and `RMP3_B_MONO_OUT` set: the plugin writes its mono mix. A mono plugin is unchanged (R = L). The NaN and
level clip runs per channel. Not yet: poly instances of a stereo plugin render mono (their `out[1]` is null).

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

1. **`init` and `setup` run on the control core, not a core 0 task.** The page moves them to a setup task on core 0
   with 100 ms of room. The engine runs both where `init` always ran, in the load's idle step on core 3, guarded and
   timed: a fault refuses the package, over 100 ms too (an API 3 plugin). Why: a guarded task on core 0 needs a second
   guarded context on the core that runs USB, the card and the web, and the page's own open question 2 says its cost
   is not measured. A long `setup` on core 3 costs one control pass (a glitch while it runs, as a long `init` does
   today); the 100 ms rule makes such a package a refusal, not a habit. Revisit with measurements on the Pi.
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
   part of instance `i` is `8 + i`: the default 8 states cover the voices, a poly machine sets `states` up to 40.
7. **`cpu_mhz` is 0** in `rmp3_host` for now (the engine has no clock call in the portable core yet); `board` is 4 or
   5 on the Pi, 0 on the Mac.
8. **The endless-loop recovery** (core 0 watching core 3, the slot skipped at the next boot, `!CRASH`) is not built:
   kernel work that only hardware can test (the plan's part 13). A plugin that loops in a control call on core 3 does
   what it does today (core 3 stops serving blocks, the stream stops); only faults are caught.
9. **Also refused at load:** an API 1 / 2 table with a null `init` or `render` (it crashed before; now BAD), and
   function pointers outside the image's code part (an API 3 table only: an API 2 table is not checked further, so
   nothing that loaded before is refused).
