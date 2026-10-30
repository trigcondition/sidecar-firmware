# Versioning

The Sidecar (the Pi 4 "Sidecar" and the Teensy 4.1 "Sidecar Mini", one shared core) uses semantic versioning,
`MAJOR.MINOR.PATCH`, starting at the first public release, **0.1.0**.

- **PATCH**: fixes only.
- **MINOR**: new features (and, while MAJOR is 0, anything that changes how a setup behaves).
- **MAJOR**: 1.0.0 follows the testers' feedback; after it, a MAJOR step means a break in how a setup is used.

One number covers both products and the shared core. The Rytm firmware mods (rytm-mods; builds such as `rui1026`) have their
own build names. **The Rytm build a Sidecar version needs is listed in that version's release notes**: flash the Rytm build named
there, not just the newest.

## Where it comes from

The `VERSION` file at the repo root is the only source (one line, `0.1.0`). `tools/gen_version.sh` turns it into the
generated, git-ignored `core/version.h` with the version, the short git commit and a dirty flag (a tracked file changed,
an untracked file among the build's inputs - core/, pi/engine, pi/ui, pi/circle's patches, teensy/, web/, sdk/, tools/,
notes (*.md) aside - or a tree whose state git could not read; tools/tests/test_gen_version.sh):

```
SIDECAR_VERSION "0.1.0"   SIDECAR_COMMIT "1332761"   SIDECAR_DIRTY 0|1   SIDECAR_VERSION_FULL "0.1.0 (1332761)"
```

Every build runs it: `core/Makefile` (the Mac library and tests), `pi/engine/build.sh` (on the host, before Docker: the
container has no git), `teensy/setup_core_lib.sh` (run by `run_sketch.sh`, so `host_stereo.sh` and `host_uac2.sh`). The
header is rewritten only when its content changes. A build of a changed tree reads `0.1.0 (1332761-dirty)`; release builds
are made from a clean commit.

To release: change `VERSION`, commit, tag `v0.1.0`, build from the tagged commit.

## Where it shows

- Remote UI on the Rytm: the SYSTEM page's title, `SYSTEM 0.1.0`.
- Pi display: About page, the "Version" row (`0.1.0 (1332761)`).
- Pi web page: About tab, "Version"; `/status` JSON `"version"`, `"commit"`, `"dirty"`.
- Sidecar Mini web console (`web/`): About tab, "Version"; the console's `@{"t":"version","version","commit","dirty"}` reply.
- Both consoles: `:version` prints `version: Sidecar 0.1.0 (1332761)`; the boot log has the same line (Pi: the
  `==== rytm-pi engine ====` line, Mini: `boot: Sidecar Mini ...`).
