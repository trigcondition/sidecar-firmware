# Hardware testing through the Sidecar (the Rytm MIDI bridge)

With the Rytm plugged into the Sidecar, its USB port belongs to the Pi, so the Mac's test tools cannot see it as a MIDI
device. The **Rytm MIDI bridge** (Sidecar 0.3.0; the design and the rules: `docs/pi-engine.md` "Rytm MIDI bridge")
gives them the Rytm's USB MIDI over the network instead, as the same raw byte stream that analog-emu's
`--usb-midi-tcp` serves. Iced-Cream's tools (analogusb.py, analogrun, elkflash, elkbench) take it as
`--port tcp:HOST:PORT`.

```
analogrun / analogusb.py / elkbench  --tcp:127.0.0.1:7321-->  tools/rytm_bridge.py  --TCP 7330, challenge-->  Sidecar  --USB-->  Rytm
```

The Sidecar keeps working normally meanwhile: it still gets everything the Rytm sends, and its own messages (the
mods' host link, the keys, the computer's MIDI) still go to the Rytm, with the tool's messages between them.

## 1. Turn the bridge on (every boot)

It is **off at every start**, and it needs the pairing code: with `paircode=0` in cmdline.txt it refuses to turn on.
Turn it on for this boot in one of two ways:

- **The web page**: `http://sidecar.local`, paired with the code on the Rytm's screen, **Status > Rytm MIDI bridge >
  Turn on for this boot**. The page then shows the bridge's **token** (32 hex digits, new each time it is turned on)
  with a Copy button. The token is shown nowhere else: not in the log, not in `/status`, not on the console.
- **The serial console**: `midibridge=on` (then read the token from the web page). `midibridge` logs the state and the
  counters, `midibridge=off` closes it. (`POST /cmd` refuses `midibridge=`: from the network only the page can turn it on.)

The section also shows the state - off, waiting, or connected from an address - the bytes each way, and the messages
dropped (the tool stopped reading) or blocked.

## 2. Start the relay on the Mac

The tools send raw MIDI from their first byte, but the Sidecar wants its challenge answered first, so a small relay
sits between:

```
export SIDECAR_BRIDGE_TOKEN=<the token from the page>     # or --token-file FILE (chmod 600); never on the command line
python3 tools/rytm_bridge.py --pi sidecar.local
```

It checks the token once at the start (a wrong one is said at once), then listens on `127.0.0.1:7321`. For the tool
that connects it answers the Sidecar's challenge (`HMAC-SHA256(token, nonce)`, port 7330; the token itself never goes
over the network) and copies the bytes both ways, unchanged. Since anything on the Mac - including a web page in its
browser - can reach 127.0.0.1:

- **MIDI only**: a client gets the bridge only once its first byte is a MIDI status byte; one that sends text (every
  HTTP request) or nothing at all within 1 s is closed, and nothing of it reaches the Sidecar;
- **one tool at a time**: the first keeps the bridge until it disconnects; another is closed at once (so close one tool
  before starting the next).

A tool that connects and only *reads* at first (a MIDI monitor) would be closed after 1 s: start the relay with
`--passive` for it. It then gets the bridge after that second, and its first byte is still checked when it comes; the
cost is that a silent client (a browser's preconnect, a stray process) can then hold the one slot until it closes.
analogrun, analogusb.py and elkflash send first and do not need it.

`--pi 10.0.0.2` or `--pi HOST:PORT` for an address; `--listen [::1]:7321` or another local port (a non-loopback address is
warned about: anyone reaching it gets the Rytm's MIDI without the token). Standard library only, no packages.

## 3. Point the tools at it

```
python analogusb.py --port tcp:127.0.0.1:7321 cfw
python analogusb.py --port tcp:127.0.0.1:7321 stats 5
python analogrun.py --port tcp:127.0.0.1:7321 data ls /
python -m elkflash info --port tcp:127.0.0.1:7321
```

elkbench: set the hardware unit's port in its `config.toml`,

```
[units.rytm]
port = 'tcp:127.0.0.1:7321'
```

and run its hardware steps as usual (keep the relay running; elkbench's own emulator units are not affected).

## 4. OS upgrades and +Drive writes are blocked unless you allow them

By default the Sidecar refuses on the bridge:

- the Elektron **OS upgrade** messages (the MIDI RPC's OsUpgradeStart / Write / End, as elkflash and Elektron Transfer
  send them, and the OS `.syx` file's own messages): `elkflash upgrade` gets no answer;
- the **+Drive writes**: every RPC command that is not a known read (put, delete, rename, move, copy, clear, swap, the
  RAM slots, and any id the Sidecar does not know). **analogrun's data writes - `data put`, `roundtrip`, `clear` - need
  the "+Drive writes" switch**; without it they get no answer.

So a test plan cannot flash the Rytm or wipe the user's +Drive by accident; the page counts the messages as
**blocked**. Reads (`data ls`, `get`, ping, version, screenshots, `analogusb.py stats`) always pass, and kit and pattern
dumps are not filtered. To allow either for this session, tick **allow OS upgrades** or **allow +Drive writes** on the
page (or `midibridge=os:on`, `midibridge=data:on` on the console), with the bridge on. Both go off again with the
bridge and at every boot; turning one off also drops what was queued under it.

## 5. Ending it

**Turn off** on the page or `midibridge=off`: the tool's connection closes, anything of its still queued for the Rytm is
dropped (a SysEx already half sent is finished first, so the Rytm is not left inside one), and the token is gone - the
next turn-on makes a new one. **Forget every device** (Status > Access) turns the bridge off too. Five wrong answers
from one address lock that address out for 10 minutes; the bridge itself stays on for everyone else.

## Notes

- The bridge reaches the Rytm when the Sidecar's link is the Rytm's USB (`link=usb`, the hardware case).
- **A trusted network only, and turn the bridge off when the test is done.** The stream is plain TCP on the LAN, like
  the web page. The challenge keeps the token off the bridge's own wire and a sniffed handshake cannot be replayed, but
  the page that shows the token is plain HTTP, the MIDI itself is not encrypted, and someone active on the network
  could take over the connection after the handshake. Turning it off ends all of that; the next turn-on has a new token.
- The computer's MIDI clock (from a DAW on the Sidecar's USB-C) is never held back by the bridge: it goes out inside a
  long SysEx. Its notes - and, once something waits, its start / continue / stop, so they keep their order with a
  song position - wait for the SysEx to end (156 ms at most).
- If the Rytm stops taking USB MIDI (any send fails after 3 s), the Sidecar stops sending for 5 s, then tries again (up
  to 60 s apart while it keeps failing), one send per try; what the bridge had queued, and the computer's messages that
  were waiting, are dropped.
- Throughput to the Rytm is at most 250 KB/s (paced below the Rytm's ~300 KB/s), after the Sidecar's own messages. A
  SysEx is at most 38912 bytes (the largest RPC message, 37457, fits); a longer one is dropped. While one is going out
  the Sidecar's own messages wait, 156 ms at most.
- The client's address is in the log at every connect and close (`midibridge: client ... connected`).
- Tests: `make -C pi/engine/tests` (`test_midibridge`, and `tools/tests/test_rytm_bridge.py` against a fake Sidecar).
