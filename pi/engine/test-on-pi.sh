#!/bin/bash
# pi/engine/test-on-pi.sh - the Pi engine's on-Pi checks that could not run when their task was built (the Pi was
# unreachable): each step flashes the kernel over the serial cable (pi/circle/flash.sh, SECS >= 15) and checks
# its log. Needs the Pi 4 with the serial bootloader card (docs/pi-circle-spike.md) on /dev/cu.usbserial-*,
# running a kernel that answers the reboot magic, or freshly power-cycled (the bootloader then waits).
#   pi/engine/test-on-pi.sh [all]      every step in order (the watchdog step last), then a PASS/FAIL line per step
#   pi/engine/test-on-pi.sh STEP...    only those (e.g. task3-selfcheck task7-bench)
#   pi/engine/test-on-pi.sh --dry-run [STEP...]   the steps that would run, in order; touches nothing
# Exit status 0 when every step run passed; each check prints PASS or FAIL with the reason. About 45 minutes for all.
# Steps that need the Rytm are listed as DEFERRED (Rytm) comments and are not run here.
# The Rytm must be OFF or unplugged from the Pi for every step here: task4-boot's expected stats line (fifo full, no
# blocks) and the self-test (refused while a Rytm is connected) assume no Rytm.
set -uo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
FAILS=0

pass() { echo "PASS  $STEP: $*"; }
fail() { echo "FAIL  $STEP: $*"; FAILS=$((FAILS + 1)); }

# flash SECS: build the kernel, flash it, log SECS seconds; sets LOG to the log's path. The new kernel's banner must
# be in the log: flashy always writes its own "Opening ... / Waiting for device" lines, so a non-empty log proves
# nothing. Without the banner PI_DEAD is set and the remaining steps are skipped (a dead Pi is not re-flashed at
# every step). TEST_ON_PI_FAKE_LOG=FILE (testing this script only): no build, no flash, LOG=FILE.
PI_DEAD=0
flash() {
  local secs=$1
  [ "$secs" -ge 15 ] || { echo "flash: SECS must be >= 15 (a shorter monitor can leave the Pi wedged)"; exit 2; }
  if [ -n "${TEST_ON_PI_FAKE_LOG:-}" ]; then
    LOG=$TEST_ON_PI_FAKE_LOG
  else
    ./build.sh kernel >/dev/null || { fail "build failed"; return 1; }
    local out rel try
    for try in 1 2; do
      # FLASH_SECS_AFTER_GO=1: secs is the kernel's own run time, counted from the "go" (the upload takes ~8 s);
      # FLASH_GO_WAIT: an upload not done in 60 s has stalled.
      out=$(FLASH_SECS_AFTER_GO=1 FLASH_GO_WAIT=60 CIRCLE_TREE=circle-engine "$REPO/pi/circle/flash.sh" ../engine "$secs" 2>&1)
      # flashy's monitor writes through tee in a process substitution, so its lines can come after "log: ...", and
      # the kernel ends its lines LF CR, so a CR (or the start of a kernel line) can precede "log: " on its line:
      # take the "log: out/....log" text wherever it is, not the last line.
      rel=$(printf '%s\n' "$out" | grep -Eo 'log: out/[A-Za-z0-9._-]+\.log' | tail -1); rel=${rel#log: }
      if [ -z "$rel" ] || [ ! -f "$REPO/pi/circle/$rel" ]; then
        fail "flash.sh printed no usable 'log: ' line"; printf '%s\n' "$out" | tail -5; PI_DEAD=1; return 1
      fi
      LOG="$REPO/pi/circle/$rel"
      echo "      flash: $(grep -Eo 'Finished in [0-9.]+ seconds' "$LOG" | tail -1 || true); $(printf '%s\n' "$out" | grep -Eo 'upload: (go after [0-9]+ s|timed out after [0-9]+ s|flashy exited before the go)' | tail -1 || true); log $LOG"
      # Measured: now and then a flash into the waiting serial bootloader (after a reboot: 4 of about 12 such
      # flashes, none into a running kernel) stops after ~30 of its ~400 progress dots and never finishes; the Pi
      # stays in its bootloader, and the next flash goes through.
      grep -q 'Finished in' "$LOG" && break
      [ "$try" -eq 1 ] && echo "      flash: the upload stalled - once more"
    done
  fi
  if [ ! -s "$LOG" ] || ! grep -q '==== rytm-pi engine' "$LOG"; then
    fail "no '==== rytm-pi engine' banner after the flash ($LOG) - the Pi is not answering"
    PI_DEAD=1
    return 1
  fi
}

# nofault: no exception, abort, panic, failed assertion or allocation in the log. Circle starts a LogPanic line
# with ESC[1m (ESC[91m with USE_LOG_COLORS), no other severity uses either.
nofault() {
  if grep -Eiq '\b(exception|abort|panic|assertion failed|out of memory)\b' "$LOG" ||
     LC_ALL=C grep -Eq $'\x1b\\[(1|91)m' "$LOG"; then fail "a fault in the log ($LOG)"; else pass "no fault"; fi
}

# pirun ARGS: tests/pi_run.py from the repo root (the port is free once flash.sh's monitor has ended).
pirun() { (cd "$REPO" && uv run python pi/engine/tests/pi_run.py "$@"); }

# expect REGEX WHAT: the log has a line matching REGEX (grep -E).
expect() {
  if grep -Eq "$1" "$LOG"; then pass "$2"; else fail "$2 (no line matching /$1/ in $LOG)"; fi
}

# ---- Task 3: the core on the Pi (FatFs port) and the boot self-check
# Pass: the banner; the card mounted (FAT32) and fingerprinted, or "card: no /Samples" (no /Samples folder yet:
# Task 6 adds it); the JUNO and DX7 libraries looked for (with a card only: engine_begin skips them without); engine_begin reported; the PLAITS trig; one second of
# PLAITS VA rendered offline with a peak over 1000 and under 1 % of real time (spike3: about 1 % at 600 MHz; here
# 1.5 GHz). First boot also logs "index: card changed or no index - building" / "index built: ...".
step_task3_selfcheck() {
  flash 25 || return
  expect '==== rytm-pi engine' "banner"
  expect 'card: FAT32, mounted in [0-9]+ ms|card: no /Samples' "card mounted"
  expect 'card: listing hash [0-9a-f]+|card: no /Samples' "card fingerprint"
  if grep -q 'card: no /Samples' "$LOG"; then   # no card for the core: engine_begin skips the JUNO and DX7 libraries
    pass "JUNO and DX7 libraries skipped (card: no /Samples)"
  else
    expect 'juno: (no /JUNO on the card|[0-9]+)' "JUNO library"
    expect 'dx7: (no /DX7 on the card|[0-9]+|index)' "DX7 library"
  fi
  expect '(core: engine_begin|engine: begin) (ok|WITHOUT a card) in [0-9]+ ms' "engine_begin"
  expect 'trig voice 0 note 48 engine 8' "PLAITS trig"
  local line
  line=$(grep -Eo 'core: 1 s of PLAITS VA rendered in [0-9]+ us \([0-9]+\.[0-9] % of real time\), peak [0-9]+' "$LOG" | tail -1)
  if [ -z "$line" ]; then
    fail "no self-check render line in $LOG"
  else
    local pct peak
    pct=$(echo "$line" | sed -E 's/.*\(([0-9]+)\.([0-9]) %.*/\1\2/')    # tenths of a percent
    peak=$(echo "$line" | sed -E 's/.*peak ([0-9]+).*/\1/')
    if [ "$pct" -lt 10 ] && [ "$peak" -gt 1000 ]; then pass "$line"; else fail "$line (want < 1.0 % and peak > 1000)"; fi
  fi
  nofault
  echo "      log: $LOG (record the % and peak in the Task 3 commit / docs/pi-engine.md)"
}

# ---- Task 4: the engine kernel - cores 1-3, the console, the stats; without a Rytm (nothing on its USB port)
# Pass: the banner; engine begin; Task 3's self-check line still there; cores 1, 2 and 3 up; waiting for the Rytm;
# a stats line a second with the fifo at its target (min = max = 96: filled once, nothing pulls; core 0's loop
# wakes core 3 every pass, so it counts the level each second) and no blocks rendered; the to-host
# counter present; no fault and the capture buffer allocated. The counter is not required to be > 0 here: the
# plan's "boot's 14/16/17" (pool table 0x14, JUNO 0x16 and FM 0x17 names) are sent by core/pool/pool.cpp's
# set_project (the Rytm's 0x10 project) and the 0x13 request (host_link.cpp) - both from a Rytm; engine_begin's
# project_load sends nothing. With the Rytm it must count up (DEFERRED (Rytm) below).
step_task4_boot() {
  flash 30 || return
  expect '==== rytm-pi engine ==== CPU [0-9]+ MHz' "banner"
  expect 'engine: begin (ok|WITHOUT a card) in [0-9]+ ms' "engine begin"
  expect 'core: 1 s of PLAITS VA rendered in [0-9]+ us' "boot self-check"
  expect 'core 1 up: renders voices 0-3' "core 1 up"
  expect 'core 2 up: renders voices 4-7' "core 2 up"
  expect 'core 3 up: control \(fifo target 96 frames, MIDI notes on\)' "core 3 up"
  expect 'waiting for the Rytm' "core 0 loop"
  expect 'engine: fifo min 96 max 96 target 96 under 0 \| blocks 0 max [0-9]+ us \| cpu c1 [0-9.]+ c2 [0-9.]+ c3 [0-9.]+ %' "stats line (no stream: fifo full, no blocks)"
  expect 'to-host dropped [0-9]+ \| peak [0-9]+/1000 \| [0-9]+ C' "stats line tail"
  if [ "$(grep -c 'engine: fifo min' "$LOG")" -ge 20 ]; then pass "a stats line a second"; else fail "fewer than 20 stats lines in 30 s"; fi
  if grep -q 'capture: cannot allocate' "$LOG"; then fail "capture buffer not allocated"; else pass "capture buffer"; fi
  nofault
  echo "      log: $LOG"
}

# Task 4's Rytm-less self-test (authorised addition: selftest.h): flash, then tests/pi_run.py --selftest engines
# drives the console - :selftest=on, each machine on voice 0 with synthetic MIDI notes through midi_in -> notes ->
# the core, a 10 s 'w' capture each (capture.py), :selftest=off. Pass (pi_run.py, exit 0): per machine the voice
# set, its trigs, engine blocks 3900..4100/s, self-test OUT 6700..7000 blocks/s, 1..3 notes/s, no underflow in
# steady state, the capture's OUT MAIN L peak in (-45, -11.9] dBFS and (except md and daisy) the pitch within 3 % of
# the machine's own (pi_run.py EXPECT: plaits 130.81 Hz, juno and fm 65.41 Hz) (daisy, inharmonic MODAL: its strongest mode within 3 % of the core's 626 Hz); no fault in the lines read. plaits juno fm daisy md here (dsps needs Task 6's pool
# sample: step task6-engines). About 5 x 45 s. Then the flash log has no fault.
step_task4_selftest() {
  flash 20 || return
  if pirun --selftest engines --machines plaits,juno,fm,daisy,md; then
    pass "pi_run.py --selftest engines: every machine played, captured, level and pitch ok"
  else
    fail "pi_run.py --selftest engines (its FAIL lines above; captures in pi/circle/out/engine-*.wav)"
  fi
  nofault
}

# DEFERRED (Rytm) - Task 4, Step 6: the stock Rytm in OVERBRIDGE mode on a Pi USB port, flash.sh ../engine 40.
#   Pass: "engine: begin ..."; "alt 3 selected: 8 URBs x 1 blocks ..."; each second the rytmob line with
#   "IN 6857/s", "OUT 6857/s", "gap 0 lost 0", "ep 1/1"; the engine line with "under" flat after the first
#   second, "fifo min" > 0, "blocks" about 4000, cpu c1/c2 below 1 %, "to-host dropped" counting the boot's
#   14/16/17; "midi: umidi1 found, listening (read only)".
# DEFERRED (Rytm) - Task 4, Step 6: then (flash.sh's monitor stopped) ':notemach=0:plaits' and ':drone=0' on the
#   console: "notes: voice 0 plays plaits", "drone 0", "trig voice 0 note 48 engine 8" twice a second, engine
#   lines with plaits about 0.1-0.3 %, peak above 0/1000 and at most 250/1000 (the -12 dBFS limit), "under" not
#   growing; ':drone=off'. Also: the Rytm's own MIDI notes (track 1, note 36, channel 1) counted as "notes N" and
#   marked in a 'w' capture; ':selftest=on' refused while the Rytm is connected ("selftest: refused - the Rytm is
#   connected"); a Rytm plugged in during the self-test switches it off ("selftest: off (the Rytm's USB MIDI is
#   up)" or "(the Rytm streams)"); the reboot magic logs "stream stopped
#   cleanly: alt 0" before the reboot.

# ---- Task 5: the voice guard, the watchdog and the clean stop. tests/guard.py drives the console over the serial
# port; the self-test transport (':selftest=on') paces the blocks in place of the Rytm's IN stream.
guard() { (cd "$REPO" && uv run --with pyserial python pi/engine/tests/guard.py "$@"); }

# After every flash the boot logs the guard's budget: 50 % of a 12-frame block at 54 MHz = 6750 ticks = 125 us.
budget() { expect 'guard: voice budget 125 us \(50 % of a 12-frame block\)' "voice budget (voicebudget= default)"; }

# Budget mute, fault mute and recovery, :unmute (guard.py mute; its PASS/FAIL lines name each check): ':drone=all';
# ':overtest=6' -> "voice 6 !PLAITS muted: over budget on an overloaded core (longest render 3xx us, budget 125 us)", "engine: muted
# !PLAITS(6)"; ':faulttest=7' -> "core 2 faulted at PC 0x... (fault 1 on this core), resumed at task level
# (EL1t)", "voice 7 !PLAITS muted: crashed", "engine: muted !PLAITS(6) !PLAITS(7)"; throughout a stats line a second,
# "under" flat, cpu c2 > 0 after the fault (voices 4-5), peak > 0; ':unmute', then ':faulttest=5' and ':faulttest=7':
# faults 2 and 3 on core 2 also "resumed at task level (EL1t)" (the exception return left the core sound), "engine:
# muted !PLAITS(5) !PLAITS(7)"; ':unmute' -> "guard: every voice unmuted", the "engine: muted" line gone. No
# exception, no reboot.
step_task5_guard() {
  flash 20 || return
  budget
  nofault
  if guard mute; then pass "guard.py mute: budget and fault mutes, recovery, unmute"; else fail "guard.py mute (its FAIL lines above)"; fi
}

# A render core that misses the 20 ms block limit is fatal (guard.py fatal): ':hangtest=1' (voice 1 renders 30 ms
# once) -> "reboot: a render core did not finish its block in 20 ms - stopping the selftest stream first", then
# quiet (the Pi rebooted into its serial bootloader). Then a flash must boot the engine again (banner, budget).
step_task5_fatal() {
  flash 20 || return
  if guard fatal; then pass "guard.py fatal: clean stop, then reboot"; else fail "guard.py fatal (its FAIL lines above)"; fi
  flash 15 || return
  expect '==== rytm-pi engine' "the Pi came back after the fatal reboot"
  budget
}

# The BCM watchdog (4 s) catches a hang of core 0 (guard.py watchdog): ':wdtest' -> "watchdog test: core 0 hangs",
# then no stats line. 8 s later a flash must boot the engine: without the watchdog the hung kernel would ignore the
# reboot magic and this flash would fail ("no log output") - the Pi then needs a power cycle. Run it last.
step_task5_watchdog() {
  flash 20 || return
  if guard watchdog; then pass "guard.py watchdog: core 0 hung"; else fail "guard.py watchdog (its FAIL lines above)"; fi
  flash 15 || return
  expect '==== rytm-pi engine' "the watchdog rebooted the Pi (the flash after the hang booted)"
  nofault
}

# The clean stop on the flasher's magic, three rounds (guard.py reboot sends RYTM-REBOOT itself, so the old kernel's
# last lines are read: flashy does not echo them): "reboot: the flasher's magic - stopping the selftest stream
# first", quiet, and the next flash boots (the plan's five reboots mid-stream with the Rytm: DEFERRED (Rytm) below).
step_task5_reboots() {
  local i
  for i in 1 2 3; do
    flash 15 || return
    [ "$i" -eq 1 ] || expect '==== rytm-pi engine' "round $i: booted after the clean stop"
    if guard reboot; then pass "round $i: clean stop on the magic"; else fail "round $i: guard.py reboot"; fi
  done
  flash 15 || return
  expect '==== rytm-pi engine' "booted after the third clean stop"
  nofault
}

# DEFERRED (Rytm) - Task 5, Step 4: the same guard tests on the Rytm's stream (no self-test): the rytmob lines keep
#   "gap 0 lost 0" throughout the overrun and the fault, the engine lines "under" flat.
# DEFERRED (Rytm) - Task 5, Step 5: five reboots mid-stream through flash.sh's magic; each round the previous kernel's
#   "stream stopped cleanly: alt 0 (URBs still pending in 0 out 0)" (read on the serial port, as guard.py reboot
#   does: flashy does not echo it), the new kernel's "alt 3 selected ...", and a rytmob line with "OUT 6857/s",
#   OUT not stuck at 16 blocks. If a round shows OUT stuck at 16 blocks, stop: record the log in docs/pi-engine.md
#   and power-cycle the Rytm (the user's hand); do not iterate on the Rytm's state.
# DEFERRED (Rytm) - a fatal (':hangtest=1') with the Rytm streaming: "stream stopped cleanly: alt 0" after the
#   "reboot: a render core did not finish ..." line, and the Rytm streams to the next kernel.

# ---- Task 6: card content on the Pi's SD and the automated engine tests. The Pi's card is written here (the plan's
# only card writes: the self-test sample and the _default pool; nothing that exists is touched).
# First boot with this kernel: "selftest: wrote /Samples/pi-test/sine440.wav" and, without /.rytm/project, "selftest:
# pool _default slot 1 = pi-test/sine440.wav"; the index rebuilt for the new file ("index: card changed or no index
# - building", "index built: ..."). The second flash: no "selftest: wrote" (the files exist), "index: unchanged,
# loaded", and the pool loaded with the sample found ("pool /.rytm/pools/_default.txt: 1 slots, 1 samples, 0 not on
# the card", or the project's own pool when /.rytm/project names one: then at least 1 sample). Engine begin "pool N"
# with N >= 1.
step_task6_content() {
  flash 25 || return
  if grep -q 'selftest: wrote /Samples/pi-test/sine440.wav' "$LOG"; then
    pass "self-test sample written (first boot with it)"
    expect 'index: card changed or no index - building' "index rebuilt for the new file"
  else
    pass "self-test sample already on the card"
  fi
  expect 'engine: begin ok in [0-9]+ ms \([0-9]+ folders, project "[^"]*", pool [1-9][0-9]*\)' "engine begin with a pool"
  nofault
  flash 20 || return
  if grep -q 'selftest: wrote' "$LOG"; then fail "the second boot wrote the content again"; else pass "nothing rewritten"; fi
  expect 'index: unchanged, loaded' "index loaded, not rebuilt"
  expect 'pool /\.rytm/pools/[^:]+: [0-9]+ slots, [1-9][0-9]* samples' "pool loaded, its sample found"
  nofault
}

# Every engine on notes without a Rytm (the plan's Step 3, on the self-test transport): pi_run.py --selftest engines
# with all six machines, DSP SAMPLE on the pool's sine440.wav (note 48 = 220 Hz; Mac: make -C tests content measured
# 220.0 Hz on the core). Pass: ALL PASS, 6 x (voice set, trigs, 4 stats checks, level) + 4 pitches (not md, daisy: its strongest mode) and the
# self-test on/off and fault checks. About 6 x 45 s.
step_task6_engines() {
  flash 20 || return
  if pirun --selftest engines; then pass "pi_run.py --selftest engines: all six machines"; else fail "pi_run.py --selftest engines (FAIL lines above)"; fi
  nofault
}

# Ten minutes, all 8 voices droning (every machine) on the self-test transport (the plan's Step 4 without the
# Rytm): a progress line every 10 s (OUT ~6857/s, under constant, cpu c1/c2/c3); then "PASS: the self-test pulls 6857
# blocks/s throughout", "PASS: no output underflows", "PASS: no muted voice", "PASS: no fault in the log", ALL PASS.
# Record the cpu percentages (Task 6 commit / docs/pi-engine.md). About 11 minutes.
step_task6_gaps() {
  flash 20 || return
  if pirun --selftest gaps 600; then pass "pi_run.py --selftest gaps 600"; else fail "pi_run.py --selftest gaps 600 (FAIL lines above)"; fi
  nofault
}

# DEFERRED (Rytm) - Task 6, Step 3: the Rytm in OVERBRIDGE mode on a Pi USB port: `pi_run.py --flash engines`.
#   Pass: twelve checks plus voice/trig/stats per machine (a level and a pitch each, MD - DIGIDRUM's place since 2026-10-29 - level only), the
#   stats' rytmob OUT 6700..7000/s, ALL PASS. A pitch fail: play the WAV in pi/circle/out/ against the same machine
#   on the Teensy (sim/core_golden.py's defaults); a wrong octave beyond +-1, or noise, is a bug.
#   Run 2026-10-15 on the Rytm's own notes instead of the drone: `pi_run.py engines --rytm-notes 0` (its track on
#   ch 1 plays note 36), ALL PASS after the pitch search's fix (docs/pi-engine.md "Long run and engines").
# DEFERRED (Rytm) - Task 6, Step 4: `pi_run.py gaps 600` with the Rytm (run 2026-10-15: ALL PASS): "PASS: no Overbridge gaps" (gap/lost
#   constant after the warm-up), "PASS: no output underflows", "PASS: no muted voice", ALL PASS; record cpu c1/c2/c3.

# ---- Task 7: performance. The boot bench (kernel.cpp LogBench): every machine on one voice for 1 s of audio on
# cores 0, 1, 2 and 3 in turn, before the render loops start. Pass: 6 x 4 "bench cN MACHINE: x.yy % of a core per
# voice, worst block z.z % -> N voices a core" lines and 6 "bench MACHINE: ... at 1500 MHz -> N voices a core (worst
# of cores 0-3, ...)" summaries; pi_run.py bench: each machine at most 20 % of a core per voice (four voices fit a
# core: the even split holds), its worst block under a whole block (a NOTE if over the guard's 25 %); PLAITS VA near
# spike3's 0.95 % (more than 50 voices a core). Then the boot goes on as before (cores up, waiting for the Rytm).
# Copy the table pi_run.py prints into docs/pi-engine.md's Measurements (the "pending" cells).
step_task7_bench() {
  flash 25 || return
  expect 'bench c3 md: [0-9]+\.[0-9]+ % of a core per voice' "the bench ran on every core (core 3 last)"
  expect 'core 1 up: renders voices 0-3' "the render loops started after the bench"
  if pirun bench "$LOG"; then pass "pi_run.py bench: every machine fits four voices a core"; else fail "pi_run.py bench (FAIL lines above)"; fi
  nofault
}

# The FIFO target sweep without the Rytm (the plan's Step 3 sweep on the self-test transport): for each target
# (FIFO_TARGETS, default "48 72 96 192"), pi_run.py --selftest gaps 120 --fifo N (all 8 voices droning; about
# 150 s each). Pass: the current default (96) holds - no underflow after the warm-up, OUT 6857/s throughout, no
# mute. Prints each target's result and the smallest all-passing target plus one step of margin: only a candidate,
# since the self-test's pacer (core 0's loop) is not the Rytm's IN stream - the default in kernel.cpp
# (GetAppOptionDecimal ("fifo", 96)) changes only after the Rytm sweep (DEFERRED (Rytm) below). Record the results
# in docs/pi-engine.md.
step_task7_fifo() {
  flash 20 || return
  local n ok=() bad=() pick="" prev_ok=1 margin=""
  for n in ${FIFO_TARGETS:-48 72 96 192}; do
    echo "---- fifo $n"
    if pirun --selftest gaps 120 --fifo "$n"; then ok+=("$n"); else bad+=("$n"); fi
  done
  echo "      fifo targets passing: ${ok[*]:-none}; failing: ${bad[*]:-none}"
  # the smallest target from which every larger one passed, then the next one up as the margin
  for n in ${FIFO_TARGETS:-48 72 96 192}; do
    if [[ " ${ok[*]:-} " == *" $n "* ]]; then
      if [ -z "$pick" ]; then pick=$n; elif [ -z "$margin" ]; then margin=$n; fi
    else
      pick=""; margin=""
    fi
  done
  echo "      candidate default (self-test pacing): ${margin:-${pick:-none}} (smallest passing ${pick:-none}, plus one step)"
  if [[ " ${ok[*]:-} " == *" 96 "* ]]; then pass "fifo 96 (the default) holds under a full load"; else fail "fifo 96 (the default) failed"; fi
  nofault
}

# DEFERRED (Rytm) - Task 7, Step 3: the FIFO sweep on the Rytm's IN stream (the decision): for n in 48 72 96 192,
#   `uv run python pi/engine/tests/pi_run.py gaps 120 --fifo $n` (the plan's helper sends :fifo=N the same way).
#   Pass per target: "PASS: no Overbridge gaps", "PASS: no output underflows", "PASS: no muted voice". Take the
#   smallest all-PASS target plus one step of margin as the default; change GetAppOptionDecimal ("fifo", 96) in
#   kernel.cpp if it differs (latency added: target / 48 ms on top of spike3's 2.9 ms) and record it in
#   docs/pi-engine.md. Then `pi_run.py gaps 600` at the new default: 0 gaps, 0 underflows, cpu c1/c2/c3 recorded.
# DEFERRED (Rytm) - Task 7: the bench's numbers with the Rytm streaming are the same kernel's (the bench runs at boot,
#   before USB): nothing more to run; the stats' cpu c1/c2 under the gaps run confirm the split.

ALL="task3-selfcheck task4-boot task4-selftest task5-guard task5-fatal task5-reboots task6-content task6-engines task6-gaps task7-bench task7-fifo task5-watchdog"
DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; shift; fi
[ "${1:-}" = "all" ] && [ $# -eq 1 ] && set --
STEPS=${*:-$ALL}
for STEP in $STEPS; do   # every name checked before anything runs
  declare -F "step_${STEP//-/_}" >/dev/null || { echo "unknown step $STEP (have: $ALL)"; exit 2; }
done
if [ "$DRY" -eq 1 ]; then
  i=0
  for STEP in $STEPS; do i=$((i + 1)); echo "$i. $STEP"; done
  echo "test-on-pi: dry run, nothing flashed (the Pi on $(ls /dev/cu.usbserial-* 2>/dev/null | head -1 || true))"
  exit 0
fi
SUMMARY=()
for STEP in $STEPS; do
  if [ "$PI_DEAD" -eq 1 ]; then SUMMARY+=("SKIP  $STEP (the Pi is not answering)"); continue; fi
  echo "==== $STEP"
  before=$FAILS
  "step_${STEP//-/_}"
  if [ "$FAILS" -eq "$before" ]; then SUMMARY+=("PASS  $STEP"); else SUMMARY+=("FAIL  $STEP ($((FAILS - before)) check(s))"); fi
  if [ "$PI_DEAD" -eq 1 ]; then
    echo "==== Pi not answering: power-cycle it (the serial bootloader then waits for the upload), then run the"
    echo "     remaining steps: $0 <step>..."
  fi
done
echo "==== summary"
printf '%s\n' "${SUMMARY[@]}"
echo "test-on-pi: $FAILS failure(s)"
[ "$FAILS" -eq 0 ]
