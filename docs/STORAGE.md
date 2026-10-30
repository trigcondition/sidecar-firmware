# STORAGE: USB drives, the Pi 5's NVMe SSD, and the storage bench

Sidecar | Expander (Pi 4B / Pi 5, Circle 51.1.1) boots from its SD card. Besides it, it now mounts **USB drives**
(`USB:`, `USB2:`, `USB3:`) as they are plugged in and, on the **Pi 5**, an **NVMe SSD** on the PCIe connector (`NVME:`).
`stor=bench` measures any of them. The project bundles (docs/BUNDLES.md "USB sticks") export to and import from
the USB drives through this layer: core paths that name a drive (`USB:/SIDECAR/...`), I/O in 64 KB pieces through an
aligned block (`pi/engine/platform_pi.cpp`), the bundle UIs' EJECT cancelling a job on the drive first. Background and options: the research report
`STORAGE-OPTIONS.md` (2026-10-21) and `docs/pi-engine.md` (the cores), `core/msamp/cardq.h` (the SD's card queue).

Tags as elsewhere: **[measured]** run here (Mac host tests, or on a Pi where it says so), **[inferred]** from Circle's
source or upstream, to be measured.

## Using a USB drive

- **Format: FAT32, with the MBR scheme ("Master Boot Record").** On a Mac: Disk Utility, View > Show All Devices, select
  the *device* (not the volume), Erase, Format **MS-DOS (FAT)**, Scheme **Master Boot Record**. Windows formats sticks
  MBR by default; pick FAT32 (Windows offers it only up to 32 GB in its own dialog: use a FAT32 tool for larger drives).
  - **GPT (Disk Utility's default "GUID Partition Map") does not mount** in this build, nor does **exFAT** (macOS's and
    Windows' default for large drives): this FatFs is built without `FF_LBA64` and `FF_FS_EXFAT` [measured: Mac host
    test against images macOS made, below]. The log and the web page say which it is ("GPT partition table: use the MBR
    scheme (FAT32)", "exFAT: FAT32 only in this build (STOR_EXFAT)").
  - FAT32 limits a file to 4 GB - 1 byte (fine for samples and bundles).
  - 512-byte logical sectors only (Circle's driver): a few 4Kn SSD bridges are refused.
- **Plug it in** - a blue USB 3 port on the Pi 4 (the Rytm sits on a black USB 2 port), any port on the Pi 5. Core 0
  mounts it within a second: the serial log says `stor: USB   mounted FAT32 57 GB free of 59 GB, USB 3 SuperSpeed (0781:5581) (mounted in 41 ms)`.
  The touchscreen's STATUS page (UP + T10) shows it in LINKS (`USB DRIVE`), the web page's Status tab under *Storage*.
- **Eject before pulling it:** the web page's *Eject*, or `:stor=eject:USB` on the console. It is then
  "ejected: safe to remove" until a drive is plugged in again. A drive pulled while mounted is unmounted at once and
  counted ("pulled while mounted"): a write in flight can be lost (FAT32 has no journal).
  - An ejected drive left plugged in: `:stor=rescan` (Pi 4) brings it back; on the Pi 5 unplug and plug it.
  - A drive that did not mount (wrong format), re-formatted elsewhere: plug it in again, or `:stor=mount:USB`.
- Up to three drives: `USB:`, `USB2:`, `USB3:` in the order Circle numbers them (`umsd1..3`).
- Some sticks need `usbpowerdelay=1500` in `cmdline.txt` to enumerate (upstream, discussions #668 / #582).

## Recording to a USB drive (2026-10-26)

The multitrack recorder (docs/pi-engine.md "Recording") writes to the SD card by default. A take needs up to ~4.2 MB/s
(25 channels at 24 bit); the SD card managed 5.0 MB/s with 25 files open and dropped blocks [measured on the Pi 4]. A
USB 3 stick or SSD is expected at 80-250 MB/s [inferred, not measured yet]. The destination can be a USB drive (or the
Pi 5's NVMe SSD):

- **Choose it:** the web page's Record tab, *Record to* (SD card / USB drive / ...: the mounted drives, and the chosen
  one marked "not mounted" while it is not); the console `:rec=vol:USB` (`SD`, `USB`, `USB2`, `USB3`, `NVME`). Refused
  while armed or recording. The destination is the Pi's: the web page shows what `/status` says and sends `rec=vol:`
  only when its picker is clicked (not when it opens, unlike the channels and the bit depth, so another browser, the
  console or a reboot is never undone by a page left open). Not kept across a reboot (none of the recorder's settings
  are): after one it is the SD. The touchscreen's REC page shows it with its free space (`USB 57 GB`; `USB not
  mounted` / `NO USB` while it is not; `USB ? free` for a FAT32 without FSINFO - `stor=free:USB` counts it); it has no
  picker.
- **A drive that is not mounted is refused** at start / arm (`USB drive not mounted`), and at the take's start when it
  was pulled while armed - never the SD instead.
- **Format:** FAT32 with the MBR scheme, as above (no exFAT, no GPT in this build). FAT32 limits a file to 4 GB: a take
  stops itself before its widest file gets there (stereo 24-bit: about 4 h 8 min), as on the SD.
- **Measure the drive first:** `:stor=bench:USB` (above) and compare its `+writes` row with the selection's rate (the
  Record tab shows it: `N files · X MB/s`). A take's own figures (the Record tab, the log's `stopped:` line and the
  10 s progress lines) say `drive N KB/s` (the speed inside the writes), the worst write, the ring's high-water mark and
  the dropped blocks: the same as on the SD.
- **Where:** `USB:/Recordings/<take>/<NN>_<NAME>.wav`. The take's name is numbered after the SD's takes and the drive's
  (one `take_007` on either). A take's folder is always a new one (`f_mkdir`, never an existing folder): a listing that
  missed a take moves on to the next number, and a listing that fails (a read error) starts no take at all (`cannot
  read /Recordings`), so an earlier take is never overwritten. Only "there already" moves on: a folder that cannot be
  made (an error, a drive that hangs) ends the take at once, and a wait over a second marks the drive gone. Everywhere else a drive's take is called by its volume: `USB:take_007` (the web page's takes
  list marks it `· USB`, the touchscreen shows `USB:take_007`, `/status` `rec.take`, `play=start:USB:take_007/3`,
  `play=toggle:USB:take_007`, `rec=del:USB:take_007`).
- **The takes lists** (web, touchscreen, playback) show the SD's takes and each mounted drive's, by name, newest
  first. The web page reads a drive's through `/fs` as `/USB/Recordings/...` (fsweb.h "WHAT IT CAN REACH": listed,
  downloaded, deleted; nothing else of a drive and no writes there). A drive's takes are not in the sample library.
- **Eject** is refused while the recorder writes to the drive or is armed for it (`rec=stop` first; `!` does not
  force it), from the command that armed it on.
- **The take being recorded is never deleted** (on the SD or a drive): `/fs` refuses to delete, rename or upload over it,
  a file in it, a folder above it or its volume's `/Recordings/.recording` while it records or closes (409 `the take is
  recording`; `fsweb::live_take`); `rec=del:` refuses too. FatFs here
  keeps no table of open files (`FF_FS_LOCK 0`): an unlink under the recorder would free clusters it goes on writing.
- **A pulled drive while recording:** stor's hot-plug pass unmounts it; the next write fails at once and the take ends
  with `write failed (drive full or pulled?)` (the web page, the touchscreen, the log). A write that was already on its
  way to the drive waits for Circle's transfer timeout (3 s a transfer, plus the mass-storage driver's reset and retries:
  core 0's loop stalls for that long once [inferred from `lib/usb/xhciendpoint.cpp`, `usbmassdevice.cpp`; to be
  measured]); a failed write that took over a second marks the drive gone and nothing more is sent to it (no headers,
  no close: `abandoned`, the page says so). The Overbridge audio keeps running (the xHCI interrupt).
  What reached the drive stays: while a take records, `<VOL>:/Recordings/.recording` names it, and every file's size
  reaches the directory every 2 s (on the SD one file every 2 s). When the drive is mounted again, the take its marker
  names gets its headers' sizes fixed (`rec: USB:take_007: recovered (...)`) and the marker goes. A take that ended
  abandoned while the drive stays mounted (it answered again) is repaired 3 s later the same way. Any drive call that
  waits over a second (a write, a sync, a header) marks the drive gone, so a pull costs one timeout, not one a file.
  `:rec=fix:USB` checks every take of a drive (the boot does the SD's); it runs where the command comes from (the web
  page's console: the web task on core 0) and reads every take's headers at once - seconds on a drive with many takes.
- **The shared USB host [the main risk, inferred]:** the Rytm's Overbridge audio and MIDI are on the same xHCI controller
  (the Pi 4's VL805 behind one PCIe lane). On a blue port a USB 3 drive runs SuperSpeed on its own lanes; the Rytm is a
  USB 2 HighSpeed device on the black port, so their bulk and isochronous traffic do not share a bus - only the
  controller and its PCIe link (~4 Gbit/s), far from full at 4 MB/s. A USB 2 stick (or a USB 3 one on a black port)
  shares the 480 Mbit/s bus with the Rytm: its isochronous audio keeps its reserved share, the stick gets the rest and
  is slower. Each write blocks core 0 for its transfer (64 KB: ~0.2-0.5 ms at USB 3, ~2 ms at USB 2); the audio runs from
  the xHCI interrupt meanwhile. What to watch on the first takes: the output's underruns (Status tab), the dropped blocks
  and the worst write (Record tab), the bench's `+writes` row with the Overbridge stream on.
- **The rules kept** (above): every write to a drive goes through a 64 KB-aligned bounce block in pieces of <= 64 KB
  (`stor::piece`) and every FIL of a take sits in a 4 KB slot of a 64 KB-aligned block of the low heap
  (`rec_pi.cpp`, 256 KB reserved with the recorder; without it the SD only). All of it on core 0. Core 3's capture side
  is unchanged (the RAM ring). The SD's card queue (the streams first) does not hold up a drive's writes.

## Using an NVMe SSD (Pi 5)

- Hardware: an M.2 HAT on the Pi 5's PCIe connector (the official M.2 HAT+, or a bottom-mount NVMe base). The PoE HAT
  is gone, so the connector is free.
- **Format the SSD FAT32 with the MBR scheme** (as above; a 256 GB+ SSD in FAT32 needs a FAT32 tool on Windows, Disk
  Utility on the Mac does it). 512-byte LBA format only (Circle's driver refuses 4Kn namespaces).
- **Circle's NVMe driver is experimental** (`addon/nvme`, Circle 50.1+: "Be sure that your SSD does not contain
  important data"). Use an SSD with nothing on it you cannot lose; keep the SD as the boot drive and the place of record.
- At boot the Pi 5 kernel looks for it once (`stor: NVME: looking for an SSD...`): no hot-plug. With no SSD the PCIe link
  stays down and the search costs about 100 ms [inferred: Circle's link-up wait, `lib/bcmpciehostbridge.cpp`]; a drive
  that answers the link but hangs costs its own controller timeout (`CAP.TO`, up to seconds) [inferred]. `nvme=0` in
  `cmdline.txt` skips it. PCIe runs at gen 2 x1 (Circle's `PCIE_GEN 2`), ~400 MB/s on the wire [inferred].
- Eject (`stor=eject:NVME`) unmounts it; the driver cannot detach the device, so it stays listed as ejected until the
  next boot (`stor=mount:NVME` mounts it again).

## exFAT (and GPT) - a build switch, off

`STOR_EXFAT=1 pi/engine/build.sh kernel` (default 0) builds against a Circle tree of its own (`circle-engine-exfat`,
`circle-engine-pi5-exfat`, made once from `pi/circle/build/circle` like the others) whose FatFs has **`FF_FS_EXFAT 1`
and `FF_LBA64 1`** (exFAT, files over 4 GB, GPT). Every library that uses FatFs (the wlan addon, wpa_supplicant) is
compiled in that tree with it: the `FATFS` / `FIL` layouts change with those two options, so mixing objects would break.
The first build of such a tree stops at wpa_supplicant (its hostap checkout is cloned on the host only once the tree
exists): run the same command again. The Pi 4 exFAT kernel builds [measured, 2026-10-21; 3.2 MB].

**Host test [measured, 2026-10-21, Circle 51.1.1's FatFs R0.16 on the Mac]** (`make -C pi/engine/tests`:
`test_stor_fatfs` and `test_stor_fatfs_exfat`; `make -C pi/engine/tests fatfs-mac` adds images macOS makes):

| image | default build | `STOR_EXFAT=1` |
|---|---|---|
| FAT32 / FAT16, MBR (FatFs's own mkfs; macOS `hdiutil -fs "MS-DOS FAT32" -layout MBRSPUD`) | mounts, files ok | mounts, files ok |
| FAT32, GPT (macOS `-layout GPTSPUD`) | not mounted: "GPT partition table" | mounts, files ok |
| exFAT, MBR (FatFs's mkfs; macOS `-fs ExFAT -layout MBRSPUD`) | not mounted: "exFAT: FAT32 only in this build" | mounts, files ok |
| exFAT, GPT (macOS) | not mounted: "GPT partition table" | mounts, files ok |
| exFAT 6 GB, a 5 GB file written at its end and read back | - | ok |

Each mount: files written and read back through the 64 KB piece rule, the free count before / after, an unmount and a
mount again (the eject's first step). Not yet run on a Pi: before turning it on, build it, boot it once on each board,
mount an exFAT stick and run `stor=bench:USB`, and check that Wi-Fi still joins (wpa_supplicant reads its file through
FatFs).

## The rules the volume layer keeps

- **Core 0 only.** Every mount, file and bench operation on `USB:` / `NVME:` runs in core 0's loop (`stor_pi::Poll`
  after `UpdatePlugAndPlay`, the console, the web task). Core 3 (the audio, the control) never touches them, and never
  waits for them. FatFs's per-volume lock is a spin lock deleted at an unmount, so a user on another core must go
  through a queue to core 0, as the SD's streams do (`core/msamp/cardq.h`).
- **I/O in pieces of at most 64 KB, never across a 64 KB boundary of the buffer** (`stor::piece`; `stor_pi::Read` /
  `Write`), from 64 KB-aligned buffers in the low heap (`HEAP_DMA30`). Why: Circle's xHCI driver puts a bulk transfer
  into **one Normal TRB** (`lib/usb/xhciendpoint.cpp`), whose length field holds at most 64 KB and which must not cross
  a 64 KB boundary (the xHCI spec) - a larger read would be corrupt or overflow into the TRB's other fields [inferred
  from the source; not provoked on hardware]. FatFs hands the drive up to a whole cluster per call: on a 256 KB-cluster
  exFAT drive one 1 MB `f_read` became a 512-sector (256 KB) device read; through the piece rule the largest was 128
  sectors (64 KB) and none crossed 64 KB [measured: host test]. NVMe gets the same rule: Circle's driver does not split
  at the drive's MDTS (largest transfer), so a 1 MB command may be refused [inferred].
  - FatFs's own sector windows (`FATFS`, `FIL`) sit in 4 KB slots of one 64 KB-aligned block, so they never cross either.
  - A caller's buffer that is not cache-aligned makes Circle bounce each call through a buffer of its own (`new
    (HEAP_DMA30)`), which may cross 64 KB: use aligned buffers [inferred: `usbmassdevice.cpp`].
- **The SD's order is kept:** the bench on the SD waits while the streams need the card (`cardq` busy), as the recorder
  does. USB and NVMe have their own FatFs locks: they never hold up the SD.
- **One USB transfer blocks core 0** for its duration (Bulk-Only Transport, QD1, synchronous; the xHCI interrupt keeps
  running, so the Overbridge audio does too). A 64 KB piece is ~0.2-0.5 ms at USB 3, ~2 ms at USB 2 [inferred]; the
  bench's busy phase measures what it does to core 0's loop.
- **The free count at mount** comes only from what is cheap: FAT32's FSINFO sector when it is valid (as Windows and
  macOS leave it), exFAT's allocation bitmap. A FAT32 without a valid FSINFO would need its whole FAT read (a 64 GB
  stick: ~8 MB in 512-byte reads, seconds of a stopped core 0), so it shows "free space not counted" until
  `stor=free:USB` (or a bench's start) counts it, logged with the time it took. The mount line logs the mount's time.

## The bench: `stor=bench[:VOL][:MB][:n=N][!]`

Never runs by itself. Console (`:stor=bench:USB` on the serial console, or the web page's *Log & console* /
`POST /cmd c=stor=bench:USB`), or the web page's Status tab, *Storage*, *Run bench*. Results: the serial log and the
web log as a table, `/status` `stor.bench`, the web page, and `SD:/.rytm/storbench-<VOL>.txt`; `stor=last` prints the
last table again.

- `VOL`: `SD` (default), `USB`, `USB2`, `USB3`, `NVME`. `MB`: the test file, default 256 (8..8192; shrunk to the free
  space less 24 MB). `n=`: reads per random / busy phase, default 1000 (20..4000).
- **Refused** while the Rytm plays (its MIDI clock: PLAY / CONTINUE until STOP) or a take plays back, while the recorder
  is armed or records, while the SD card test runs, or when the volume is not mounted. A trailing `!` runs it anyway (not past the last
  two). `stor=cancel` stops it and removes its files. The bench never formats anything and writes nothing but its own
  two files (`storbench.tmp`, `storbench2.tmp` at the volume's root; on the SD in `/.rytm`), removed at the end, on a
  cancel and on a failure.
- **Phases** (`pi/engine/stor_bench.h`), on core 0's loop, at most 8 ms of I/O a pass:
  1. sequential write + sync + read of the file in 64 KB, 256 KB and 1 MB pieces, each piece in calls of <= 64 KB on every
     volume (USB: the TRB rule; NVMe: no MDTS split in Circle's driver; SD: a 1 MB call would hold the SD's FatFs lock
     long enough for core 3 to take a stream's read back and spin on it), so the larger pieces differ only in how many
     calls run back to back in a pass; every piece read back is compared with what was written;
  2. random reads at QD1: N x 4 KB and N x 64 KB (aligned) in the file: p50 / p99 / max latency, IOPS;
  3. the raw device without FatFs (Circle's `CDevice::Read` on `emmc1` / `umsdN` / `nvme1`, under FatFs's lock of that
     volume): 64 KB sequential over the file's size from the device's start, N x 4 KB random (read only);
  4. busy: N x 64 KB random reads, one a pass, while the Overbridge stream runs (if it does: the `OB` column), then again
     with 64 KB writes to the second file paced at 2 MB/s (the recorder's rate, at most 16 MB); per phase core 0's loop wake latency
     max / mean (`sleep1msMaxUs`, as `/status` core0) and the output's underruns (`under`).
- **Link info** in the table's first line: the SD's mode is the build's (4-bit, High Speed 50 MHz: Circle has no
  accessor) [inferred]; USB: the speed the device enumerated at (USB 3 SuperSpeed / USB 2 HighSpeed) and VID:PID
  [measured by Circle]; NVMe: the model (Identify) and PCIe gen 2 x1 (Circle's setting; the log's `Link up, 5.0 Gbps x1`
  line confirms the negotiated rate).

Example (the host test's simulated drive; numbers there are made up):

```
stor bench USB: 8 MB file, exFAT, USB 3 SuperSpeed, calls <= 64 KB
seq      write MB/s read MB/s  worst w/r ms  sync ms calls
64 KB         49.07    100.00       1.2/0.6    3.000     1
256 KB        49.07    100.00       5.0/2.5    3.000     4
1 MB          49.07    100.00     20.0/10.0    3.000    16
QD1       p50 ms  p99 ms  max ms   IOPS
rnd 4K     0.039   0.039   0.039  25641
rnd 64K    0.625   0.625   0.625   1600
raw 4K     0.019   0.019   0.019  52631
raw seq 64K 200.32 MB/s over 8 MB (no FatFs)
busy 64K  p50 ms  p99 ms  max ms loop max/avg us under  OB  w MB/s
reads      0.625   0.625   0.625          0/1010    +0  on       -
+writes    0.625   0.625   0.625       2250/1010    +0  on   50.00
verify ok, 0 s
```

### Running it on the hardware

With the stor kernel installed and the Rytm connected but stopped (nothing recording), on the serial console (921600
baud, a `:` before each command) or the web console:

```
:stor                       # the volumes: SD, USB, ..., NVME (state, format, free, link)
:stor=bench:SD              # the SD card (a few minutes; the sound may drop out on the SD)
:stor=bench:USB             # a USB 3 stick or SSD on a blue port (Pi 4) / any port (Pi 5)
:stor=bench:NVME            # Pi 5, the NVMe SSD
:stor=last                  # the last table again
:stor=free:USB              # count the free space of a FAT32 without FSINFO
:stor=eject:USB             # before pulling the drive
```

From the Mac without the serial cable: `curl -X POST --data 'c=stor=bench:USB' http://sidecar.local/cmd`, then the
web page's Status tab (or `curl -s http://sidecar.local/status | python3 -c 'import json,sys; print(json.load(sys.stdin)["stor"]["bench"].get("table",""))'`).

### Results (fill in from the hardware)

One row per drive and board; the 64 KB / 1 MB columns from the `seq` rows, latencies from `rnd`, the busy columns
from `+writes`.

| board | drive | link | FS | seq W 64K | seq W 1M | seq R 64K | seq R 1M | 4K p50 / p99 ms | 64K p99 ms | raw 64K MB/s | busy loop max us | underruns |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Pi 4 | SD card (model) | SD HS 50 MHz | FAT32 | | | | | | | | | |
| Pi 4 | USB 3 stick (model) | USB 3 | FAT32 | | | | | | | | | |
| Pi 4 | USB 3 SSD (model) | USB 3 | FAT32 | | | | | | | | | |
| Pi 5 | SD card (model) | SD HS 50 MHz | FAT32 | | | | | | | | | |
| Pi 5 | USB 3 stick (model) | USB 3 | FAT32 | | | | | | | | | |
| Pi 5 | USB 3 SSD (model) | USB 3 | FAT32 | | | | | | | | | |
| Pi 5 | NVMe SSD (model) | PCIe gen 2 x1 | FAT32 | | | | | | | | | |

Expectations to check them against [inferred, research report]: SD 15-22 MB/s read, 8-12 write (the bus is 25 MB/s);
USB 2 stick 20-35 MB/s; USB 3 stick / SSD 80-250 MB/s sequential (BOT's per-command round trip, not the 5 Gbit/s link,
is the limit at 64 KB); NVMe ~280 MB/s reads (upstream, 16 KB blocks), writes well above upstream's 30 MB/s FatFs
figure in large aligned pieces. Upstream has no Circle USB mass-storage figures.

## Risks

- **The 64 KB TRB limit** is kept by the engine, not by Circle: since 2026-10-26 at the disk layer for every FatFs
  call on a USB drive (`stor_diskio.cpp`, below). Code that talks to a `umsd` device directly (the bench's raw reads:
  `CDevice::Read` from its own aligned buffers) must keep it itself.
- **NVMe is experimental upstream** (one I/O queue at QD1, no MDTS split, no power management, tested upstream with
  Raspberry Pi's own SSDs). Test SSDs only; the SD stays the place of record.
- **exFAT/GPT** are host-tested only; the switch rebuilds every FatFs user.
- A pulled drive (no eject) can lose a write in flight; the count shows on the page and in `stor`.
- **Recording to a drive** (above): it shares the xHCI controller with the Rytm's audio (measure, then watch the
  underruns and the dropped blocks); a drive pulled mid-write can stall core 0 for Circle's transfer timeout (3 s and
  the driver's retries) once. On a drive every file is synced every 2 s (25 files: a small directory write every
  80 ms) - the price of losing at most ~2 s on a pull.
- **The 64 KB TRB rule at the disk layer (2026-10-26):** it no longer depends on every caller. `pi/engine/stor_diskio.cpp`
  is the engine's own FatFs disk layer in place of Circle's `addon/fatfs/diskio.cpp` (Circle's file untouched, no
  patch: the object is linked before `libfatfs.a`, so the archive's `diskio.o` is never pulled in; a second definition
  would fail the link). On `USB:`, `USB2:`, `USB3:` every `disk_read` / `disk_write` goes through `stor::split_io`: a
  sector-aligned buffer straight to the device in pieces of <= 64 KB never across a 64 KB boundary, any other buffer
  through one 64 KB-aligned bounce block (reserved at boot with the volumes' block, low heap). That covers FatFs's
  own transfers too: `dir_clear`'s zero-fill when a folder is made (`FF_USE_LFN` stays 3: a `malloc`'d buffer of up to
  32 KB that may cross 64 KB), FAT / directory sectors, and the multi-sector `f_read` / `f_write` straight to or from a
  caller's buffer (up to a whole cluster: 256 KB on a big exFAT drive) [measured: host test, `test_stor_fatfs_lfn3`
  puts dir_clear's buffer across 64 KB on purpose; on a Pi: to be seen]. The SD, `ufd1` and the NVMe SSD keep Circle's
  code exactly. `:stor` logs the layer's counts (`USB disk layer (64 KB rule): N transfers split, M pieces through the
  bounce`). The piece rule above it (`stor_pi::Read` / `Write`, the recorder's bounce, `platform_pi.cpp`'s) stays:
  harmless, and it keeps the large transfers sector-aligned, so they need no bounce.
  - When Circle is upgraded: compare `addon/fatfs/diskio.cpp` with `stor_diskio.cpp` (the device table and the
    non-USB paths are a copy of Circle 51.1.1's).
- The bench on the SD competes with the streams and FatFs's lock: refused while playing unless forced; it ends each
  step early while the streams need the card (cardq busy), but a card's own write pause (100-137 ms measured) inside
  one 64 KB call can still hold a stream's read past cardq's take-back (200 ms) on a forced run.
- An ejected stick on a Pi 5 root port: Circle removes the device but leaves the port enabled; whether a later port
  event enumerates it again (and it is mounted again) is to be checked on the hardware.

## Code and tests

| part | file | tests |
|---|---|---|
| volumes: hot-plug states, eject, the piece rule, sector 0's diagnosis, JSON | `pi/engine/stor_vol.{h,cpp}` (portable) | `pi/engine/tests/test_stor_vol.cpp` |
| the bench: parser, refusals, phases, percentiles, table | `pi/engine/stor_bench.{h,cpp}` (portable) | `test_stor_bench.cpp` (a simulated drive with its own clock, a temp folder) |
| Circle's side: FatFs mounts, umsd / nvme devices, raw reads, commands, `/status`, the STATUS rows | `pi/engine/stor_pi.{h,cpp}` | the kernels build (Pi 4, Pi 5) |
| FatFs's disk layer: the TRB rule for every transfer on a USB drive (`stor::split_io`) | `pi/engine/stor_diskio.cpp`, `stor_vol.cpp` | `test_stor_fatfs*.cpp` (device calls counted after the split; `_lfn3`: the Pi's `FF_USE_LFN 3`) |
| FatFs (default and `STOR_EXFAT`) on images | Circle's `addon/fatfs` | `test_stor_fatfs.cpp`, `make fatfs-mac` |
| STATUS page rows | `pi/ui/sidecar_status.{h,c}` | `pi/ui/host` shots (`*-status*.png`) |
| web Storage section | `pi/engine/www/index.html` | `pi/engine/tests/page_stor.test.mjs`, `mock_web.py` |
| recording to a drive: the destination, the marker, recover(), a pulled drive | `pi/engine/recorder.{h,cpp}` (portable), `rec_pi.cpp` | `test_recorder.cpp` |
| a drive's takes in the lists and playback | `pi/engine/rec_play.{h,cpp}`, `pi/ui/sidecar_rec.c` | `test_rec_play.cpp`, `pi/ui/host` shots (`*-rec-*usb.png`) |
| `/fs` reaching a drive's `/Recordings` | `pi/engine/fsweb.{h,cpp}`, `web.cpp` | `test_fsweb.cpp` |
| the Record tab's *Record to* | `pi/engine/www/index.html` | `page_rec.test.mjs`, `mock_web.py` |
