# Changelog

Versions follow [semantic versioning](https://semver.org/): a new major
version changes the API, a minor one adds to it, a patch fixes or speeds up
code without API changes. Every release states the nam2c code generator
version it was generated with (`VERSION`, `MANIFEST.json`).

## 2.0.0

First release as a repository. Upgrading from the 1.x tarball: see
[MIGRATION.md](MIGRATION.md).

**API (breaking)**

- Engines are prefixed per model (`nam_a2lite_*`, `nam_a2full_*`; files
  `nam_a2lite.{c,h}`, `nam_a2full.{c,h}`), so both models link into one
  program.
- The engines keep no state of their own. The caller owns the capture
  (`nam_a2*_weights_t`), each instance (`nam_a2*_t`) and its ring buffers
  (`nam_a2*_state_t`), and places them; `NAM_DTCM` is gone. Any number of
  instances can run, sharing captures.
- `nam_a2*_init(inst, weights, state)`, `nam_a2*_set_weights`,
  `nam_a2*_load_weights(dst, w, n)`.
- Generated prewarm: `nam_a2*_prewarm(inst)` and `NAM_A2*_PREWARM_SAMPLES`.

**Block size**

- Cortex-A76: one engine per model holds the tile shapes for every block
  size; `-DNAM_A2*_BLOCK_HINT=n` selects them at compile time. The `-b16`
  A76 engines are gone.
- Cortex-M7: a `-b16` engine ships only where it differs from the default
  (`cortex-m7/a2lite-b16`).

**Examples, tests, benchmarks**

- `example/a2_engine.{c,h}`: one engine slot for either model, chosen from
  the `.namb` file, with the two models sharing memory. The host example, the
  M7 integration sketch and both benchmarks use it, so each runs either
  capture without a rebuild.
- `test/m7emu`: runs the Cortex-M7 assembly under emulation against the
  NeuralAmpModelerCore references.
- Continuous integration for every release tag.

**Performance** (cycles/sample, block 48 / 16, measured on the boards)

| | 1.x | 2.0.0 |
|---|---:|---:|
| Daisy Seed, A2-Lite | 4,076 / 4,437 | 4,059 / 4,403 |
| Daisy Seed, A2-Full | 19,262 / 19,754 | 18,972 / 19,399 |
| Cortex-A76, A2-Lite | 441 / 512 | 447 / 519 |
| Cortex-A76, A2-Full | 2,019 / 2,155 | 1,981 / 2,157 |

Outputs are unchanged.
