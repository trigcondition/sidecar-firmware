# Flashing the Teensy relay from the Pi

The Teensy 4.1 relay (docs/teensy-mirror.md) sits on a Pi USB-A port by its micro-USB. A new relay build used to mean moving
that cable to the computer and running Teensy Loader. The Pi now writes the program itself: the computer uploads the
`.hex` to the Pi's web server, the Pi tells the relay to enter its bootloader, writes the blocks, starts the program, and
checks that the relay comes back with the expected build.

State: works on the unit (T334, 2026-11-15, 77cf2c7): the relay's own build written again from the Pi, 60 blocks in 0.5 s
(38 refusals while block 0 erased), the relay back naming its build 2.1 s after the request.

## Use

```
python tools/teensy_flash.py mirror_relay.ino.hex --host 169.254.61.60 --code <pairing code> --expect <build>
```

`--expect` is the stamp `teensy/mirror_relay/build.ps1` compiles in (the git short hash, `+` for a changed tree; the relay
logs it as `mirror relay up, protocol 1.00, build <stamp>`). Without it the write is not checked against a build, only
that the relay came back. `--upload-only` uploads and checks the file without writing.

## Endpoints (paired; the writes take the lenient same-origin check, as /cmd)

| | |
|---|---|
| `POST /teensy/put?off=&total=&crc=` | the `.hex` in chunks (one multipart part each, up to 64 KB). The Pi parses each chunk as it comes; the last one's CRC-32 must be the whole file's. Answers `200 ok <next offset>`, then `200 done <bytes> bytes, <n> blocks, sha256 <hex>`. |
| `POST /teensy/flash` `expect=` | starts the write. `409` when no file is uploaded, a write runs, or no Teensy is on the USB-A ports. |
| `GET /teensy/state` | `{"state":"idle|uploading|ready|entering|writing|booting|checking|done|failed", "hex":{bytes,received,sha256,blocks}, "sent", "retries", "ms", "expect", "build", "error", "bootloader", "relay"}` |

## What the Pi does

1. **Check the file.** `teensy_hex.h` parses the Intel HEX: every record's checksum, an end record, only data inside the
   Teensy 4.1's program flash (0x60000000..0x607C0000, rebased to 0). Nothing is sent to the Teensy unless the whole
   file passed.
2. **Bootloader.** If the relay is on the link (1209:0002), `CTeensyTarget::RequestBootloader` makes its MirrorPoll send
   `kReqBootloader` (wValue 0x4C42); the Teensy reboots about 50 ms later and comes up as HalfKay, 16C0:0478. A Teensy
   already in its bootloader (a failed write, or the program button) is used as it is: that is the recovery path.
   While the Teensy moves (to HalfKay here, back to the relay after step 5) and it hangs off the Pi 4's VL805 hub
   (2109:3431), the hub's ports are read every 250 ms (`circle-hub-poll.patch`, `CUSBStandardHub::RequestPortPoll`).
   Beside a streaming Rytm the hub's status-change endpoint went unserved, so the Teensy's unplug and HalfKay's arrival
   were never seen and the write gave up after 15 s (BL3, T411); with the Rytm off both came within 0.45 s.
3. **Claim.** `circle-usbdevicefactory4.patch` hands the device name `ven16c0-478` to `CTeensyBoot` (teensy_flash.cpp)
   before its HID interface would reach Circle's gamepad driver.
4. **Write.** One HID output report per 1 KB block as SET_REPORT (0x21, 9, wValue 0x0200, wIndex 0), 1088 bytes: the
   block's offset in bytes 0..2 (little endian), zeros to byte 63, the block's data. Block 0 always (its write erases the
   chip), then every block with a byte other than 0xFF, in address order. Each report is an async control URB stepped by
   a task of its own, so core 0's loop never waits on it. A refused report is sent again after 10 ms; the first four
   blocks may take up to 45 s each (the erase), the others 2 s. A report that is never answered ends the write with an
   error and its buffer is not used again.
5. **Start.** A last report with offset 0xFFFFFF starts the program.
6. **Check.** The relay must come back on the link within 20 s, and its log (`CTeensyTarget::RequestLog`, read by its
   MirrorPoll after Start) must hold its start line within 20 s more. Its build must equal `expect=` when given.

The relay's log is read destructively (kReqLog empties the Teensy's ring), so a bench `--info` after a flash does not
see the lines the Pi took.

## Limits and risks

- A bad write leaves the Teensy in its bootloader: flash again, or press its program button. The Pi never writes the
  bootloader itself (HalfKay is in a separate chip).
- `relaydelay=`: the bootloader request and the log read wait for the relay's Start. A request made before Start is kept
  and sent at Start; if the write gave up (15 s) before that, the Teensy then waits in its bootloader for the next
  `POST /teensy/flash`.
- No emulator covers the bootloader's USB path. The host tests (`pi/engine/tests/test_teensy_hex.cpp`,
  `test_teensy_flash.cpp` on a fake Circle in `shim_tf/`) cover the parser, the reports and the write's steps; the USB side
  is covered by unit runs (T334).
