#!/usr/bin/env python3
"""pi/engine/tests/test_sdboot.py - pi/circle/sdboot.py against stand-in engines on the SD file port (sdfiles.h).

The pairing code (pi/engine/paircode.h) gates port 7310, so this tool has to handle three engines: one that asks for
the code, one that already knows this Mac, and one from before the gate existed. That last one is why this test
exists: the first version of the gated client stopped dead against a pre-0.3.0 kernel with "run it again with
SIDECAR_PAIR_CODE=..." - a code that device does not have - which is the worst failure mode there is, because it
reads as the user's mistake. Found on hardware 2026-10-25, during the first install.

    uv run python pi/engine/tests/test_sdboot.py        (or: python3 pi/engine/tests/test_sdboot.py)
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SDBOOT = os.path.join(HERE, "..", "..", "circle", "sdboot.py")
CODE = "13572468"


class Engine(threading.Thread):
    """One connection at a time, line commands, as CSdFiles::Serve does. gated=False is a pre-0.3.0 engine."""

    def __init__(self, gated=True):
        super().__init__(daemon=True)
        self.gated = gated
        self.seen = []                  # every line it was sent, for the assertions
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            try:
                self.serve(c)
            except OSError:
                pass
            c.close()

    def serve(self, c):
        f = c.makefile("rwb")
        authed = not self.gated
        while True:
            line = f.readline().decode(errors="replace").strip()
            if not line:
                return
            self.seen.append(line)
            if line == "BYE":
                return
            if not authed:
                if self.gated:
                    # paircode.h's tcp_auth: PAIR, AUTH or nothing, and the connection stays open either way
                    if line.startswith("PAIR "):
                        if line[5:] == CODE:
                            authed = True
                            f.write(b"OK 0123abcd " + b"f" * 32 + b"\n")
                        else:
                            f.write(b"ERR that is not the pairing code on the Sidecar's screen\n")
                    elif line.startswith("AUTH "):
                        parts = line.split()
                        if len(parts) == 3 and parts[1] == "0123abcd" and parts[2] == "f" * 32:
                            authed = True
                            f.write(b"OK\n")
                        else:
                            f.write(b"ERR that is not the pairing code on the Sidecar's screen\n")
                    else:
                        f.write(b"ERR unauthenticated: PAIR <code> with the code on the Sidecar's screen, or AUTH "
                                b"<id> <secret> (sdboot.py does this: SIDECAR_PAIR_CODE=<code>)\n")
                    f.flush()
                    continue
            if line == "LS":
                f.write(b"OK 2\n12 config.txt\n34 engine.img\n")
                f.flush()
                continue
            # a pre-0.3.0 engine drops the connection after an unknown command; so does this one
            f.write(b"ERR unknown command\n")
            f.flush()
            return


def run_sdboot(port, args=("ls",), env=None, store=None):
    e = dict(os.environ)
    e.pop("SIDECAR_PAIR_CODE", None)
    e.update(SIDECAR_HOST="127.0.0.1", SIDECAR_PORT=str(port), SIDECAR_LINK="tcp", SIDECAR_PAIR_FILE=store or "")
    e.update(env or {})
    return subprocess.run([sys.executable, SDBOOT, *args], capture_output=True, text=True, timeout=30,
                          stdin=subprocess.DEVNULL, env=e)


class TestSdboot(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sdboot-test-")
        self.store = os.path.join(self.tmp, "pair.json")

    def engine(self, gated=True):
        e = Engine(gated)
        e.start()
        self.addCleanup(e.srv.close)
        return e

    def test_legacy_engine_needs_no_code(self):
        """A kernel from before the gate: it must just work, with one line saying why, and never ask for a code."""
        e = self.engine(gated=False)
        r = run_sdboot(e.port, store=self.store)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("config.txt", r.stdout)
        self.assertIn("without a pairing code", r.stderr)
        self.assertNotIn("SIDECAR_PAIR_CODE", r.stderr)      # never sent looking for a code it does not have
        self.assertNotIn("PAIR", "".join(e.seen))            # and nothing was offered to it
        self.assertFalse(os.path.exists(self.store))         # nothing to remember

    def test_gated_engine_pairs_once_then_remembers(self):
        e = self.engine()
        r = run_sdboot(e.port, env={"SIDECAR_PAIR_CODE": CODE}, store=self.store)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("config.txt", r.stdout)
        self.assertIn("paired with 127.0.0.1", r.stderr)
        self.assertNotIn(CODE, r.stderr + r.stdout)          # the code is never echoed back
        with open(self.store) as f:
            saved = json.load(f)
        self.assertEqual(saved["127.0.0.1"], {"client": "0123abcd", "secret": "f" * 32})
        self.assertEqual(oct(os.stat(self.store).st_mode & 0o777), oct(0o600))
        # the second run authenticates with what it was given and never sends the code again
        e.seen.clear()
        r = run_sdboot(e.port, store=self.store)             # no SIDECAR_PAIR_CODE in the environment
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("paired (remembered)", r.stderr)
        self.assertIn("AUTH 0123abcd", "\n".join(e.seen))
        self.assertNotIn("PAIR ", "\n".join(e.seen))

    def test_gated_engine_without_a_code_says_so_clearly(self):
        """It must say the device is asking, not that the Mac did something wrong, and say where to read the code."""
        e = self.engine()
        r = run_sdboot(e.port, store=self.store)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("asking for its pairing code", r.stderr)
        self.assertIn("SIDECAR_PAIR_CODE", r.stderr)
        self.assertIn("STATUS page", r.stderr)
        self.assertIn("Rytm", r.stderr)

    def test_wrong_code_is_refused_and_nothing_is_remembered(self):
        e = self.engine()
        r = run_sdboot(e.port, env={"SIDECAR_PAIR_CODE": "00000000"}, store=self.store)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("pairing refused", r.stderr)
        self.assertIn("changes at every boot", r.stderr)
        self.assertFalse(os.path.exists(self.store))

    def test_a_stale_secret_falls_back_to_the_code(self):
        e = self.engine()
        os.makedirs(os.path.dirname(self.store), exist_ok=True)
        with open(self.store, "w") as f:
            json.dump({"127.0.0.1": {"client": "deadbeef", "secret": "0" * 32}}, f)
        r = run_sdboot(e.port, env={"SIDECAR_PAIR_CODE": CODE}, store=self.store)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("no longer paired", r.stderr)
        with open(self.store) as f:
            self.assertEqual(json.load(f)["127.0.0.1"]["client"], "0123abcd")


if __name__ == "__main__":
    unittest.main(verbosity=2)
