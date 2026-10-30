#!/usr/bin/env python3
"""The Mac's end of the Sidecar's Rytm MIDI bridge (pi/engine/midibridge.h, docs/HARDWARE-TESTING.md).

The Sidecar carries the Rytm's USB MIDI on TCP port 7330 after a challenge: it sends
"SIDECAR-MIDIBRIDGE 1 <nonce>", the client answers "BRIDGE <HMAC-SHA256(token, nonce)>" and gets "OK ..." or
"ERR ...". Test tools that speak analog-emu's --usb-midi-tcp (a raw MIDI byte stream with no preamble: analogrun,
analogusb.py, elkflash, elkbench, all with --port tcp:HOST:PORT) cannot do that, so this relay listens on
127.0.0.1:7321, answers the challenge for the tool that connects, and then copies the raw bytes both ways.

    export SIDECAR_BRIDGE_TOKEN=0123...cdef          # or --token-file FILE (0600)
    python3 tools/rytm_bridge.py --pi sidecar.local
    analogrun.py --port tcp:127.0.0.1:7321 ...

Local guards, since anything on this computer - and any web page in its browser - can reach 127.0.0.1:
  * a raw MIDI stream starts with a status byte: a client is handed the bridge only once its first byte is one
    (>= 0x80); one whose first byte is below 0x80 (every HTTP request starts with a letter) or that sends nothing for
    FIRST_BYTE_WAIT is closed, and nothing of it reaches the Sidecar;
  * one tool at a time: the first MIDI-speaking one keeps the bridge until it disconnects; another is closed.
--passive (opt in) also lets through a tool that connects and only reads at first (a MIDI monitor): after
FIRST_BYTE_WAIT it gets the bridge, and its first byte is still checked when it comes - but then a silent client (a
browser's preconnect, a stray process) can hold the one slot until it closes.
The token comes from SIDECAR_BRIDGE_TOKEN or --token-file, never the command line (`ps` would show it). It is new each
time the bridge is turned on, on the Sidecar's web page (Status > Rytm MIDI bridge). Standard library only.
"""
import argparse
import hashlib
import hmac
import os
import socket
import stat
import sys
import threading

PI_PORT = 7330
LISTEN = "127.0.0.1:7321"
HELLO_TIMEOUT = 5.0
FIRST_BYTE_WAIT = 1.0      # a client must send its first MIDI byte within this, or it is closed


class HandshakeError(Exception):
    pass


def split_addr(s, default_port):
    """HOST, HOST:PORT, [V6]:PORT or a bare IPv6 address -> (host, port)."""
    if s.startswith("["):
        host, _, rest = s[1:].partition("]")
        return host, int(rest[1:]) if rest.startswith(":") and rest[1:].isdigit() else default_port
    if s.count(":") > 1:                      # a bare IPv6 address
        return s, default_port
    host, sep, port = s.rpartition(":")
    if not sep or not port.isdigit():
        return s, default_port
    return host or "127.0.0.1", int(port)


def is_token(t):
    return len(t) == 32 and all(c in "0123456789abcdef" for c in t)


def read_token(path=None, env=None):
    """The token from the file (which must not be readable by others) or SIDECAR_BRIDGE_TOKEN; '' none."""
    env = os.environ if env is None else env
    if path:
        st = os.stat(path)
        if st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            raise ValueError("%s is readable by others: chmod 600 it" % path)
        with open(path) as f:
            return f.read().strip().lower()
    return env.get("SIDECAR_BRIDGE_TOKEN", "").strip().lower()


def answer(token, nonce):
    return "BRIDGE " + hmac.new(token.encode("ascii"), nonce.encode("ascii"), hashlib.sha256).hexdigest()


def _line(s, limit=512):
    line = b""
    while not line.endswith(b"\n"):
        b = s.recv(1)
        if not b:
            break
        line += b
        if len(line) > limit:
            break
    return line.decode("ascii", "replace").strip()


def connect_pi(host, port, token, timeout=HELLO_TIMEOUT):
    """A connected, authenticated socket to the Sidecar's bridge, or HandshakeError with its answer."""
    try:
        s = socket.create_connection((host, port), timeout=timeout)
    except OSError as e:
        raise HandshakeError("cannot connect to %s:%d: %s" % (host, port, e))
    try:
        greet = _line(s)
        parts = greet.split()
        if len(parts) != 3 or parts[0] != "SIDECAR-MIDIBRIDGE" or parts[1] != "1":
            raise HandshakeError(greet or "the Sidecar closed the connection")
        s.sendall((answer(token, parts[2]) + "\n").encode("ascii"))
        text = _line(s)
    except OSError as e:
        s.close()
        raise HandshakeError("the Sidecar did not answer: %s" % e)
    except HandshakeError:
        s.close()
        raise
    if not text.startswith("OK"):
        s.close()
        raise HandshakeError(text or "the Sidecar closed the connection")
    s.settimeout(None)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def pump(src, dst, stop, check_first=False, log=None):
    """src -> dst until either end closes; then both are shut down. check_first: refuse a first byte below 0x80."""
    first = check_first
    try:
        while not stop.is_set():
            data = src.recv(65536)
            if not data:
                break
            if first:
                first = False
                if data[0] < 0x80:
                    if log:
                        log("rytm_bridge: refused a client that is not sending MIDI (an HTTP request?)")
                    break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        stop.set()
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Relay:
    def __init__(self, pi_host, pi_port, token, listen=LISTEN, log=None, passive=False):
        self.pi_host, self.pi_port, self.token = pi_host, pi_port, token
        self.passive = passive       # --passive: a client that sends nothing at first still gets the bridge
        self.log = log or (lambda m: print(m, file=sys.stderr, flush=True))
        host, port = split_addr(listen, 7321)
        fam = socket.AF_INET6 if ":" in host else socket.AF_INET
        self.srv = socket.socket(fam, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((host, port))
        self.srv.listen(4)
        self.address = tuple(self.srv.getsockname()[:2])
        self.lock = threading.Lock()
        self.busy = False            # a tool holds the bridge (from its first MIDI byte until it disconnects)
        self.current = None          # (tool socket, pi socket, stop event)
        self.closed = False

    def _end(self, tool, pi=None, mine=False):
        for s in (tool, pi):
            if s is None:
                continue
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()
        if mine:
            with self.lock:
                self.busy = False
                self.current = None

    def handle(self, tool):
        """One tool, from its accept to its end. It gets the bridge only with a first byte that is MIDI, and only if no
        other tool has it: a silent client (a browser's preconnect, a stray process) never takes the slot."""
        pi = None
        mine = False
        try:
            tool.settimeout(FIRST_BYTE_WAIT)
            try:
                first = tool.recv(1, socket.MSG_PEEK)
            except socket.timeout:
                if not self.passive:
                    self.log("rytm_bridge: dropped a client that sent nothing for %.0f s (--passive lets a tool that "
                             "reads first through)" % FIRST_BYTE_WAIT)
                    return
                first = None             # --passive: it gets the bridge; its first byte is still checked in the pump
            if first is not None and not first:
                return
            if first and first[0] < 0x80:
                self.log("rytm_bridge: refused a client that is not sending MIDI (an HTTP request?)")
                return
            with self.lock:              # the slot: the first MIDI-speaking tool keeps it until it disconnects
                if self.busy:
                    self.log("rytm_bridge: refused a second client (one tool at a time)")
                    return
                self.busy = mine = True
            tool.settimeout(None)
            try:
                pi = connect_pi(self.pi_host, self.pi_port, self.token)
            except HandshakeError as e:
                self.log("rytm_bridge: the Sidecar refused: %s" % e)
                return
            tool.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            stop = threading.Event()
            with self.lock:
                self.current = (tool, pi, stop)
            self.log("rytm_bridge: relaying a tool <-> %s:%d" % (self.pi_host, self.pi_port))
            b = threading.Thread(target=pump, args=(pi, tool, stop), daemon=True)
            b.start()
            pump(tool, pi, stop, check_first=True, log=self.log)
            b.join(5)
        except OSError:
            pass
        finally:
            self._end(tool, pi, mine)

    def serve_forever(self):
        while not self.closed:
            try:
                tool, _ = self.srv.accept()
            except OSError:
                break
            threading.Thread(target=self.handle, args=(tool,), daemon=True).start()

    def close(self):
        self.closed = True
        try:
            self.srv.close()
        except OSError:
            pass
        with self.lock:
            cur = self.current
        if cur:
            cur[2].set()
            for s in cur[:2]:
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass


def main(argv=None, env=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], allow_abbrev=False)
    ap.add_argument("--pi", default="sidecar.local", help="the Sidecar: HOST or HOST:PORT (port %d)" % PI_PORT)
    ap.add_argument("--token-file", help="a file (mode 600) holding the token; else SIDECAR_BRIDGE_TOKEN")
    ap.add_argument("--listen", default=LISTEN, help="where the tools connect (default %s)" % LISTEN)
    ap.add_argument("--passive", action="store_true",
                    help="also let through a tool that connects and reads before it sends (a MIDI monitor); a silent "
                         "client then holds the one slot until it closes")
    a = ap.parse_args(argv)
    try:
        token = read_token(a.token_file, env)
    except (OSError, ValueError) as e:
        ap.error("--token-file: %s" % e)
    if not is_token(token):
        ap.error("the token: the 32 hex digits on the Sidecar's web page (Status > Rytm MIDI bridge), in "
                 "SIDECAR_BRIDGE_TOKEN or --token-file")
    host, port = split_addr(a.pi, PI_PORT)
    lhost, _ = split_addr(a.listen, 7321)
    if lhost not in ("127.0.0.1", "localhost", "::1"):
        print("rytm_bridge: WARNING: listening on %s - anyone who reaches it gets the Rytm's MIDI without the token"
              % a.listen, file=sys.stderr)
    # the token is checked once now, so a wrong one is said at the start and not at the first tool's connect
    try:
        connect_pi(host, port, token).close()
    except HandshakeError as e:
        print("rytm_bridge: %s" % e, file=sys.stderr)
        return 1
    r = Relay(host, port, token, a.listen, passive=a.passive)
    print("rytm_bridge: the Rytm's USB MIDI at tcp:%s:%d (the Sidecar %s:%d); Ctrl-C ends it" % (r.address + (host, port)),
          file=sys.stderr, flush=True)
    try:
        r.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
