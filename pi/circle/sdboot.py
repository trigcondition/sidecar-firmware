#!/usr/bin/env python3
"""The Pi engine's SD card boot files, over the running engine's TCP port 7310 (pi/engine/sdfiles.h).

docs/pi-engine.md, "Boot from the SD card". Every write goes to <name>.new, is read back and checked on the Pi, then
put in place; the replaced file is kept as <name>.prev, and config.txt/cmdline.txt keep their original as <name>.bak.

  sdboot.py ls                         the card's root
  sdboot.py get NAME [OUT]             a file (to stdout, or OUT)
  sdboot.py scratch                    the write path's test (64 KB written, read back, deleted)
  sdboot.py install [IMG ...]          each IMG to its board's file, by what the image is (default
                                       pi/engine/kernel8-rpi4.img): a Pi 4 kernel -> engine.img (header + image), a
                                       Pi 5 kernel (RASPPI=5) -> kernel_2712.img (plain, the old one kept as .prev);
                                       on a Pi 4 + Pi 5 card (make-sd-dual.sh) from either board, the other board's
                                       kernel is left as it is. Anything else is refused.
  sdboot.py previous                   engine.img.prev back as engine.img (the two swap: run it again to undo)
  sdboot.py bootloader [IMG]           IMG (default pi/circle/out/sdboot.img, build-sdboot.sh) -> sdboot-try.img
  sdboot.py tryboot                    tryboot.txt = config.txt + the sdboot lines (kernel=sdboot-try.img if it is on
                                       the card, else sdboot.img), then a one-shot tryboot reboot (the next reboot
                                       or power cycle boots config.txt as before)
  sdboot.py commit                     sdboot-try.img -> sdboot.img (after a tryboot that worked)
  sdboot.py enable | disable           add / remove the sdboot lines at the end of config.txt
  sdboot.py restore-config             config.txt.bak (the original) back as config.txt
  sdboot.py cmdline [LINE]             show cmdline.txt, or replace it with LINE (the original kept as cmdline.txt.bak)
  sdboot.py reboot                     the engine stops and reboots
  sdboot.py pi5-try [IMG]              Pi 5 (no sdboot there, docs/pi5.md): IMG (default pi/engine/kernel_2712.img) ->
                                       sdboot-try.img, tryboot.txt = config.txt + "[pi5] kernel=sdboot-try.img", then
                                       a one-shot tryboot reboot (a power cycle boots kernel_2712.img as before)
  sdboot.py pi5-commit                 sdboot-try.img -> kernel_2712.img (the old one kept as kernel_2712.img.prev)
  sdboot.py pi5-previous               kernel_2712.img.prev back as kernel_2712.img (the two swap)
  sdboot.py usbreset                   a USB reset of the FT232R serial adapter (0403:6001; needs Homebrew's libusb):
                                       after a flasher killed mid-upload the Mac's writes to it can hang (measured
                                       2026-10-16: the Pi then hears nothing, not even the reboot magic)
The link: TCP to SIDECAR_HOST (default: PI_IP, else sidecar.local) port 7310 (SIDECAR_PORT moves it); if that does not connect within 3 s
(or SIDECAR_LINK=serial), the serial port (SIDECAR_SERIAL, default /dev/cu.usbserial-*, 921600; nothing else may have
it open): the console's ":sdf", then the same commands, data as base64 lines (about 50 KB/s, a 1.5 MB engine in ~35 s).

THE PAIRING CODE (docs/pi-engine.md "Pairing code"). Over TCP the engine serves nothing until this tool is paired,
because this is the port that replaces the firmware. The first time per Pi:

    SIDECAR_PAIR_CODE=12345678 uv run --with pyserial python pi/circle/sdboot.py install

- the 8 digits are the ones on the Rytm's screen and on the Sidecar's touchscreen (STATUS page, PAIR CODE). They
  change at every boot. The engine's console command `pair=show` puts them on the Rytm's screen again.
- without SIDECAR_PAIR_CODE the tool asks for the code on the terminal (and fails with a clear message if there is
  no terminal, e.g. in a script).
- after that it is remembered: the Pi hands out a client id and a secret, kept in ~/.config/sidecar/pair.json
  (0600; SIDECAR_PAIR_FILE moves it), one entry per host, and later runs authenticate with those and never send the
  code again. `pair=revoke` on the Pi, or deleting /.rytm/pair.txt from the card, makes it ask for the code once more.
- cmdline.txt paircode=0 on a card turns the gate off entirely (a dev card: the engine says so loudly at boot, on
  the STATUS page and in /status).
- the serial link is never gated: whoever has the serial header has the board, and it is the way back in if the
  network side is locked out.
- an engine from before 0.3.0 has no gate at all; this tool notices (it asks it to list the card first) and goes
  ahead unauthenticated with one line saying so, so upgrading a Pi still works.

Run as: uv run --with pyserial python pi/circle/sdboot.py ...
"""
import base64, binascii, glob, json, os, socket, stat, struct, sys, time

PORT = int(os.environ.get("SIDECAR_PORT") or 7310)   # 7310; an ssh tunnel or a test may move it
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ENGINE_ADDR = 0x02000000          # sdboot.patch's SDB_ADDR
BEGIN = "# sidecar sdboot begin (pi/circle/sdboot.py disable removes this block; config.txt.bak is the original)"
END = "# sidecar sdboot end"
def block(kernel="sdboot.img"):
    # disable_splash: no rainbow square; boot_delay=0: the firmware does not wait its default 1 s before the kernel.
    # [pi4], not [all]: sdboot and engine.img are the Pi 4's; a Pi 5 reading the same card (make-sd-dual.sh) skips
    # these lines and keeps its own [pi5] kernel=kernel_2712.img. On a Pi 4 [pi4] matches as [all] did.
    return (f"{BEGIN}\n[pi4]\nkernel={kernel}\ninitramfs engine.img 0x{ENGINE_ADDR:08x}\n"
            f"disable_splash=1\nboot_delay=0\n{END}\n")


def pair_file():
    return os.environ.get("SIDECAR_PAIR_FILE") or os.path.join(
        os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "sidecar", "pair.json")


def pairs_load():
    """{host: {"client": ..., "secret": ...}} - this Mac's own secret per Pi, never the pairing code."""
    try:
        with open(pair_file(), encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def pairs_save(host, client, secret):
    p = pair_file()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    d = pairs_load()
    d[host] = {"client": client, "secret": secret}
    tmp = p + ".new"                  # written, then put in place, as the Pi's own saves are
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, stat.S_IRUSR | stat.S_IWUSR)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=1, sort_keys=True)
    os.replace(tmp, p)
    os.chmod(p, stat.S_IRUSR | stat.S_IWUSR)
    return p


def ask_code():
    """The 8 digits, for a Sidecar that is asking for them. Only reached when the engine has a gate (see Pi.auth)."""
    code = os.environ.get("SIDECAR_PAIR_CODE")
    if code:
        return code.replace(" ", "").strip()
    if not sys.stdin.isatty():
        raise SystemExit("this Sidecar is asking for its pairing code and there is no terminal to ask you on.\n"
                         "Run it again with SIDECAR_PAIR_CODE=<the 8 digits>. They are on the Rytm's screen and on\n"
                         "the Sidecar's own screen (STATUS page, PAIR CODE), and they change at every boot; the\n"
                         "engine's console `pair=show` puts them on the Rytm again. A Sidecar with no display at all\n"
                         "writes them to /sidecar-paircode.txt at the card's root.\n"
                         'See docs/pi-engine.md "Pairing code".')
    return input("the pairing code on the Sidecar's screen (8 digits): ").replace(" ", "").strip()


class Pi:
    def __init__(self):
        host = os.environ.get("SIDECAR_HOST") or os.environ.get("PI_IP") or "sidecar.local"
        self.ser = None
        self.gated = True             # until the probe in auth() says otherwise
        if os.environ.get("SIDECAR_LINK") != "serial":
            try:
                self.s = socket.create_connection((host, PORT), timeout=3)
                self.s.settimeout(30)
                self.f = self.s.makefile("rb")
                print(f"link: tcp {host}:{PORT}", file=sys.stderr)
                self.auth(host)
                return
            except OSError as e:
                print(f"link: tcp {host}:{PORT} failed ({e}), the serial port", file=sys.stderr)
        import serial
        port = os.environ.get("SIDECAR_SERIAL") or (glob.glob("/dev/cu.usbserial-*") or [None])[0]
        if not port:
            raise SystemExit("no serial adapter")
        self.ser = serial.Serial(port, 921600, timeout=0.5, write_timeout=5)
        self.ser.reset_input_buffer()
        self.ser.write(b"\n:sdf\n")
        t = time.time()
        while time.time() - t < 5:
            l = self.ser.readline().decode(errors="replace").strip()
            if l.startswith("SDF READY"):
                print(f"link: serial {port}", file=sys.stderr)
                return
        raise SystemExit("the engine did not answer :sdf on the serial port (not running? an older build?)")

    def close(self):
        if self.ser:
            self.ser.write(b"BYE\n")
            t = time.time()
            while time.time() - t < 3:
                if self.ser.readline().startswith(b"SDF BYE"):
                    break

    def auth(self, host):
        """The pairing code's gate (pi/engine/paircode.h): AUTH with what this Mac was given, else PAIR with the code
        once. The serial link is not gated. Nothing is sent before this, so an unpaired run cannot touch the card.

        An engine from before the gate existed has to keep working, or every upgrade from a pre-0.3.0 kernel would
        stop here asking for a code that device does not have. LS is the probe, because both kinds answer it and
        neither closes the connection: a gated engine answers "ERR unauthenticated ..." to anything before PAIR/AUTH,
        an older one just lists the card. The decision is made on the gated answer, never on a version string, so the
        only way to skip authentication is for the engine to demonstrably not ask for it."""
        probe = self.cmd("LS")
        if not probe.startswith("ERR unauthenticated"):
            if probe.startswith("OK"):
                for _ in range(int(probe.split()[1])):    # its listing, so the next reply is ours
                    self.line()
            self.gated = False
            print(f"link: {host} is running firmware without a pairing code (before 0.3.0): going ahead unauthenticated",
                  file=sys.stderr)
            return
        rec = pairs_load().get(host) or {}
        if rec.get("client") and rec.get("secret"):
            r = self.cmd(f"AUTH {rec['client']} {rec['secret']}")
            if r == "OK":
                print("link: paired (remembered)", file=sys.stderr)
                return
            print(f"link: this Mac is no longer paired with {host} ({r}): asking for the code again", file=sys.stderr)
            time.sleep(1.2)           # the engine allows one pairing attempt a second
        code = ask_code()
        r = self.cmd(f"PAIR {code}")
        if not r.startswith("OK "):
            raise SystemExit(f"pairing refused: {r[4:] if r.startswith('ERR ') else r}\n"
                             "The code is on the Rytm's screen and on the Sidecar's STATUS page; it changes at every "
                             "boot. After several wrong ones the engine locks pairing for a while (it says how long).")
        parts = r.split()
        if len(parts) != 3:
            raise SystemExit(f"the engine's PAIR answer is not what this version expects: {r!r}")
        where = pairs_save(host, parts[1], parts[2])
        print(f"link: paired with {host}; it is remembered in {where} (the code is not stored)", file=sys.stderr)

    def send(self, b):
        if self.ser:
            self.ser.write(b)
        else:
            self.s.sendall(b)

    def line(self, timeout=30):
        if not self.ser:
            l = self.f.readline().decode(errors="replace").strip()
            if not l:
                raise SystemExit("the Pi closed the connection")
            return l
        t = time.time()
        while time.time() - t < timeout:      # the log's lines pass by; ours start "SDF "
            l = self.ser.readline().decode(errors="replace").strip()
            if l.startswith("SDF "):
                return l[4:]
        raise SystemExit("no answer on the serial port")

    def cmd(self, c, data=None):
        self.send(c.encode() + b"\n")
        if data is not None:
            if not self.ser:
                self.s.sendall(data)
            else:
                t0 = time.time()
                for i in range(0, len(data), 1024):
                    self.ser.write(b"D " + base64.b64encode(data[i:i + 1024]) + b"\n")
                    r = self.line(10)
                    if r != "K":
                        return r
                    if i % (64 * 1024) == 0 and len(data) > 64 * 1024:
                        print(f"  {i // 1024} / {len(data) // 1024} KB, {time.time() - t0:.0f} s", file=sys.stderr)
        return self.line()

    def ls(self):
        r = self.cmd("LS")
        if not r.startswith("OK"):
            raise SystemExit(r)
        return [self.line() for _ in range(int(r.split()[1]))]

    def get(self, name, missing_ok=False):
        r = self.cmd(f"GET {name}")
        if not r.startswith("OK"):
            if missing_ok:
                return None
            raise SystemExit(f"GET {name}: {r}")
        size, crc = int(r.split()[1]), int(r.split()[2], 16)
        b = b""
        while len(b) < size:
            if self.ser:
                l = self.line(10)
                if not l.startswith("D "):
                    raise SystemExit(f"GET {name}: unexpected {l!r}")
                b += base64.b64decode(l[2:])
                continue
            k = self.f.read(size - len(b))
            if not k:
                raise SystemExit("short read")
            b += k
        if binascii.crc32(b) != crc:
            raise SystemExit(f"GET {name}: crc mismatch")
        return b

    def put(self, name, data):
        r = self.cmd(f"PUT {name} {len(data)} {binascii.crc32(data):08x}", data)
        print(f"PUT {name} ({len(data)} bytes): {r}")
        if not r.startswith("OK"):
            raise SystemExit(1)


def wrap(img):
    img += b"\0" * (-len(img) % 4)
    s1 = s2 = 0
    for (w,) in struct.iter_unpack("<I", img):
        s1 = (s1 + w) & 0xFFFFFFFF
        s2 = (s2 + s1) & 0xFFFFFFFF
    return b"SDCRENG1" + struct.pack("<IIIIII", len(img), s1, s2, 0, 0, 0) + img


def without_block(cfg):
    out, skip = [], False
    for l in cfg.splitlines(keepends=True):
        if l.startswith(BEGIN):
            skip = True
        elif skip and l.startswith(END):
            skip = False
        elif not skip:
            out.append(l)
    return "".join(out)


def with_block(cfg, kernel="sdboot.img"):
    cfg = without_block(cfg)
    if cfg and not cfg.endswith("\n"):
        cfg += "\n"
    return cfg + block(kernel)


SDBOOT_MAX = 256 * 1024           # sdboot.img is about 4 KB; a kernel (2+ MB) as sdboot-try.img is pi5-try's


def board(img):
    """4 or 5: the board a Pi engine kernel image was built for (RASPPI); else SystemExit."""
    if b"==== rytm-pi engine" not in img:
        raise SystemExit("not a Pi engine kernel image (no '==== rytm-pi engine' in it)")
    return 5 if b"gadget: none on this board (Pi 5 build)" in img else 4


def sizes(pi):
    return {l.split(" ", 1)[1]: int(l.split(" ", 1)[0]) for l in pi.ls()}


def names(pi):
    return {l.split(" ", 1)[1] for l in pi.ls()}


def need(pi, names):
    have = {l.split(" ", 1)[1] for l in pi.ls()}
    miss = [n for n in names if n not in have]
    if miss:
        raise SystemExit(f"missing on the card: {', '.join(miss)} (sdboot.py install / bootloader + tryboot + commit first)")


def usbreset():
    import ctypes.util
    import usb.backend.libusb1, usb.core
    lib = next((p for p in ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib",
                            ctypes.util.find_library("usb-1.0")) if p and os.path.exists(p)), None)
    be = usb.backend.libusb1.get_backend(find_library=lambda _: lib)
    d = usb.core.find(idVendor=0x0403, idProduct=0x6001, backend=be)
    if d is None:
        raise SystemExit("no FT232R (0403:6001)")
    d.reset()
    time.sleep(2)
    print("usbreset: the FT232R was reset")


def main(a):
    if not a or a[0] in ("-h", "--help"):
        print(__doc__)
        return
    if a[0] == "usbreset":
        usbreset()
        return
    c, pi = a[0], Pi()
    try:
        run(c, a, pi)
    finally:
        pi.close()


def run(c, a, pi):
    if c == "ls":
        print("\n".join(pi.ls()))
    elif c == "get":
        b = pi.get(a[1])
        if len(a) > 2:
            open(a[2], "wb").write(b)
        else:
            sys.stdout.write(b.decode(errors="replace"))
    elif c == "scratch":
        print(pi.cmd("SCRATCH"))
    elif c == "install":
        imgs = [open(p, "rb").read() for p in (a[1:] or [os.path.join(REPO, "pi/engine/kernel8-rpi4.img")])]
        for img in imgs:
            board(img)              # all checked before anything is written
        for img in imgs:
            if board(img) == 5:
                pi.put("kernel_2712.img", img)
            else:
                pi.put("engine.img", wrap(img))
    elif c == "previous":
        pi.put("engine.img", pi.get("engine.img.prev"))
    elif c == "bootloader":
        p = a[1] if len(a) > 1 else os.path.join(HERE, "out/sdboot.img")
        pi.put("sdboot-try.img", open(p, "rb").read())
    elif c == "tryboot":
        sz = sizes(pi)
        k = "sdboot-try.img" if "sdboot-try.img" in sz else "sdboot.img"
        if sz.get(k, 0) > SDBOOT_MAX:
            raise SystemExit(f"{k} is {sz[k]} bytes: a Pi 5 kernel from pi5-try, not sdboot (pi5-commit it on the Pi 5)")
        need(pi, [k, "engine.img"])
        cfg = pi.get("config.txt").decode()
        pi.put("tryboot.txt", with_block(cfg, k).encode())
        print(f"tryboot with {k}:", pi.cmd("TRYBOOT"))
    elif c == "commit":
        need(pi, ["sdboot-try.img"])
        if sizes(pi)["sdboot-try.img"] > SDBOOT_MAX:
            raise SystemExit("sdboot-try.img is a Pi 5 kernel from pi5-try, not sdboot: on the Pi 5, pi5-commit")
        pi.put("sdboot.img", pi.get("sdboot-try.img"))
        print("DEL sdboot-try.img:", pi.cmd("DEL sdboot-try.img"))
    elif c == "enable":
        need(pi, ["sdboot.img", "engine.img"])
        cfg = pi.get("config.txt").decode()
        new = with_block(cfg)
        if new == cfg:
            print("config.txt already has the sdboot lines")
        else:
            pi.put("config.txt", new.encode())
    elif c == "disable":
        cfg = pi.get("config.txt").decode()
        new = without_block(cfg)
        if new == cfg:
            print("config.txt has no sdboot lines")
        else:
            pi.put("config.txt", new.encode())
    elif c == "restore-config":
        pi.put("config.txt", pi.get("config.txt.bak"))
    elif c == "cmdline":
        if len(a) > 1:
            pi.put("cmdline.txt", (" ".join(a[1:]).strip() + "\n").encode())
        else:
            b = pi.get("cmdline.txt", missing_ok=True)
            print(b.decode().strip() if b is not None else "(no cmdline.txt)")
    elif c == "reboot":
        print(pi.cmd("REBOOT"))
    elif c == "pi5-try":
        p = a[1] if len(a) > 1 else os.path.join(REPO, "pi/engine/kernel_2712.img")
        img = open(p, "rb").read()
        if board(img) != 5:
            raise SystemExit(f"{p} is a Pi 4 kernel: pi5-try takes a RASPPI=5 build (kernel_2712.img)")
        pi.put("sdboot-try.img", img)
        cfg = without_block(pi.get("config.txt").decode())
        pi.put("tryboot.txt", (cfg.rstrip("\n") + "\n[pi5]\nkernel=sdboot-try.img\n").encode())
        print("tryboot with sdboot-try.img:", pi.cmd("TRYBOOT"))
    elif c == "pi5-commit":
        need(pi, ["sdboot-try.img"])
        img = pi.get("sdboot-try.img")
        if board(img) != 5:
            raise SystemExit("sdboot-try.img is not a Pi 5 kernel (a Pi 4 sdboot from 'bootloader'?): not committed")
        pi.put("kernel_2712.img", img)
        print("DEL sdboot-try.img:", pi.cmd("DEL sdboot-try.img"))
    elif c == "pi5-previous":
        need(pi, ["kernel_2712.img.prev"])
        pi.put("kernel_2712.img", pi.get("kernel_2712.img.prev"))
    else:
        raise SystemExit(f"unknown command {c} (sdboot.py --help)")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Exception as e:
        if type(e).__name__ == "SerialTimeoutException":
            print("the serial adapter does not send (a flasher killed mid-upload?): sdboot.py usbreset", file=sys.stderr)
            os._exit(2)             # a close would wait for the stuck output
        raise
