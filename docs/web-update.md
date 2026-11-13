# Updating from the web page (0.5.0)

From 0.5.0 a Sidecar can be updated from its web page: Settings › Update. No SD card in a computer, no network
download by the Sidecar or the page: you download one file from the release page and pick it on the web page.

## For users

1. Download `sidecar-<version>.scu` from the release page (one file for the Pi 4 and the Pi 5; its `.sha256` beside it).
2. Open the Sidecar's web page, pair this device if it is not paired yet (Settings › Access), then Settings › Update.
3. Pick the file. The page checks it at once: a damaged or incomplete download, a file that is not a Sidecar update or
   one without this board's firmware is refused here, before anything is sent. It then says what it will do:
   "Update from 0.5.0 to 0.5.1". The same version again, or an older one, needs the box "Install it anyway".
4. Install. The file goes up (a progress bar), the Sidecar checks it again and restarts into the new version *on
   trial*: audio stops for about a minute. Leave the Sidecar switched on until the page says it is done.
5. After the restart the page reconnects by itself and follows the trial: "Trying 0.5.1 (abc1234): permanent in 20 s",
   then "Updated to 0.5.1 (abc1234)", or "Failed: back on 0.5.0 (...)" when the new version did not start. Reload the
   page: the update brings its own web app. There is nothing to install twice "so it sticks": the page and the screens
   (below) say when it is permanent.

**On the screens:** while the update is written the touchscreen shows a full-screen notice ("INSTALLING UPDATE - DO NOT
POWER OFF"), during the trial "TRIAL: PERMANENT IN N S", for a few seconds after it "UPDATED TO 0.5.1", and after a
failed trial "UPDATE FAILED - BACK ON 0.5.0". The Rytm's screen gets the same lines as its status popup (shown once a
second, held back while a Sidecar page is open on the Rytm; no Rytm firmware change). All of it from the engine's
memory: the screens never read the card.

**Installing again:** once the trial has made itself permanent a new update can be installed at once; the Sidecar does not
have to be restarted first (the engine keeps saying `"trial":true` in `/update/state` until the next restart, which no
longer matters). Only a job under way (state `staging`, `trying`, `committing`) refuses another.

**A crash report:** Settings › Update › "Read the last crash report" shows `/.rytm/prevboot.txt` (the report and the
log tail of the last boot that did not end by a planned restart), with "Save as a file" for a bug report. Paired devices
only (`GET /prevboot`, unlike `/log`, which is open; capped at 64 KB); the file holds no pairing code or Wi-Fi key.

**When it goes wrong:** if the new version does not start, hangs, or the power goes during the trial, the Sidecar
starts its old version again at the next start, by itself (on a hang the watchdog restarts it; after a power cut
switch it on again). The page then says "the new firmware did not start: the Sidecar went back to 0.5.0 by itself".
Nothing is lost: try again, or update from a card.

**Back to the previous version:** the card keeps the version before the last update. "Back to 0.5.0" on the same
card restarts into it, on trial the same way. A previous version from before 0.5.0 (0.4.x) cannot make itself
permanent: the page warns, and when you go ahead it is swapped in at once, without a trial; the way up again is then the
SD card in a computer (or the page, once the older version runs a web update of its own: it does not).

**Turning it off:** `webupdate=0` in `cmdline.txt`. Off by itself without a card, in the engine test modes, and on a
Pi 4 card that does not start through sdboot (`config.txt` without the sdboot block's line
`initramfs engine.img 0x02000000`): such a card is updated in a computer.

**Who can update:** the same as any other change on the page: a paired device (the code on the Rytm's screen) or the
trusted USB-C cable, and only from the page itself (the strict same-origin check). Release cards keep `sdport=0` (no
boot-file writes over TCP 7310): the web update is a separate, narrower way to the boot files: only a whole, checked
update file, only through a trial boot that falls back by itself.

## The update file (`.scu`)

`tools/make_update.py` makes it (tests: `tools/tests/test_make_update.py`); sidecar-expander's
`scripts/package_pi.sh` calls it with `--release` after the card sets. Little endian, a 512-byte header, then the
parts back to back up to the file's end.

| offset | size | field |
|---|---|---|
| 0 | 8 | magic `SCUPDAT1` |
| 8 | 4 | header bytes, 512 |
| 12 | 4 | format, 1 |
| 16 | 4 | parts, 1 to 6 |
| 20 | 4 | flags (bit 0: a -dirty build) |
| 24 | 24 | version, NUL padded (`0.5.1`, `0.5.0-rc1`) |
| 48 | 24 | commit as the kernels stamp it (`abc1234`, `abc1234-dirty`) |
| 80 | 6 × 64 | parts: kind u32, flags u32, offset u64, size u64, sha256[32], reserved[8] |
| 480 | 32 | sha256 of bytes 0 to 479 |

Kinds: **1** the Pi 4's `engine.img` (sdboot's `SDCRENG1` header and sums around kernel8-rpi4.img, at most sdboot's
7.5 MB), **2** the Pi 5's `kernel_2712.img` (at most 16 MB), **3** the web app: `SCWEB001`, a u32 count, then per file
u16 name length, u16 0, u32 size, the name, the bytes; web-next's `dist/` as `scripts/card.mjs` puts it on the card
(text files as `.gz` only), names as `webnext_path.h` allows, at most 1024 files and 8 MB.

Every kernel must be a Sidecar engine for its board and carry the update code's stamp
`sidecar-webupdate-1 v=<version> c=<commit>` matching the header: a kernel without it could not make itself
permanent, so the tool refuses it. The sha256 sums guard against damage, not against a forged file: whoever can
install can already pair. Signing is a possible later step.

## The Pi's side

`pi/engine/update.cpp` (portable, host tests in `pi/engine/tests/test_update.cpp`), `update_pi.cpp` (the boot check,
the commit task, the reboot), `web.cpp` (the endpoints).

| endpoint | what |
|---|---|
| `GET /update/state` | open, as /status: `{"api","enabled","why","board","version","commit","trial","state","kind","target","from","msg","prev","prevVersion","uploaded","commitIn","uptime"}` |
| `POST /update/put?off=&total=&crc=` | the file in 48 KB chunks, `/fs/put`'s protocol (`200 ok <next>`, `200 done <n>`, `409 expected <off>`), to `/.rytm/update/update.scu` |
| `POST /update/install` `[force=1]` | checked again (header, every part's sha256, this board's kernel and its stamp, the web archive, the version, free space), staged as the trial; `{"ok","code","msg","reboot":"tryboot"}`, the reboot half a second later |
| `POST /update/previous` `[force=1]` | the `.prev` kernel as a trial; one without the update code only with `force=1`, swapped at once (`"reboot":"plain"`) |
| `POST /update/discard` | the uploaded file deleted |
| `GET /prevboot` | (paired) `/.rytm/prevboot.txt`, text, at most 64 KB: the last crashed boot's report and log tail; 404 when none |

From the install until its commit ends (or the next boot reverts it; not for the rest of a trial boot that has committed:
see "Installing again" below) `put`, `discard` and a second install are refused: the uploaded
file stays the one install checked. The commit unpacks the web app only if its sha256 is still the one install
recorded (`state.txt` `wsha=`), with every name checked again: a file changed by any other way to the card is not
unpacked (the kernel still commits; the old web app stays).

Paired (or the trusted cable) for everything but `/update/state`, and the strict same-origin check on every POST.

### The write order

Install, on the running (old) engine, writes only new files: `/.rytm/update/state.txt` (`staging`), the trial kernel
(`engine-try.img` on the Pi 4, `kernel_2712-try.img` on the Pi 5: written as `.part`, every piece read back, the sha256
and on the Pi 4 the SDCRENG1 sums checked on the card, then renamed), `cmdline-try.txt` (cmdline.txt's line plus
`updtrial=1`), `tryboot.txt` (first line `# sidecar web update`; the Pi 4: config.txt with the sdboot block's line
changed to `initramfs engine-try.img 0x02000000`, then `[pi4]` `cmdline=cmdline-try.txt`; the Pi 5: config.txt
without the sdboot block, then `[pi5]` `kernel=kernel_2712-try.img` `cmdline=cmdline-try.txt`), state `trying`, and
last the firmware's one-shot tryboot flag. `config.txt`, `cmdline.txt`, `engine.img`, `kernel_2712.img` and sdboot are
never written by the install. A power cut anywhere there boots the old engine from config.txt.

### Trial, commit, fallback

The trial boot reads `tryboot.txt` once; `updtrial=1` on its cmdline tells the engine it is the trial. The trial arms
the watchdog from its first boot step. After 30 s of uptime a task on core 0 commits: the stamp must match the
target, the older `.prev` goes, the running kernel becomes `.prev`, the trial kernel takes the real name (these two
renames are the one moment without a kernel under the boot name: FAT has no atomic replace, the same window as
`sdboot.py install`), then the trial files go, the web app is unpacked to `/.rytm/www-next.new` and swapped in
(`www-next` becomes `www-next.prev`), and state `committed`. Each step is resumable: a power cut in the commit is
finished by the next boot (whichever kernel the firmware then finds).

Any reboot before the commit boots `config.txt`, the old engine: it finds state `trying` without `updtrial=1`, records
`reverted` with the reason, and deletes the trial files (`tryboot.txt` only when it is ours). sdboot is not changed:
the Pi 4 gets its fallback from the firmware's tryboot, as the Pi 5 (measured on both boards, pi-engine.md
2026-10-16). A Pi 4 whose bootloader ignores the flag boots the old engine, which says so: no update, nothing broken.

Every boot that sorts out an interrupted job writes its verdict (`reverted`, `failed`) before it deletes the trial
files, so a cut in that boot is finished by the next one and never mistaken for a commit.

"Previous" goes by the `.prev` kernel's own stamp (`prev.txt` can be stale: a card changed in a computer);
`prev.txt`'s `w=1` says that `www-next.prev` is that version's web app, and only then is it swapped back with it.

Residual risks: the commit's rename window (above), and the same window in the direct swap to a version without the
web update (its kernel moved aside, then the previous one in place); a trial that crashes only after its 30 s is committed (the
previous version stays one button away); a Pi 4 `engine-try.img` whose sums fail at boot stops in sdboot's serial
bootloader with its watchdog off: power cycle, then the fallback runs.

### Installing again after a commit (ruling, 2026-11-12)

`Env.trial` (cmdline `updtrial=1`) says how this boot started; it stays true after the trial has committed, until the
next restart. It used to refuse a new install ("a trial is running") for all of that time. It is safe to allow one:
the firmware's tryboot flag is one-shot (the firmware clears it on the trial boot, install sets it again), the
committed kernel is already under the boot name and the old one is `.prev`, the trial's files (`tryboot.txt`,
`cmdline-try.txt`, the trial kernel) are gone, and the new install writes only new files exactly as the first one did.
So `in_flight()` looks at the state only. Two guards keep the old boot out of the new job: its commit task has ended,
and `commit_due()` / `commitIn` also require that the staged target is this very engine (`tv`/`tc` equal to the
running version and commit); a new job's target never is. If the new trial then does not commit, the next plain boot
reverts to the committed kernel, as always (`test_update`: "install after a commit on the same boot").

### The update file's web part

`make_update.py --web web-next/dist` (or `--build-web`, which runs `npm run build` in web-next first; it needs
`web-next/node_modules`, so run `cd web-next && npm ci` once; the tool installs nothing) puts the web app in the file.
The release (sidecar-expander `scripts/package_pi.sh`) builds web-next and passes it. A file **without** a web part (the
kernels-only trial files) leaves `/.rytm/www-next` and `www-next.prev` alone and `prev.txt` says `w=0` (a "previous"
then does not swap the web app back). A trial/dev update with the page's fixes needs the web part, since the page is
part of what changed: build it with `--build-web`.

### Tests

`make -C pi/engine/tests` runs `test_update` (header and part checks, both boards' boot files, the refusals, a whole
install → trial → commit on a folder card through a firmware simulator, the revert, previous with and without the
update code, and a power cut after every single card operation of the install, the commit and the previous swap:
after each, the next boot must load either the old or the new kernel byte for byte, with exactly one cut point in the
rename window; the install after a commit on the same boot, the screen notices through install, trial, commit and
revert, an update without a web part) and `tools/tests/test_make_update.py` (also `--build-web`'s refusal without
node_modules). web-next: `src/lib/update.test.ts` (SHA-256 vectors, the header as make_update.py writes it, verdicts
incl. a committed trial boot, the follower after the restart, the texts, the upload to `/update/put`) and
`src/lib/prevboot.test.ts`.
