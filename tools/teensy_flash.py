#!/usr/bin/env python3
"""Write a Teensy relay build through the Sidecar Pi (docs/teensy-flash.md), so the Teensy's micro-USB stays on the Pi.

Usage: python tools/teensy_flash.py <file.hex> --host 169.254.61.60 (--code <pairing code> | --token <token>)
                                    [--expect <build>] [--upload-only]

Pairs with POST /pair (a session token for this run only; the code and the token are never printed or saved), uploads the
.hex in 48 KB chunks to POST /teensy/put (off=, total=, crc=: the Pi parses and checks it as it comes), starts the write
with POST /teensy/flash (expect=: the build the relay must name when it is back; build.ps1's stamp, the git short hash
with "+" for a changed tree), then follows GET /teensy/state until it is done or failed. Exit 0 only when done.
"""
import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request
import uuid
import zlib

CHUNK = 48 * 1024


def pair(host, code):
    body = urllib.parse.urlencode({"code": code, "name": "teensy-flash", "remember": "0"}).encode()
    req = urllib.request.Request(f"http://{host}/pair", data=body, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=10) as r:
        ans = json.loads(r.read().decode("utf-8", "replace"))
    if not ans.get("ok"):
        sys.exit(f"pairing refused: {ans.get('error', '?')} (wait {ans.get('wait', 0)} s)")
    return ans["token"]


def call(host, token, path, data=None, ctype=None, timeout=20):
    headers = {"X-Sidecar-Auth": token}
    if ctype:
        headers["Content-Type"] = ctype
    req = urllib.request.Request(f"http://{host}{path}", data=data, method="POST" if data is not None else "GET",
                                 headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def put(host, token, hexdata):
    total, crc = len(hexdata), zlib.crc32(hexdata) & 0xFFFFFFFF
    off = 0
    while off < total:
        chunk = hexdata[off:off + CHUNK]
        b = uuid.uuid4().hex
        body = (f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"relay.hex\"\r\n"
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + chunk + f"\r\n--{b}--\r\n".encode()
        ans = call(host, token, f"/teensy/put?off={off}&total={total}&crc={crc}", body,
                   f"multipart/form-data; boundary={b}").strip()
        code, _, msg = ans.partition(" ")
        if code != "200":
            sys.exit(f"upload refused at {off}: {ans}")
        print(f"  {msg}")
        off += len(chunk)
    return msg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hex")
    ap.add_argument("--host", default="169.254.61.60")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--code", help="the pairing code (shown on the Rytm and the Sidecar)")
    g.add_argument("--token", help="a session token (X-Sidecar-Auth)")
    ap.add_argument("--expect", default="", help="the build the relay must name afterwards")
    ap.add_argument("--upload-only", action="store_true", help="upload and check, do not write")
    ap.add_argument("--timeout", type=float, default=120, help="seconds for the whole write")
    a = ap.parse_args()

    hexdata = open(a.hex, "rb").read()
    print(f"{a.hex}: {len(hexdata)} bytes")
    token = a.token or pair(a.host, a.code)
    put(a.host, token, hexdata)
    if a.upload_only:
        print(call(a.host, token, "/teensy/state"))
        return 0
    ans = call(a.host, token, "/teensy/flash", urllib.parse.urlencode({"expect": a.expect}).encode(),
               "application/x-www-form-urlencoded").strip()
    print(ans)
    if not ans.startswith("200 "):
        return 1
    t0, last = time.time(), None
    while time.time() - t0 < a.timeout:
        time.sleep(0.5)
        try:
            st = json.loads(call(a.host, token, "/teensy/state", timeout=5))
        except Exception as e:  # the web may pause while USB re-enumerates
            print(f"  (state: {e})")
            continue
        line = f"  {st['state']}: {st['sent']}/{st['hex']['blocks']} blocks, {st['retries']} retries, {st['ms']} ms"
        if line != last:
            print(line)
            last = line
        if st["state"] == "done":
            print(f"done: the relay is back with build {st['build']}")
            return 0
        if st["state"] == "failed":
            print(f"failed: {st['error']}")
            return 1
    print("timed out waiting for the write (GET /teensy/state)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
