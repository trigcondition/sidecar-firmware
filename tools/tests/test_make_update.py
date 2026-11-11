#!/usr/bin/env python3
"""tools/make_update.py: the update file's format and its refusals (docs/web-update.md). The engine's side reads the
same file in pi/engine/tests/test_update.cpp. Run: python3 tools/tests/test_make_update.py [-q]"""
import hashlib, io, os, struct, sys, tempfile, contextlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import make_update as mu  # noqa: E402


def kernel(board, v="0.5.1", c="bbbbbbb", stamp=True, net=False, size=50000):
    k = bytearray(os.urandom(size // 2)) + b"\0==== rytm-pi engine ==== Sidecar\0"
    if board == 5:
        k += mu.PI5_LINES[0] + b"\0"
    if stamp:
        k += f"sidecar-webupdate-1 v={v} c={c}".encode() + b"\0sidecar-webupdate-1 v=%s c=%s\0"
    if net:
        k += b"\x01169.254.61.60\x00"
    return bytes(k + os.urandom(size // 2))


def refused(argv, why):
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            mu.main(argv)
    except SystemExit as e:
        assert why in str(e), (why, str(e))
        return
    raise AssertionError(f"not refused: {why}")


def main():
    quiet = "-q" in sys.argv
    with tempfile.TemporaryDirectory() as d:
        p = lambda n: os.path.join(d, n)
        def save(n, b):
            open(p(n), "wb").write(b)
            return p(n)
        k4, k5 = save("k4", kernel(4, net=True)), save("k5", kernel(5, net=True))
        os.makedirs(p("dist/assets"))
        save("dist/index.html", b"<html>")
        save("dist/assets/a.js", b"js")
        save("dist/assets/f.woff2", b"font")
        with contextlib.redirect_stdout(io.StringIO()):
            mu.main(["--pi4", k4, "--pi5", k5, "--web", p("dist"), "--release", "--out", p("u.scu")])
        data = open(p("u.scu"), "rb").read()
        v, c, parts = mu.check(data)
        assert (v, c) == ("0.5.1", "bbbbbbb") and [k for k, _ in parts] == [mu.PI4, mu.PI5, mu.WEB]
        assert data[:8] == b"SCUPDAT1" and hashlib.sha256(data[:480]).digest() == data[480:512]
        _, _, off, size, _ = struct.unpack_from("<IIQQ32s", data, 80)
        e4 = data[off:off + size]
        assert mu.unwrap(e4)[:50000] == open(k4, "rb").read()[:50000]   # the Pi 4 part is the wrapped kernel
        _, _, woff, wsize, _ = struct.unpack_from("<IIQQ32s", data, 80 + 128)
        w = data[woff:woff + wsize]
        assert w[:8] == b"SCWEB001" and struct.unpack_from("<I", w, 8)[0] == 3
        assert b"index.html.gz" in w and b"assets/a.js.gz" in w and b"assets/f.woff2" in w and b"index.html\x00" not in w
        # an engine.img as --pi4 is the same file
        save("e4", mu.wrap(open(k4, "rb").read()))
        with contextlib.redirect_stdout(io.StringIO()):
            mu.main(["--pi4", p("e4"), "--pi5", k5, "--web", p("dist"), "--out", p("u2.scu")])
        assert open(p("u2.scu"), "rb").read() == data
        # deterministic
        with contextlib.redirect_stdout(io.StringIO()):
            mu.main(["--pi4", k4, "--pi5", k5, "--web", p("dist"), "--out", p("u3.scu")])
        assert open(p("u3.scu"), "rb").read() == data
        # --check finds damage
        bad = bytearray(data)
        bad[off + 100] ^= 1
        save("bad.scu", bytes(bad))
        refused(["--check", p("bad.scu")], "sha256 wrong")
        bad = bytearray(data)
        bad[30] ^= 1
        save("bad2.scu", bytes(bad))
        refused(["--check", p("bad2.scu")], "header is damaged")
        # refusals
        refused(["--pi4", k5, "--pi5", k5, "--out", p("x")], "is a Pi 5 kernel")
        refused(["--pi4", k4, "--pi5", k4, "--out", p("x")], "is a Pi 4 kernel")
        refused(["--pi4", save("j", b"junk" * 100), "--pi5", k5, "--out", p("x")], "not a Pi engine kernel")
        refused(["--pi4", save("o4", kernel(4, c="ccccccc")), "--pi5", k5, "--out", p("x")], "stamps differ")
        refused(["--pi4", save("n4", kernel(4, stamp=False)), "--pi5", k5, "--out", p("x")], "no update stamp")
        refused(["--pi4", k4, "--pi5", k5, "--version", "0.5.2", "--out", p("x")], "not 0.5.2")
        refused(["--pi4", save("l4", kernel(4)), "--pi5", save("l5", kernel(5)), "--release", "--out", p("x")], "release address")
        refused(["--pi4", save("d4", kernel(4, c="bbbbbbb-dirty", net=True)), "--pi5", save("d5", kernel(5, c="bbbbbbb-dirty", net=True)),
                 "--release", "--out", p("x")], "-dirty")
        os.makedirs(p("dist2"))
        save("dist2/.hidden", b"x")
        save("dist2/index.html", b"x")
        refused(["--pi4", k4, "--pi5", k5, "--web", p("dist2"), "--out", p("x")], "will not serve")
        os.makedirs(p("dist3"))
        save("dist3/a.js", b"x")
        refused(["--pi4", k4, "--pi5", k5, "--web", p("dist3"), "--out", p("x")], "no index.html")
        bad4 = bytearray(mu.wrap(open(k4, "rb").read()))
        bad4[12] ^= 1
        refused(["--pi4", save("b4", bytes(bad4)), "--pi5", k5, "--out", p("x")], "sums are wrong")
    if not quiet:
        print("test_make_update: ok")


if __name__ == "__main__":
    main()
