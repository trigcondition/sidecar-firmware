# tools/tests/test_rytm_bridge.py - the Mac's relay for the Sidecar's Rytm MIDI bridge (tools/rytm_bridge.py) against a
# fake Sidecar that speaks midibridge_pi.cpp's challenge: the answer is the HMAC and a replayed one fails, nothing of
# the handshake reaches the tool, the bytes go through raw both ways, a wrong token relays nothing, an HTTP request and a
# silent client are refused before they reach the Sidecar, and one tool at a time keeps the bridge.
#   python3 tools/tests/test_rytm_bridge.py
import hashlib
import hmac
import os
import secrets
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import rytm_bridge  # noqa: E402

TOKEN = "0123456789abcdef0123456789abcdef"


class FakePi:
    """The Sidecar's port 7330 as midibridge_pi.cpp serves it: a nonce, the HMAC answer, OK or ERR, then raw bytes."""

    def __init__(self, token=TOKEN, greeting=b"", fixed_nonce=None):
        self.token, self.greeting, self.fixed_nonce = token, greeting, fixed_nonce
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(8)
        self.port = self.srv.getsockname()[1]
        self.hellos, self.received, self.conns, self.accepts = [], bytearray(), [], 0
        self.cond = threading.Condition()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            with self.cond:
                self.accepts += 1
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    def _one(self, c):
        nonce = self.fixed_nonce or secrets.token_hex(16)
        c.sendall(("SIDECAR-MIDIBRIDGE 1 %s\n" % nonce).encode())
        line = b""
        while not line.endswith(b"\n"):
            b = c.recv(1)
            if not b:
                return
            line += b
        with self.cond:
            self.hellos.append(line)
        want = "BRIDGE " + hmac.new(self.token.encode(), nonce.encode(), hashlib.sha256).hexdigest() + "\n"
        if line != want.encode():
            c.sendall(b"ERR wrong answer\n")
            c.close()
            return
        c.sendall(b"OK sidecar-midibridge\n" + self.greeting)
        with self.cond:
            self.conns.append(c)
            self.cond.notify_all()
        try:
            while True:
                d = c.recv(4096)
                if not d:
                    return
                with self.cond:
                    self.received += d
                    self.cond.notify_all()
        except OSError:
            return

    def wait(self, pred, timeout=3.0):
        end = time.time() + timeout
        with self.cond:
            while not pred():
                left = end - time.time()
                if left <= 0:
                    return False
                self.cond.wait(left)
            return True

    def close(self):
        self.srv.close()
        for c in self.conns:
            try:
                c.close()
            except OSError:
                pass


def recv_exact(s, n, timeout=3.0):
    s.settimeout(timeout)
    out = b""
    while len(out) < n:
        d = s.recv(n - len(out))
        if not d:
            break
        out += d
    return out


def at_eof(s, timeout=3.0):
    s.settimeout(timeout)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                return True
    except (ConnectionResetError, OSError):
        return True


class RelayTest(unittest.TestCase):
    def start(self, pi, token=TOKEN):
        logs = []
        r = rytm_bridge.Relay("127.0.0.1", pi.port, token, "127.0.0.1:0", log=logs.append)
        threading.Thread(target=r.serve_forever, daemon=True).start()
        self.addCleanup(r.close)
        self.addCleanup(pi.close)
        return r, logs

    def tool(self, r):
        s = socket.create_connection(r.address, timeout=3)
        self.addCleanup(s.close)
        return s

    def test_raw_both_ways(self):
        pi = FakePi(greeting=bytes([0xF0, 0x7D, 0x52, 0x54, 0x1D, 0xF7, 0x90, 0x24, 0x64]))
        r, _ = self.start(pi)
        t = self.tool(r)
        t.sendall(b"\xfe")                           # a tool that speaks first (active sensing)
        # the tool's first bytes are the Rytm's MIDI: no challenge, no "OK" line, no framing
        self.assertEqual(recv_exact(t, 9), bytes([0xF0, 0x7D, 0x52, 0x54, 0x1D, 0xF7, 0x90, 0x24, 0x64]))
        sysex = bytes([0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00]) + bytes(range(0x00, 0x80)) * 300 + bytes([0xF7])
        t.sendall(sysex)
        self.assertTrue(pi.wait(lambda: len(pi.received) >= 1 + len(sysex)))
        self.assertEqual(bytes(pi.received), b"\xfe" + sysex)
        self.assertEqual(len(pi.hellos), 1)
        pi.conns[0].sendall(b"\xf8\xfa\xb0\x10\x20")
        self.assertEqual(recv_exact(t, 5), b"\xf8\xfa\xb0\x10\x20")

    def test_silent_client_dropped(self):
        # R2-L3: a client that connects and sends nothing (a browser's preconnect) is closed after FIRST_BYTE_WAIT and
        # never takes the slot: a real tool connecting meanwhile gets the bridge at once
        pi = FakePi()
        r, logs = self.start(pi)
        silent = self.tool(r)
        t = self.tool(r)
        t.sendall(b"\xfe")
        self.assertTrue(pi.wait(lambda: len(pi.conns) == 1, timeout=0.8))   # before the silent one's time is up
        self.assertTrue(at_eof(silent, timeout=rytm_bridge.FIRST_BYTE_WAIT + 3))
        self.assertEqual(pi.accepts, 1)
        self.assertTrue(any("sent nothing" in m for m in logs), logs)

    def test_passive_lets_a_reading_tool_through(self):
        # --passive: a tool that connects and only reads (a monitor) gets the bridge after FIRST_BYTE_WAIT; text from
        # it later is still refused before it reaches the Sidecar
        pi = FakePi(greeting=b"\xf8")
        logs = []
        r = rytm_bridge.Relay("127.0.0.1", pi.port, TOKEN, "127.0.0.1:0", log=logs.append, passive=True)
        threading.Thread(target=r.serve_forever, daemon=True).start()
        self.addCleanup(r.close)
        self.addCleanup(pi.close)
        t = self.tool(r)
        self.assertEqual(recv_exact(t, 1, timeout=rytm_bridge.FIRST_BYTE_WAIT + 3), b"\xf8")
        t.sendall(b"GET / HTTP/1.1\r\n\r\n")
        self.assertTrue(at_eof(t))
        self.assertEqual(bytes(pi.received), b"")
        # and a MIDI-speaking one that reads first works too
        t2 = self.tool(r)
        self.assertEqual(recv_exact(t2, 1, timeout=rytm_bridge.FIRST_BYTE_WAIT + 3), b"\xf8")
        t2.sendall(b"\x90\x24\x64")
        self.assertTrue(pi.wait(lambda: bytes(pi.received) == b"\x90\x24\x64"))

    def test_http_post_refused(self):
        # H2: a web page's fetch("http://127.0.0.1:7321/", {method: "POST", body: sysex}) - never reaches the Sidecar
        pi = FakePi()
        r, logs = self.start(pi)
        t = self.tool(r)
        t.sendall(b"POST / HTTP/1.1\r\nHost: 127.0.0.1:7321\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\n"
                  b"\xf0\x00\x20\x3c\x10\x00\x01\xf7")
        self.assertTrue(at_eof(t))
        time.sleep(0.2)
        self.assertEqual(pi.accepts, 0)
        self.assertEqual(bytes(pi.received), b"")
        self.assertTrue(any("not sending MIDI" in m for m in logs), logs)

    def test_wrong_token_relays_nothing(self):
        pi = FakePi(token="f" * 32)
        r, logs = self.start(pi)
        t = self.tool(r)
        t.sendall(b"\x90\x24\x64")
        self.assertTrue(at_eof(t))
        self.assertEqual(bytes(pi.received), b"")
        self.assertTrue(any("wrong answer" in m for m in logs), logs)

    def test_one_tool_at_a_time(self):
        pi = FakePi()
        r, logs = self.start(pi)
        a = self.tool(r)
        a.sendall(b"\xfe")
        self.assertTrue(pi.wait(lambda: len(pi.conns) == 1))
        b = self.tool(r)
        b.sendall(b"\xfe")
        self.assertTrue(at_eof(b))                   # the second is closed at once, and never reaches the Sidecar
        self.assertEqual(pi.accepts, 1)
        a.sendall(b"\xfc")
        self.assertTrue(pi.wait(lambda: pi.received.endswith(b"\xfe\xfc")))
        a.close()                                    # the first one goes: the next tool gets it
        time.sleep(0.2)
        c = self.tool(r)
        c.sendall(b"\xfa")
        self.assertTrue(pi.wait(lambda: pi.received.endswith(b"\xfa")))
        self.assertEqual(pi.accepts, 2)

    def test_two_tools_racing(self):
        # many tools at once: exactly one gets through
        pi = FakePi()
        r, _ = self.start(pi)
        socks = [self.tool(r) for _ in range(8)]
        for s in socks:                             # all with a MIDI first byte
            try:
                s.sendall(b"\xfe")
            except OSError:
                pass
        self.assertTrue(pi.wait(lambda: len(pi.conns) == 1))
        time.sleep(0.3)
        self.assertEqual(pi.accepts, 1)

    def test_pi_closes(self):
        pi = FakePi()
        r, _ = self.start(pi)
        t = self.tool(r)
        t.sendall(b"\xfe")
        self.assertTrue(pi.wait(lambda: len(pi.conns) == 1))
        pi.conns[0].shutdown(socket.SHUT_RDWR)
        self.assertTrue(at_eof(t))

    def test_handshake(self):
        pi = FakePi(token="e" * 32)
        self.addCleanup(pi.close)
        with self.assertRaises(rytm_bridge.HandshakeError) as e:
            rytm_bridge.connect_pi("127.0.0.1", pi.port, TOKEN)
        self.assertIn("wrong answer", str(e.exception))
        rytm_bridge.connect_pi("127.0.0.1", pi.port, "e" * 32).close()
        # M1: the answer depends on the nonce, so a sniffed one is no good for the next connection
        self.assertNotEqual(rytm_bridge.answer(TOKEN, "a" * 32), rytm_bridge.answer(TOKEN, "b" * 32))
        # the same HMAC as midibridge.h's (test_midibridge.cpp checks this value)
        self.assertEqual(rytm_bridge.answer(TOKEN, "00112233445566778899aabbccddeeff"),
                         "BRIDGE " + hmac.new(TOKEN.encode(), b"00112233445566778899aabbccddeeff",
                                              hashlib.sha256).hexdigest())

    def test_token_sources(self):
        with self.assertRaises(SystemExit):
            rytm_bridge.main(["--pi", "127.0.0.1"], env={"SIDECAR_BRIDGE_TOKEN": "nothex"})
        with self.assertRaises(SystemExit):                  # no --token: it would be in `ps`
            rytm_bridge.main(["--pi", "127.0.0.1", "--token", TOKEN], env={})
        d = tempfile.mkdtemp()
        p = os.path.join(d, "tok")
        with open(p, "w") as f:
            f.write(TOKEN + "\n")
        os.chmod(p, 0o644)
        with self.assertRaises(ValueError):
            rytm_bridge.read_token(p)
        os.chmod(p, 0o600)
        self.assertEqual(rytm_bridge.read_token(p), TOKEN)
        self.assertEqual(rytm_bridge.read_token(None, {"SIDECAR_BRIDGE_TOKEN": TOKEN.upper()}), TOKEN)
        pi = FakePi(token="d" * 32)
        self.addCleanup(pi.close)
        self.assertEqual(rytm_bridge.main(["--pi", "127.0.0.1:%d" % pi.port, "--token-file", p, "--listen", "127.0.0.1:0"]), 1)

    def test_addresses(self):
        self.assertEqual(rytm_bridge.split_addr("sidecar.local", 7330), ("sidecar.local", 7330))
        self.assertEqual(rytm_bridge.split_addr("10.0.0.2:7331", 7330), ("10.0.0.2", 7331))
        self.assertEqual(rytm_bridge.split_addr("::1", 7321), ("::1", 7321))
        self.assertEqual(rytm_bridge.split_addr("[::1]:7400", 7321), ("::1", 7400))
        if socket.has_ipv6:
            pi = FakePi()
            self.addCleanup(pi.close)
            try:
                r = rytm_bridge.Relay("127.0.0.1", pi.port, TOKEN, "[::1]:0", log=lambda m: None)
            except OSError:
                self.skipTest("no IPv6 loopback here")
            self.addCleanup(r.close)
            self.assertEqual(r.address[0], "::1")


if __name__ == "__main__":
    unittest.main()
