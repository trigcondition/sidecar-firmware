# The USB-C network: the web page over the USB-C cable

Current state (2026-11-07): built for 0.5.0 (target). Runs on the Pi 5 with a Mac (macOS's own NCM driver, Mac
unlocked): the boot chose USB-C 0.8 s after the gadget's start, DHCP lease, ping under 1 ms, the page, `/status` and
`sdboot.py ls` over 172.31.77.1, audio device still present. Still to try on hardware: see "Not yet tested". The portable parts are tested on the
host (`make -C pi/engine/tests`: `test_ncm_ntb`, `test_dhcpd_lite`, `test_usbnet_cfg`, `test_usbnet_ring`,
`test_sidecar_desc`); the gadget, the net device and the boot's choice need a computer on USB-C (see "Not yet tested").

With a computer on the Sidecar's USB-C port, the Sidecar's web page and its kernel updater work over that cable, with
no Wi-Fi and no Ethernet: plug in, open **http://172.31.77.1**. The same cable carries the Sidecar's audio and USB MIDI
as before. This is how a new unit can be set up (Wi-Fi included) without a LAN cable or a Wi-Fi network first.

## How it works

In CLASS COMPLIANT mode the Sidecar's USB-C device has one more function next to its audio and MIDI: a USB network
adapter (CDC-NCM, the class macOS, Linux and Windows 11 drive with their own driver). The computer sees a new network
interface ("Sidecar | Expander (Pi)") and gets an address from the Sidecar by DHCP.

| | |
|---|---|
| the Sidecar | 172.31.77.1 (`usbip=`), mask 255.255.255.0 |
| the computer | 172.31.77.2, by DHCP (a one-hour lease) |
| router, DNS | none: the computer's internet stays on its own Wi-Fi or Ethernet |
| sidecar.local | also answered on the cable (mDNS; not yet tested) |
| services | the web page (HTTP, /ws), the SD card's boot files (TCP 7310, `pi/circle/sdboot.py`) |
| not on it | the Rytm MIDI bridge (TCP 7330): the computer has the Rytm's MIDI on that cable already |

The engine runs one network stack on one device (Circle). At boot it picks the device:

- `net=auto` (the default): when a computer on USB-C starts its USB network driver at boot, the USB-C network.
  Otherwise the network of today: Wi-Fi with a stored network, else Ethernet. **A Sidecar with no computer on USB-C
  comes up exactly as before.**
- On the USB-C network the Wi-Fi chip is still started, for scans only: the Wi-Fi settings work from the page. A stored
  network is not joined that boot.

The wait at boot runs while the Wi-Fi chip starts (about 1-2 s), so it rarely adds time:

| what is on USB-C | detected by | result |
|---|---|---|
| nothing, or a power supply | no start-of-frame on the bus within `usbquiet` ms (700) of the gadget's start | today's network, no extra wait |
| a computer whose network driver starts | the computer selects the NCM data interface (its driver took the link) | the USB-C network |
| a computer with no driver for it (Windows 10, see below) | start-of-frames, but no driver within `usbwait` s (5) | today's network, after that wait |

When the boot chooses Wi-Fi or Ethernet, the network function stays in the USB-C device (removing it would replug the
computer's audio), but it tells the computer the cable is not connected and takes nothing from it.

The choice is made once per boot. Plugging a computer in later does not move the network to the cable, and pulling the
cable does not move it back to Wi-Fi: reboot the Sidecar (from the page, or its power) to choose again.

## Trust: no pairing code on the cable

The USB-C cable is trusted: on the USB-C network the page and port 7310 need no pairing code (as with `paircode=0`, for
that cable only). The trust follows the interface a connection arrived on, never an address: with one stack on one
device, every connection on the USB-C network came over the cable, and nothing from Wi-Fi or Ethernet can reach that
stack. Wi-Fi and Ethernet keep the pairing code as before. `usbtrust=0` asks for the code on the cable too.

One thing is still asked on the cable: an HTTP request that changes or reads something gated must carry the
`X-Sidecar-Auth` header, with any value (the pages send `usb`). Another web site open on the same computer cannot send
that header (the browser would first ask the Sidecar's permission, which it never gives), so it cannot change the
Sidecar through the trusted cable with a hidden image or form. The WebSocket keeps its strict same-origin check. A tool
on the cable adds the header itself: `curl -H 'X-Sidecar-Auth: usb' http://172.31.77.1/...`.

The SIDECAR MENU's WEB UI off still turns the page and port 7310 off on the cable too.

## The settings (cmdline.txt)

The keys go on the card's `cmdline.txt` line, like the engine's other options (`docs/pi-engine.md`); an unknown value
is logged and the default used.

| key | default | |
|---|---|---|
| `net=auto\|usb\|wifi\|lan` | auto | auto: as above. `usb`: always the USB-C network (the Sidecar waits for the computer). `wifi`: never the USB-C network (Wi-Fi with a stored network, else Ethernet: today's behaviour). `lan`: Ethernet, even with Wi-Fi stored (the chip for scans) |
| `usbnet=on\|off` | on | the network function in the USB-C device. off: the device is 0.4.2's, byte for byte |
| `usbip=A.B.C.D` | 172.31.77.1 | the Pi's address on the cable (/24). The computer gets .2 (or .1 when the Pi is .2). Not 0.x, 127.x, multicast, or a .0 / .255 host |
| `usbwait=S` | 5 | net=auto: how long a computer on the bus may take to start its network driver (1..30) |
| `usbquiet=MS` | 700 | net=auto: no start-of-frame this long after the gadget's start is no computer (100..5000) |
| `usbtrust=1\|0` | 1 | 1: the cable needs no pairing code. 0: the code as on Wi-Fi and Ethernet |

For debugging: `net=wifi` or `net=lan` takes the cable out of the picture; `net=usb` forces it; `usbnet=off` removes
the function from the USB device altogether (if a computer has trouble with the composite device).

`/status` says which one runs: `"net":"usb|wifi|ethernet"` and `"usbNet":{"function","link","setting","ip",
"computer","trusted","late","why"}` (`late`: a computer started its USB-C network after the boot chose Wi-Fi or Ethernet) (`why`: the boot's reason, as in the log line `network: net=auto: ...`). The log has a
`usbnet` line every 10 s while the link is up (blocks and frames each way, malformed blocks, drops, requests).

## Interplay

- **OVERBRIDGE mode** (`docs/usb-mirror.md`): the USB-C network is a CLASS COMPLIANT function. A Sidecar that boots in
  OVERBRIDGE mode has no USB-C network (Wi-Fi or Ethernet as before). Switching to OVERBRIDGE from a page on the USB-C
  network takes that page's connection with it (the page warns): set CLASS COMPLIANT again on the Rytm (SIDECAR MENU >
  USB-C), or from Wi-Fi or Ethernet.
- **A Wi-Fi join being tried** (the page's Join: the next boot judges it, `wifi.try`): with `net=auto` that boot is a
  Wi-Fi boot, even with a computer on USB-C, so the join is judged. Afterwards the page on the cable is gone until the
  next boot; on the same network the page is at http://sidecar.local. With `net=usb` the trial waits for a boot on
  Wi-Fi (nothing is reverted).
- **`link=tcp`** (the emulator over Ethernet) keeps today's network.

## Computers

- **macOS**: its own NCM driver; nothing to install. **Unlock the Mac the first time the Sidecar is plugged in**: macOS
  does not attach a new network interface while the screen is locked (its log: "Console locked, network interface[new]
  ignored"), so the Sidecar sees no driver within `usbwait` and boots on Wi-Fi or Ethernet. Seen on hardware
  2026-11-07; whether a known interface comes up while locked later is not yet checked. When a computer starts its
  USB-C network after the boot's choice, the log says so (`usbnet: a computer started its USB-C network after the boot
  chose Wi-Fi or Ethernet ...`) and `/status` has `"usbNet":{"late":true}`. The page (on Wi-Fi or Ethernet) then offers **Reboot onto USB-C** in its
  USB-C settings (`POST /usbnet/reboot`: gated, this page only, refused unless a late USB-C network was seen); or set
  `net=usb`.
- **Linux**: `cdc_ncm`; nothing to install.
- **Windows 11**: its own NCM driver (UsbNcm); nothing to install.
- **Windows 10**: may not pick a driver for it by itself (newer Windows 10 builds have UsbNcm; not yet tried). In Device Manager: the "Sidecar |
  Expander (Pi)" network device without a driver > Update driver > Browse > Let me pick > Network adapters > Microsoft >
  UsbNcm Host Device. Until then the Sidecar boots on Wi-Fi or Ethernet after `usbwait` s.

## Notes

- The function: CDC-NCM 1.0, NTB16 only, 16 KB blocks each way, at most 8 datagrams in a block from the computer.
  Endpoints 0x86 (notifications), 0x87 IN and 0x07 OUT (bulk), interfaces 5 (communication) and 6 (data). No Circle
  patch was needed: the endpoints use circle-gadget-mirror.patch's IN and OUT endpoint on one number, the net device is
  Circle's type "Unknown", which CNetSubSystem can be asked for.
- At a link change (the computer selecting the data interface) both data endpoints are stopped, their data toggles set
  back to DATA0, and what was queued dropped. An armed endpoint is stopped and waited for (`pi/engine/ep_stop.h`: an OUT
  one under the global OUT NAK), at a link change, a bus suspend and when the boot chose Wi-Fi or Ethernet. Circle's
  plain cancel left the core's "EP disabled" to its interrupt handler, which panics on it for a bulk endpoint; the first
  hardware tries (Pi 5, 2026-11-07) died about 16 s after the link came up, for another reason: the net device's
  multicast filter answered Circle's default FALSE, and Circle's IGMP handler asserts on it the first time the link is
  up (the serial console: igmphandler.cpp(87)); the halted Pi was reset by the watchdog 15 s later. The device takes
  every frame anyway (NCM has no filter): it answers TRUE.
  `GET /prevboot` (paired) returns the last crashed boot's report and log tail (`/.rytm/prevboot.txt`).
- The trust is the cable's, not the computer's: a computer that forwards IP traffic between its interfaces could pass
  another machine's packets onto the cable. The Sidecar has no gateway on the cable, so it cannot answer them.
- The MAC addresses come from the board's serial number (locally administered): stable per Sidecar.
- A computer's request to set a value the Sidecar refuses (an unsupported NTB size or format) cannot be STALLed in
  Circle's EP0 (the request arrives after its status stage); the value is left as it was. The computers above never
  send one.
- The bandwidth is not measured yet; Circle's TCP stack, not the cable, is expected to be the limit (a few MB/s), enough
  for the page, file transfers and a kernel install.

## Not yet tested

Tried on the Pi 5 with a Mac: enumeration, net=auto choosing the cable, the DHCP lease, the page, `/status`, reading
the card with `sdboot.py`. Not yet: Windows 11, Windows 10, Linux, the Pi 4, /ws over the cable, an `sdboot.py` install
over the cable, a computer asleep and awake (bus suspend and resume), unplugging and plugging back, audio and MIDI
under network load, OVERBRIDGE mode with `usbnet=on`, the late link and its Reboot onto USB-C button.
