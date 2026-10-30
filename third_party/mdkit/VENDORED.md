# third_party/mdkit - Soejrd's mdkit drum voices (CC0 1.0), vendored

| Upstream | Commit | Vendored | Licence |
|---|---|---|---|
| https://github.com/soejrd/mdkit | 96157fe4fbda17bf4cae3168f52437d8232e6570 | 2026-10-29 (`git archive` of that commit, unchanged) | CC0 1.0 Universal (`LICENSE`, `NOTICE`) |

Vendored because DIGIDRUM's upstream (the same author's soejrd/digidrum) disappeared (404, 2026-10-29), and the
built-in MD engine (voice byte 0x40, `core/md/`) and the MD user machines (`plugins/mdkit`) now build from it with no
network. The whole tree at that commit, nothing edited; the developer agreed to the Sidecar's use (plugins/mdkit/README.md).

What is compiled (unchanged, but for the PI cymbals' delay window: `plugins/mdkit/patch_pi.py` writes patched copies of
PI-RC, PI-CC, PI-HH next to the build, never here): `include/` and the 30 voices under `src/efm`, `src/pi`, `src/trx`.
`examples/`, `tests/`, `tools/`, `website/` and the `Makefile` are kept as upstream ships them and are not built here.
