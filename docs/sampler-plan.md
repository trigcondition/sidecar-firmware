# Plan: a sample-streaming machine on the Teensy's SD card (2026-10-08)

Not started. A plan to execute later; open questions are at the end.

## Idea

A third external machine next to PLAITS and DAISYSP: it plays samples that live
on the Teensy's SD card, streamed from disk as the M8 does, so sample storage and
sample length are bounded by the card (gigabytes, minutes per sample) instead of
the Rytm's sample memory. It is chosen per track like any machine; the sequencer,
p-locks, the LFO and the voice's analog filter, overdrive and amp all apply, as
they do for PLAITS. The Rytm browses the card's folders and files through its own
knobs: the Teensy sends the names, the Rytm shows them.

## What already exists to build on

- The report path (rytm-mods 0021): every trig with P1..P16 in 14 bits, and
  parameter changes, Rytm -> Teensy over USB MIDI SysEx. 14 bits gives a sample
  start or loop point a 1/16384 resolution of the sample's length.
- The return path (rytm-mods 0022): SysEx from the Teensy is already received and
  acted on by the Rytm (looper slot states 07, mute handover 08), from the USB
  MIDI router 0x40083d6c.
- Value text from a table (0021's ext_engine_text): a knob shows a name per value
  while turning. The same trick shows file names, with the table filled from the
  Teensy instead of fixed in the firmware.
- The SMPL page as page 2 (PLAITS): 8 more parameters in the sample words.
- Spare parameter records: the TEST machines 45, 47-50 and 52 (29 records) are
  still free; a sampler with two pages needs 16. Free machine slots: the same.
- Overbridge per-voice inputs: the sampler's audio goes back on the voice's own
  channel, through its analog chain, like PLAITS.
- The looper (0022) already records the Rytm's output on the Teensy; its clips
  could be written to the SD card and become samples (resampling).

## The parts

### 1. Streaming on the Teensy (the core risk)

- SD: the Teensy 4.1's built-in 4-bit SDIO slot with SdFat (in Teensyduino;
  FAT32 and exFAT). Eight voices of 16-bit stereo at 48 kHz are 1.5 MB/s; the
  slot should manage several times that, but random seeks across eight files
  are what matters - measure first.
- Trig latency: an SD read can stall for milliseconds, so a trig cannot wait for
  the card. Keep the head of every sample in the current kit/folder in RAM
  (preload, e.g. the first 100-200 ms) and start from there while the stream
  catches up, as hard-disk samplers do. RAM2 (about 330 KB free next to the
  voices) holds short heads only. PSRAM (two 8 MB chips soldered on the Teensy
  4.1's pads, 16 MB, about 12 euro) changes the picture: 16 MB is about 170 s
  of 16-bit mono at 48 kHz, so a folder's heads (128 x 200 ms = 2.4 MB) fit
  easily, and short one-shots (drums of 0.5 s, about 48 KB) can load whole and
  never stream - only long samples then stream from the card. The looper's clips
  (a 4-bar stereo clip at 120 BPM is about 1.5 MB) want it too. Plan: the code
  works without PSRAM (heads in RAM2, smaller folders), better with it.
  Ready-made boards exist with PSRAM already soldered and tested, up to 32 MB
  (e.g. ProtoSupplies' "Teensy 4.1 fully loaded", which also fits the USB host
  header on request): no soldering for people building this. 32 MB is about
  350 s of 16-bit mono at 48 kHz. To check on such a board: that Teensyduino
  reports all 32 MB (`external_psram_size`).
- Per voice: a ring buffer (e.g. 32 KB) refilled from the card by the main loop
  (not the audio path), read with interpolation for pitch.
- Files: WAV 16/24-bit, mono/stereo, any rate (resampled on play). The voice
  input on the Rytm is mono: stereo files are summed to mono per voice, or (an
  option) played to MAIN L/R directly, bypassing the analog chain.

### 2. The engine (the fun part)

Playback modes on ENG (model zones, as PLAITS/DAISYSP):

- ONESHOT, LOOP (forward / ping-pong), REVERSE
- SLICE: the file cut into N equal slices (or at transients); a knob picks the
  slice, so p-locks play a different slice per step
- GRAIN: granular position / size / density / spray, the position movable by
  the LFO for scanning a long recording
- STRETCH: time-stretch at a fixed pitch (a grain-based stretch is cheap enough)

Per-sample parameters (14-bit): start, length, loop start/end, pitch (semitones +
fine), attack/decay of an amp envelope, level. Effects on page 2 from DaisySP:
decimator / sample-rate reducer, wavefolder, overdrive, SVF, comb. Folders as
groups: a folder of files can be a round-robin or velocity-layer set.

### 3. Browsing on the Rytm

- MVP, low risk: two knobs, FLD (folder index) and SMP (file index in the
  folder), 0..127 each (16384 files). Turning either shows the name in the knob's
  value text, as ENG shows model names. The firmware keeps a name cache (e.g. the
  128 names of the current folder plus the folder list, 16 characters each,
  about 4 KB in a RAM cave) that the Teensy fills by SysEx when FLD changes.
- Messages (new, next to sim/protocol.py's 03..08):
  - Rytm -> Teensy `09 <track> <folder>`: show me this folder
  - Teensy -> Rytm `0A <folder> <first> <n> <names...>`: names, in chunks
  - Teensy -> Rytm `0B <folder> <count>` and folder names
  - Teensy -> Rytm `0C <track> <length> <channels> <rate>`: the chosen file's
    info, for the screen (and to scale start/length)
- Later, a real list browser like the machine list (a pop-up of names, scrolled
  with the arrows). That means finding and reusing the Rytm's list widget: more
  firmware work, better to do after the MVP works.
- Elektron Transfer browses the +Drive over Elektron's (undocumented) SysEx
  protocol, and rytm-emu runs the code that answers it. Reusing the Rytm's own
  sample browser for the Teensy's card would be the nicest UI, but needs that
  protocol and UI reverse-engineered; a research item, not the first step.

### 4. Remembering which sample a sound plays

Folder/file indices break when the card changes. Instead the sound stores a
14-bit sample ID, and the Teensy keeps a pool file per Rytm project on the card
(e.g. `/RYTM/<project>/pool.txt`: ID -> path), as the Rytm's own sample slots
map to +Drive files. Choosing a file in the browser adds it to the pool and
writes its ID into the sound. Project identity: the Rytm's project name over
SysEx, or a pool chosen on the Teensy.

### 5. On the Rytm side (rytm-mods)

- A machine slot (one of the free TEST machines), named after the engine, 16
  records from the free TEST records: page 1 on SRC (e.g. ENG FLD SMP STA LEN
  PIT DEC LEV), page 2 on SMPL (loop points, grain/slice controls, FX).
- The report flag in the voice byte (DAISYSP has 0x10; this one 0x20).
- The name cache and its SysEx handler (next to 0022's), and the value-text
  functions for FLD/SMP.
- Caves: about 1-2 KB of code and 4 KB of RAM; the free caves today are cave20's
  rest and parts of caves 15, 19, 21, 24, which is tight - finding more cave
  space (or a RAM area for the name cache) is part of the work.

### 6. Capabilities: basic without PSRAM, more with it

One build for every Teensy; it looks at what it has and adapts:

- At boot the Teensy reads its PSRAM size (Teensyduino's
  `external_psram_size`, in MB: 0, 8, 16 or 32; memory from `extmem_malloc`) and
  whether an SD card is present and readable.
- It tells the Rytm in a capability report, at connect and when asked:
  Teensy -> Rytm `0D <version> <psram MB> <sd 0/1> <flags>`, the flags naming
  the features it offers. The Rytm can show it (e.g. in the page title:
  "SAMPLER 16MB" / "SAMPLER BASIC" / "NO CARD") so a user sees what their
  hardware does.
- Tiers:
  - **No card:** PLAITS and DAISYSP as now; the sampler machine says NO CARD.
  - **Basic (no PSRAM):** the sampler streams from the card with short heads in
    RAM2 (fewer files per folder preloaded, e.g. 32 x 100 ms); ONESHOT, LOOP,
    REVERSE, SLICE; looper clips on the card instead of in memory.
  - **Extended (PSRAM):** whole short samples in memory, long heads for a full
    folder, GRAIN and STRETCH (they jump around a file, which streaming makes
    slow), looper clips in memory, more voices of the sampler at once.
- A mode the hardware cannot do (GRAIN on a basic build) falls back to its
  nearest basic mode and the knob's name text says so, rather than failing
  silently.

## Measured on the Teensy (2026-10-08, teensy/sd_bench)

Teensy 4.1 without PSRAM, a 32 GB card holding /Samples (311 folders with .wav
files, 15184 files), SD library (SdFat 2.1.2, Teensyduino 1.62), read-only:

| | result |
|---|---|
| card begin | 0.15 s |
| scan of /Samples | 20.9 s (311 folders, 15184 .wav) |
| sequential read | 22.9 MB/s |
| 8 streams, 4 KB reads round robin, 5 s | 5.26 MB/s (3.4 x 8 voices of 16-bit stereo); avg 0.78 ms a read, worst 3.96 ms, none over 5 ms |
| open + 44-byte header, random file | avg 3.0 ms, worst 4.8 ms |

What follows:
- Streaming is not the problem: a few KB of ring buffer per voice covers the
  worst read many times over.
- Opening a file is: 3..5 ms would make a trig on a sample that is not open yet
  audibly late. So each sound's selected sample stays open with its head in RAM
  (prepared when FLD/SMP change, and from the 03 parameter reports), and a
  p-locked sample not prepared yet starts up to ~5 ms late in the basic build;
  with PSRAM the whole folder's heads (or short samples whole) are kept.
- A full scan takes 21 s: too slow at every boot. Read one folder's listing on
  demand (when the Rytm asks for it), and later keep an index file on the card
  (that writes to the card - only with the user's say-so).
- 311 folders do not fit FLD's 0..127, and folders can hold more than 128 files:
  browsing needs more than two plain knobs (a bank, a 14-bit value, or walking
  the folder tree level by level).

## The card index (2026-10-08, the user's idea; measured with teensy/sd_walk*)

At boot the Teensy decides whether its index of the card is still good, and
rebuilds it only when the card changed:

- **Change check: a hash of the listing, not of the audio.** Hashing the files'
  contents (MD5) would read all 10.1 GB: about 441 s at 22.9 MB/s, every boot.
  Hashing every entry's name, size, modify time and first cluster catches any
  added, removed, renamed or rewritten file. Walked through SdFat's file API
  (opening each entry) it took 19.7 s; reading the folders' raw 32-byte FAT32
  entries (only the 449 folders opened) **0.72 s**, the same counts (449 folders,
  39548 entries, 39100 files, 10.11 GB) and the same hash run after run.
- **The index**: per folder its subfolders and .wav files (macOS "._" files
  left out: ~7000 of the 22185 .wav names are those), and per file its size,
  format (from the header), first cluster/sector - so a trig opens a sample
  without walking its path - and an ID.
- **Stored on the card** in a hidden folder (e.g. `/Samples/.rytm/index.bin`
  and `/Samples/.sidecar/env` holding the listing hash and the index version).
  Writing to the card is the user's choice; it is the only write.
- **At boot**: raw walk (< 1 s) -> hash; same as `env` -> load the index; else
  rebuild it (one full scan with headers, tens of seconds, once) and write it.
- **Per-file content checksums** only where they pay: a sample put in a
  project's pool can be recognised after it moved (computed when it is added,
  or in the background).

### Built (2026-10-08, teensy/voiceloop/card_index.*)

On the user's card (FAT32, 32 GB, 10.1 GB in /Samples): first boot builds the
index in 120 s (449 folders, 15580 usable .wav, 15 skipped - formats other than
PCM 16/24/32-bit), writing /.sidecar/folders.bin, files.bin and env; every later
boot: listing hash 0.69 s, "unchanged, loaded". Browsing from the index over
serial (the 09/0B/0C/0A messages, via the bridge's new 'X' relay to the Rytm):
each answer within a second; subfolders and files in name order (worked out per
folder when asked; the index keeps the directory order). Each file's record
holds its size, format, data offset/length and, when the file is contiguous, its
first sector - for opening without a path walk when playback comes.

### Streaming playback (2026-10-08, teensy/voiceloop/stream_voice.h)

DSP SAMPLE voices on the Teensy: when FLD/SMP/STA change (from the reports, or
YES/NO), the file is opened and 12 KB from STA read into RAM (11..14 ms, between
notes); a trig plays from that head at once; between render blocks the voices
take turns topping up an 8 KB ring, 2 KB a read. Measured over serial on the
user's card: a 24-bit kick and a long drone (4 s) through RESON, 0 underruns,
11.5 % CPU with one voice. (A first version refilled only once per audio request
- up to ~6 KB of a 24-bit stereo file a request - and ran dry.) Reverse playback
of long files and FB (needs the Rytm's audio) are not there yet.

### 8 voices at once (2026-10-08)

8 DSP SAMPLE voices streaming different long 24-bit stereo drones (2.3 MB/s),
the Teensy's own meter (send_frames' wall time against real time, card reads
included): 2 KB reads were too slow; with 8 KB reads (16 KB rings, 8 KB heads):
MIX 0 (resonator skipped) 60 % (28 % synthesis), 0 underruns; MIX 127 (8
resonators) 119 % - over. Converting the card to mono 16-bit 48 kHz cuts the
reads to a third; PSRAM (8 MB is plenty) would allow 32..64 KB reads, near the
card's 23 MB/s sequential speed, and short samples kept whole.

### Card conversion (2026-10-08, tools/card_convert.py)

On the Mac, as Transfer converts for the Rytm: mono, 16-bit, 48 kHz WAV (WAV,
AIFF, FLAC, OGG in; polyphase resampling, TPDF dither; --stereo keeps stereo).
`card_convert.py SRC DST` writes a converted copy (e.g. a library straight onto
the card's /Samples); `--in-place DIR` converts what does not match yet,
replacing the originals; both report only, without --yes. On the 1735-sample
library in rytm-emu/assets: 0.71 -> 0.42 GB in 2.9 s, every file mono/16/48k,
lengths and levels kept. After the card changes the Teensy rebuilds its index
on the next boot (the listing hash differs).

### exFAT (2026-10-08)

The user's new card (an M8-recommended one) is exFAT, which the FAT32-only index
refused. The index now runs on SdFat's FsFile (FAT32 and exFAT): FAT32 keeps the
raw-entry hash, exFAT hashes through the file API (fast there: 0.25 s for 75
folders / 1976 entries, against 19.7 s for that API on the old FAT32 card); files
and folders are reopened by their dir index (exFAT has no 8.3 names); index
version 2. On the new card: index built in 4.9 s (75 folders, 1737 .wav, 2.8 ms a
file against 7.7 ms on the old card), then loaded at boot in 0.25 s.

## Order of work

1. **Measure SD streaming on the real Teensy, without PSRAM** (a bench sketch like
   daisy_bench): sequential and random reads, 8 simultaneous streams, worst-case
   stall per read, with a few card types. This decides the preload size and
   whether PSRAM is needed.
2. **Capability report** (`0D`): PSRAM size and card presence, sent at
   connect; the simulator can pretend to be either tier.
3. **Sampler voice in the simulator and the Teensy sketch** (shared C++ like
   daisy_engine.h): WAV reading, streaming ring buffers, preloaded heads,
   ONESHOT/LOOP/REVERSE, pitch. The simulator reads a local folder as its "SD".
4. **Rytm machine, MVP**: slot, records, page 1, trig/param reports with the new
   flag; files chosen by plain index first (names later).
5. **Names**: the SysEx messages, the Rytm's name cache and value text; browsing
   with FLD/SMP in rytm-emu.
6. **Pool and IDs**: persistence across projects and card changes.
7. **Modes and FX**: SLICE, GRAIN, STRETCH, page 2, DaisySP effects.
8. **Resampling**: looper clips saved to the card and offered in the browser.
9. **On hardware** once the host cable is here (latency, stalls, real card
   speeds).

## Open questions

- The machine's name (it plays the Teensy's card: "SDPLAY", "STREAM", "TAPE"...).
- Mono into the voice's analog chain only, or also a stereo option to MAIN?
- How many files per folder the UI should handle (128 per page via FLD/SMP, or a
  BANK knob for more).
- PSRAM: optional but recommended (16 MB, about 12 euro); step 1 measures how much the build without it can do.
- Whether to chase the Rytm's own browser UI (Transfer's protocol) later.
