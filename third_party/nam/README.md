# third_party/nam - TONE3000's NAM A2 engines and the .namb format (MIT)

Vendored for INSERT TYPE 9 AMP (core/fx/amp.h, docs/INSERT-FX.md "AMP"). Shallow clones, the `.git` folders removed,
and trimmed to A2-Lite (removed: the A2-Full engines of both targets, `cortex-m7/a2lite-b16`, `weights/a2full.namb`,
`test/golden_a2full_output.raw`, `test/m7emu/`, `.github/`; the README / Makefiles still name them);
nothing here was built or run from these folders (their Makefiles, scripts and Python are untouched and unused):

| Folder | Upstream | Commit | Date | Licence |
|---|---|---|---|---|
| `embedded/` | https://github.com/tone-3000/embedded (v2.0.0) | b84b60c652cb386ea341be50b8c665ae8a489cff | 2026-10-07 | MIT, Copyright (c) 2026 TONE3000 (`embedded/LICENSE`); the example captures and golden outputs: NeuralAmpModelerCore's test models, MIT, Copyright (c) 2023 Steven Atkinson (`embedded/NOTICE`) |
| `nam-binary-loader/` | https://github.com/tone-3000/nam-binary-loader | 92c9e85bb7903ccbfd3264d3f7bcb5fee6c0892d | 2026-04-16 | MIT, Copyright (c) 2026 TONE3000 (`nam-binary-loader/LICENSE`) |

What the Sidecar uses, unchanged, compiled into `core/fx/amp.cpp` (as C++, `restrict` spelled `__restrict`):

- `embedded/engines/cortex-a76/a2lite/nam_a2lite.{c,h}`: the A2-Lite engine (NEON assembly on AArch64: the Pi 4 and an
  Apple Silicon Mac; its scalar stand-in elsewhere), `NAM_A2LITE_MAX_BUFFER_SIZE` 16, `NAM_A2LITE_BLOCK_HINT` 12;
- `embedded/namb/namb_reader.{c,h}`: the `.namb` reader (container, CRC, the A2-Lite shape check).

Used by the tests only (`core/tests/test_amp.cpp`): `embedded/engines/cortex-m7/a2lite/nam_a2lite.c` (its C tiles: the
reference the A76 engine is checked against), `embedded/weights/a2lite.namb` with `embedded/test/golden_input.raw` and
`golden_a2lite_output.raw` (NeuralAmpModelerCore's test capture and output: the engine matches it at -135 dB ESR).
`nam-binary-loader/` is kept as the format's specification (`namb/namb_format.h`) and the `nam2namb` converter's source
(a user converts a `.nam` with it on a computer; the Sidecar also reads A2-Lite `.nam` JSON itself).
