#!/usr/bin/env python3
"""The release's update file for the web page's Settings > Update (docs/web-update.md, pi/engine/update.h).

One file for both boards: the Pi 4's engine.img (the raw kernel8-rpi4.img wrapped as sdboot.py does), the Pi 5's
kernel_2712.img and the web app (web-next's dist/, as web-next/scripts/card.mjs puts it on the card: text files as
.gz only). A 512-byte header (magic SCUPDAT1, the version and commit, each part's kind, offset, size and sha256, the
header's own sha256), then the parts.

  make_update.py --pi4 KERNEL8 --pi5 KERNEL2712 [--web DIST] [--release] --out FILE
                 [--version V --commit C]        (default: the kernels' own stamp, which both must carry)
                 [--build-web]                   (builds web-next first and takes its dist/; needs web-next/node_modules:
                                                  `cd web-next && npm ci` once - nothing is installed for you)
  make_update.py --check FILE                    the file's header and parts, checked and listed

--pi4 takes the raw kernel8-rpi4.img or an engine.img (SDCRENG1, its sums checked). --release refuses kernels without
the release address 169.254.61.60 (scripts/package_pi.sh's check) and -dirty builds. Every kernel must carry the update
code's stamp "sidecar-webupdate-1 v=<version> c=<commit>" (a kernel without it could never make itself permanent).
"""
import argparse, gzip, hashlib, os, re, struct, subprocess, sys

MAGIC, HEADER, FORMAT = b"SCUPDAT1", 512, 1
PI4, PI5, WEB = 1, 2, 3
KIND = {PI4: "Pi 4 engine.img", PI5: "Pi 5 kernel_2712.img", WEB: "web app"}
PI4_MAX = 0x800000 - 0x80000 + 32   # sdboot's SDB_MAXLEN + the header
PI5_MAX, WEB_MAX, WEB_FILES = 16 << 20, 8 << 20, 1024
GZ = re.compile(r"\.(html|js|css|svg|txt|json)$")          # card.mjs
SAFE = re.compile(r"^[A-Za-z0-9_\-./]+$")
STAMP = re.compile(rb"sidecar-webupdate-1 v=([0-9A-Za-z.\-]{1,23}) c=([0-9a-f]{7,12}(?:-dirty)?)\x00")
PI5_LINES = (b"board: Raspberry Pi 5 build (kernel_2712.img)", b"gadget: none on this board (Pi 5 build)")
RELEASE_NET = re.compile(rb"(?<![\x20-\x7e])169\.254\.61\.60(?![\x20-\x7e])")


def fail(msg):
    raise SystemExit(f"make_update: {msg}")


def wrap(img):
    """sdboot.py wrap(): SDCRENG1, the length, two running sums over the 32-bit words"""
    img += b"\0" * (-len(img) % 4)
    s1 = s2 = 0
    for (w,) in struct.iter_unpack("<I", img):
        s1 = (s1 + w) & 0xFFFFFFFF
        s2 = (s2 + s1) & 0xFFFFFFFF
    return b"SDCRENG1" + struct.pack("<IIIIII", len(img), s1, s2, 0, 0, 0) + img


def unwrap(e):
    """the kernel inside an engine.img, its header and sums checked (as sdboot does)"""
    if len(e) < 32 or e[:8] != b"SDCRENG1":
        fail("not an engine.img")
    n, s1, s2 = struct.unpack_from("<III", e, 8)
    if n != len(e) - 32 or n % 4 or any(e[20:32]):
        fail("engine.img: the header does not match the size")
    if wrap(e[32:]) != e or (s1, s2) != struct.unpack_from("<II", wrap(e[32:]), 12):
        fail("engine.img: the sums are wrong")
    return e[32:]


def board(img):
    if b"==== rytm-pi engine" not in img:
        fail("not a Pi engine kernel image (no '==== rytm-pi engine' in it)")
    return 5 if any(m in img for m in PI5_LINES) else 4


def stamp(img, what):
    s = set(STAMP.findall(img))
    if len(s) != 1:
        fail(f"{what}: {'no' if not s else 'more than one'} update stamp 'sidecar-webupdate-1 v=.. c=..' "
             "(a kernel without the web update code cannot install itself)")
    v, c = s.pop()
    return v.decode(), c.decode()


WEB_NEXT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web-next")


def build_web(web_next=WEB_NEXT):
    """`npm run build` in web-next (vue-tsc, then vite): its dist/. The dependencies must be there already."""
    if not os.path.isfile(os.path.join(web_next, "package.json")):
        fail(f"{web_next}: no web-next here")
    if not os.path.isdir(os.path.join(web_next, "node_modules")):
        fail(f"{web_next}/node_modules is missing: run `cd web-next && npm ci` once (the build needs it; this tool installs nothing)")
    try:
        subprocess.run(["npm", "run", "build"], cwd=web_next, check=True, stdout=sys.stderr)
    except (OSError, subprocess.CalledProcessError) as e:
        fail(f"web-next's build failed: {e}")
    return os.path.join(web_next, "dist")


def web_archive(dist):
    files = []
    for d, dirs, names in os.walk(dist):
        dirs.sort()
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(d, n), dist).replace(os.sep, "/")
            if not (0 < len(rel) <= 120 and SAFE.match(rel)) or any(p == "" or p.startswith(".") for p in rel.split("/")):
                fail(f"{rel}: a name the Pi will not serve")
            b = open(os.path.join(d, n), "rb").read()
            if GZ.search(rel):
                rel, b = rel + ".gz", gzip.compress(b, 9, mtime=0)
                if len(rel) > 120:
                    fail(f"{rel}: too long")
            files.append((rel, b))
    if not any(n == "index.html.gz" for n, _ in files):
        fail(f"{dist}: no index.html (cd web-next && npm run build)")
    if len(files) > WEB_FILES:
        fail("the web app has more than 1024 files")
    out = bytearray(b"SCWEB001" + struct.pack("<I", len(files)))
    for n, b in files:
        nb = n.encode()
        out += struct.pack("<HHI", len(nb), 0, len(b)) + nb + b
    if len(out) > WEB_MAX:
        fail("the web app is over 8 MB")
    return bytes(out)


def build(parts, version, commit, dirty=False):
    """the file: parts = [(kind, bytes)]"""
    if not (0 < len(version) < 24 and 0 < len(commit) < 24):
        fail("version / commit too long")
    h = bytearray(HEADER)
    struct.pack_into("<8sIII I", h, 0, MAGIC, HEADER, FORMAT, len(parts), 1 if dirty else 0)
    h[24:24 + len(version)] = version.encode()
    h[48:48 + len(commit)] = commit.encode()
    off = HEADER
    for i, (kind, b) in enumerate(parts):
        struct.pack_into("<IIQQ32s", h, 80 + 64 * i, kind, 0, off, len(b), hashlib.sha256(b).digest())
        off += len(b)
    h[480:512] = hashlib.sha256(bytes(h[:480])).digest()
    return bytes(h) + b"".join(b for _, b in parts)


def check(data):
    """the header and every part checked (as the Pi does, less the card); returns (version, commit, [(kind, size)])"""
    if len(data) < HEADER or data[:8] != MAGIC:
        fail("not a Sidecar update file")
    hb, fmt, n, flags = struct.unpack_from("<IIII", data, 8)
    if hb != HEADER or fmt != FORMAT:
        fail("header size / format")
    if hashlib.sha256(data[:480]).digest() != data[480:512]:
        fail("the header is damaged")
    version = data[24:48].split(b"\0")[0].decode()
    commit = data[48:72].split(b"\0")[0].decode()
    at, out = HEADER, []
    want = f"sidecar-webupdate-1 v={version} c={commit}".encode() + b"\0"
    for i in range(n):
        kind, _, off, size, sha = struct.unpack_from("<IIQQ32s", data, 80 + 64 * i)
        if off != at or off + size > len(data) or kind not in KIND:
            fail(f"part {i}: kind / offset / size")
        b = data[off:off + size]
        if hashlib.sha256(b).digest() != sha:
            fail(f"part {i} ({KIND[kind]}): sha256 wrong")
        if kind in (PI4, PI5):
            k = unwrap(b) if kind == PI4 else b
            if board(k) != (5 if kind == PI5 else 4) or want not in k:
                fail(f"part {i} ({KIND[kind]}): wrong board or not version {version} ({commit})")
        out.append((kind, size))
        at = off + size
    if at != len(data):
        fail("bytes after the last part")
    return version, commit, out


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    a.add_argument("--pi4"), a.add_argument("--pi5"), a.add_argument("--web"), a.add_argument("--out")
    a.add_argument("--version"), a.add_argument("--commit"), a.add_argument("--check")
    a.add_argument("--release", action="store_true")
    a.add_argument("--build-web", action="store_true")
    o = a.parse_args(argv)
    if o.check:
        v, c, parts = check(open(o.check, "rb").read())
        print(f"{o.check}: Sidecar {v} ({c}): " + ", ".join(f"{KIND[k]} {s} bytes" for k, s in parts))
        return
    if not (o.pi4 and o.pi5 and o.out):
        a.error("--pi4, --pi5 and --out are needed (or --check FILE)")
    if o.build_web and o.web:
        fail("--build-web and --web together: pick one")
    k4 = open(o.pi4, "rb").read()
    k4 = unwrap(k4) if k4[:8] == b"SDCRENG1" else k4
    k5 = open(o.pi5, "rb").read()
    if board(k4) != 4:
        fail(f"{o.pi4} is a Pi 5 kernel")
    if board(k5) != 5:
        fail(f"{o.pi5} is a Pi 4 kernel")
    s4, s5 = stamp(k4, o.pi4), stamp(k5, o.pi5)
    if s4 != s5:
        fail(f"the kernels' stamps differ: Pi 4 {s4}, Pi 5 {s5} (build both from one clean commit)")
    v, c = s4
    if (o.version and o.version != v) or (o.commit and o.commit != c):
        fail(f"the kernels are {v} ({c}), not {o.version} ({o.commit})")
    if o.release:
        if c.endswith("-dirty"):
            fail("a -dirty build is not a release")
        for p, k in ((o.pi4, k4), (o.pi5, k5)):
            if not RELEASE_NET.search(k):
                fail(f"{p} is not built with the release address 169.254.61.60")
    e4 = wrap(k4)
    if len(e4) > PI4_MAX or len(k5) > PI5_MAX:
        fail("a kernel is too big")
    if o.build_web:   # (after the kernels' checks: a wrong path must not cost a whole npm build first)
        o.web = build_web()
    parts = [(PI4, e4), (PI5, k5)] + ([(WEB, web_archive(o.web))] if o.web else [])
    data = build(parts, v, c, c.endswith("-dirty"))
    check(data)
    tmp = o.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, o.out)
    print(f"{o.out}: Sidecar {v} ({c}), {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}: "
          + ", ".join(f"{KIND[k]} {len(b)}" for k, b in parts))


if __name__ == "__main__":
    main()
