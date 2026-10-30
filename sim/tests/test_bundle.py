# sim/tests/test_bundle.py - an independent reader for the bundles core/bundle writes (docs/BUNDLES.md): the core's
# test (core/tests/test_bundle.cpp) leaves its bundles in BUNDLE_OUT; Python's zipfile and hashlib open them and
# tools/bundle.py checks every rule. python3 -m unittest sim/tests/test_bundle.py (make -C core engines first).
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bundle  # noqa: E402  (tools/bundle.py)


class Bundles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="bundle-py-")
        subprocess.run(["make", "-C", os.path.join(ROOT, "core"), "build/test_bundle"], check=True, capture_output=True)
        env = dict(os.environ, BUNDLE_OUT=cls.out)
        r = subprocess.run([os.path.join(ROOT, "core", "build", "test_bundle")], env=env, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-2000:]
        cls.files = sorted(os.path.join(cls.out, f) for f in os.listdir(cls.out) if f.endswith(".sidecar.zip"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def test_bundles_made(self):
        self.assertEqual([os.path.basename(f) for f in self.files], ["ar-perc.sidecar.zip", "live-set.sidecar.zip"])

    def test_tool_verifies(self):
        for f in self.files:
            self.assertEqual(bundle.verify(f), [], f)

    def test_zipfile_and_hashes(self):
        for f in self.files:
            with zipfile.ZipFile(f) as z:
                self.assertIsNone(z.testzip())
                m = json.loads(z.read("manifest.json"))
                self.assertEqual(z.infolist()[0].filename, "manifest.json")
                for e in m["entries"]:
                    b = z.read(e["name"])
                    self.assertEqual(len(b), e["size"])
                    self.assertEqual(hashlib.sha256(b).hexdigest(), e["sha256"], e["name"])
                self.assertEqual(set(z.namelist()) - {"manifest.json"}, {e["name"] for e in m["entries"]})

    def test_contents(self):
        with zipfile.ZipFile(self.files[0]) as z:
            m = json.loads(z.read("manifest.json"))
        kinds = {e["name"]: e["kind"] for e in m["entries"]}
        for n in ("project/samples.pool.txt", "project/wavetables.pool.txt", "project/instruments.pool.txt", "project/pool-used.txt"):
            self.assertEqual(kinds[n], "pool")
        self.assertNotIn("project/pool.txt", kinds)   # (the old per-machine pools: not carried)
        self.assertEqual(kinds["files/MULTISAMPLE/Piano/Piano.sfz"], "sfz")
        self.assertEqual(kinds["plugins/sc.test-1.0.0.a64.rmp"], "package")
        self.assertEqual(m["rytm_file"]["name"], "rytm/AR PERC.arprj")
        self.assertIn("/Samples/Drums/gone.wav", m["missing"])
        self.assertNotIn("pools", m)
        self.assertNotIn("bank_a", m)
        self.assertEqual({k: m["formats"][k] for k in ("samplepool", "wtpool", "instpool", "poolused")},
                         {"samplepool": 1, "wtpool": 1, "instpool": 1, "poolused": 1})
        fm = {e["name"]: e["format"]["name"] for e in m["entries"] if "format" in e}
        self.assertEqual(fm["project/samples.pool.txt"], "samplepool")
        self.assertEqual(fm["project/instruments.pool.txt"], "instpool")
        members = {e["name"] for e in m["entries"] if e["kind"] == "sample"}
        self.assertTrue({"files/MULTISAMPLE/Piano/samples/c4.wav", "files/Samples/Abs/d4.wav"} <= members)

    def test_damage_found(self):
        with open(self.files[1], "rb") as f:
            raw = bytearray(f.read())
        with zipfile.ZipFile(self.files[1]) as z:
            i = z.getinfo("project/samples.pool.txt")
        import struct
        nl, xl = struct.unpack("<HH", bytes(raw[i.header_offset + 26:i.header_offset + 30]))
        raw[i.header_offset + 30 + nl + xl] ^= 1   # the first byte of its data
        p = os.path.join(self.out, "damaged.zip")
        with open(p, "wb") as f:
            f.write(bytes(raw))
        self.assertTrue(bundle.verify(p))

    def test_extract_stays_inside(self):
        d = tempfile.mkdtemp(prefix="bundle-x-")
        bundle.extract(self.files[1], d)
        self.assertTrue(os.path.exists(os.path.join(d, "project", "samples.pool.txt")))
        evil = os.path.join(self.out, "evil.zip")
        with zipfile.ZipFile(evil, "w") as z:
            z.writestr("../escape.txt", "x")
        with self.assertRaises(SystemExit):
            bundle.extract(evil, d)
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
