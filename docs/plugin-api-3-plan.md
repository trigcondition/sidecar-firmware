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

(Filled in as the phases land; the report in the private notes has the commits and test runs.)

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

(Each: what the page says, what the engine does, why. Listed for the owner.)
