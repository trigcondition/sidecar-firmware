// core/tests/test_bundle_zip.cpp - bundles phase 1, the container (docs/BUNDLES.md "Format"): SHA-256 against
// FIPS 180-4's examples, CRC-32, the stored zip's layout (4096-aligned data, the 0xD935 padding), any byte range of the
// archive equal to the whole, byte-exact against a fixed reference, read by Python's zipfile and `unzip -t`, the
// reader's checks (every region's damage refused), the 4 GiB refusal; the manifest's JSON round trip and its bounded
// reader against truncations, huge numbers and deep nesting.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bundle/json.h"
#include "bundle/manifest.h"
#include "lib/sha256.h"
#include "bundle/zip.h"
#include "check.h"

using namespace core::bundle;

static std::string hexs(const uint8_t* d, int n) {
  char b[129];
  hex(d, n, b);
  return b;
}
static std::string sha(const std::string& s) {
  uint8_t d[32];
  sha256(s.data(), s.size(), d);
  return hexs(d, 32);
}

struct Src {
  std::vector<std::string>* data;
};
static bool data_fn(void* ctx, int i, uint32_t off, uint8_t* buf, uint32_t n) {
  auto& d = *((Src*)ctx)->data;
  if (off + n > d[i].size()) return false;
  memcpy(buf, d[i].data() + off, n);
  return true;
}
static std::string whole(std::vector<ZEntry>& e, std::vector<std::string>& d, Layout* L, uint32_t epoch) {
  uint16_t dd, dt;
  dos_time(epoch, &dd, &dt);
  for (size_t i = 0; i < e.size(); ++i) e[i].size = (uint32_t)d[i].size(), e[i].crc = crc32(0, d[i].data(), d[i].size());
  CHECK(layout(e.data(), (int)e.size(), dd, dt, L));
  std::string z(L->total, '\0');
  Src s{&d};
  CHECK(render(e.data(), *L, 0, (uint8_t*)&z[0], (uint32_t)z.size(), data_fn, &s));
  return z;
}
static ZEntry ze(const char* n) {
  ZEntry e;
  memset(&e, 0, sizeof e);
  snprintf(e.name, sizeof e.name, "%s", n);
  return e;
}
static bool read_all(const std::string& z, std::vector<ZEntry>& out, std::string* err) {
  char e[160] = "";
  ZRead r;
  uint32_t t = z.size() < kEocdTail ? (uint32_t)z.size() : kEocdTail;
  if (!eocd((const uint8_t*)z.data() + z.size() - t, t, z.size(), &r, e, sizeof e)) return *err = e, false;
  out.assign(r.n, ZEntry());
  if (!central((const uint8_t*)z.data() + r.cd_off, r, out.data(), r.n, e, sizeof e)) return *err = e, false;
  for (int i = 0; i < r.n; ++i) {
    uint64_t next = i + 1 < r.n ? out[i + 1].local : r.cd_off;
    uint64_t avail = z.size() - out[i].local;
    if (!local((const uint8_t*)z.data() + out[i].local, (uint32_t)(avail < kLocalMax ? avail : kLocalMax), &out[i], next, e, sizeof e))
      return *err = e, false;
  }
  return true;
}

int main() {
  // ---- SHA-256 (FIPS 180-4 examples), any split of the input the same
  CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  {
    std::string m(1000000, 'a');
    CHECK(sha(m) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Sha256 s;
    s.init();
    for (size_t at = 0, k = 1; at < m.size(); at += k, k = k * 7 % 997 + 1) s.update(m.data() + at, std::min(k, m.size() - at));
    uint8_t d[32];
    s.final(d);
    CHECK(hexs(d, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  uint8_t u[32];
  CHECK(unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", u, 32) && hexs(u, 32)[0] == 'b');
  CHECK(!unhex("zz", u, 1) && !unhex("abc", u, 1));
  // ---- CRC-32 (the check value), in pieces
  CHECK(crc32(0, "123456789", 9) == 0xCBF43926u);
  CHECK(crc32(crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);

  // ---- names
  for (const char* ok : {"manifest.json", "files/Samples/Drums/kick.wav", "files/Samples/caf\xc3\xa9.wav", "rytm/AR PERC.arprj"})
    CHECK(name_ok(ok));
  for (const char* bad : {"", "/abs", "a/../b", "..", "a/./b", "a//b", "a/", "a\\b", "c:x", "a\nb", "a\x7f"})
    CHECK(!name_ok(bad));
  CHECK(!name_ok(std::string(201, 'a').c_str()) && name_ok(std::string(200, 'a').c_str()));

  // ---- a stored zip: aligned, any range equal to the whole, byte-exact, read back
  std::vector<ZEntry> e = {ze("manifest.json"), ze("project/pool.txt"), ze("files/Samples/a b.wav"), ze("empty.txt")};
  std::vector<std::string> d = {"{\"format\":\"sidecar-bundle\"}\n", "kick.wav\n\nsnare.wav\n", std::string(10000, 'x'), ""};
  for (int i = 0; i < 10000; ++i) d[2][i] = (char)(i * 31 + 7);
  Layout L;
  std::string z = whole(e, d, &L, 1792677780u);   // 2026-10-22 14:03:00 UTC
  for (auto& x : e) CHECK(x.data % kAlign == 0 && z.compare(x.data, x.size, d[&x - &e[0]]) == 0);
  CHECK(z.size() == L.total && L.cd_off + L.cd_size + 22 == L.total);
  printf("test_bundle_zip: 4 entries, %llu bytes, sha256 %s\n", (unsigned long long)z.size(), sha(z).c_str());
  CHECK(sha(z) == "2699c2fc16f18f099b14ec34667d04057b01fd7f712893af95cec329af06a6a4");   // the fixed reference (checked by Python's zipfile and unzip -t below)
  {
    Src s{&d};
    std::vector<uint8_t> b(z.size());
    for (uint32_t step : {1u, 7u, 4096u, 4093u, 65536u})
      for (uint64_t off = 0; off < z.size(); off += step) {
        uint32_t n = (uint32_t)std::min<uint64_t>(step, z.size() - off);
        CHECK(render(e.data(), L, off, b.data(), n, data_fn, &s));
        CHECK(memcmp(b.data(), z.data() + off, n) == 0);
      }
    CHECK(!render(e.data(), L, z.size() - 1, b.data(), 2, data_fn, &s));
  }
  {
    std::vector<ZEntry> r;
    std::string err;
    CHECK(read_all(z, r, &err));
    CHECK(r.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(!strcmp(r[i].name, e[i].name) && r[i].data == e[i].data && r[i].size == e[i].size && r[i].crc == e[i].crc);
  }
  // Python's zipfile and unzip read it: every entry, its CRC, the alignment
  char tmpl[] = "/tmp/bzip-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string dir = tmpl, path = dir + "/t.sidecar.zip";
  FILE* f = fopen(path.c_str(), "wb");
  fwrite(z.data(), 1, z.size(), f);
  fclose(f);
  std::string py = "python3 -I -c \"import zipfile,sys\nz=zipfile.ZipFile(sys.argv[1])\nassert z.testzip() is None\n"
                   "n=[i.filename for i in z.infolist()]\nassert n==['manifest.json','project/pool.txt','files/Samples/a b.wav','empty.txt'],n\n"
                   "assert all(i.compress_type==0 for i in z.infolist())\nassert z.read('project/pool.txt')==b'kick.wav\\n\\nsnare.wav\\n'\n"
                   "import struct\nb=open(sys.argv[1],'rb').read()\n"
                   "for i in z.infolist():\n h=b[i.header_offset:i.header_offset+30]\n nl,xl=struct.unpack('<HH',h[26:30])\n"
                   " assert (i.header_offset+30+nl+xl)%4096==0\n x=b[i.header_offset+30+nl:i.header_offset+30+nl+4]\n"
                   " assert struct.unpack('<H',x[:2])[0]==0xD935\n assert i.flag_bits&0x800 and not i.flag_bits&8\" '" + path + "'";
  CHECK(system(py.c_str()) == 0);
  if (system("command -v unzip >/dev/null 2>&1") == 0) CHECK(system(("unzip -tq '" + path + "' >/dev/null").c_str()) == 0);

  // ---- damage in each region refused: the end record, the central directory, a local header; data damage is the
  // CRC / sha256's (the import's hash pass)
  {
    std::vector<ZEntry> r;
    std::string err;
    std::vector<uint64_t> spots = {z.size() - 22, z.size() - 10, L.cd_off, L.cd_off + 10, L.cd_off + 20, L.cd_off + 42,
                                   e[1].local, e[1].local + 14, e[1].local + 18, e[1].local + 26, e[2].local + 31};
    for (uint64_t at : spots) {
      std::string bad = z;
      bad[at] ^= 0x41;
      CHECK(!read_all(bad, r, &err));
    }
    CHECK(!read_all(z.substr(0, z.size() - 1), r, &err));
    CHECK(!read_all(z + "x", r, &err));
    CHECK(!read_all("PK", r, &err));
  }
  // a zip by another tool: deflated, data descriptors -> refused with the reason
  {
    std::string p2 = dir + "/other.zip";
    CHECK(system(("python3 -I -c \"import zipfile\nz=zipfile.ZipFile('" + p2 + "','w',zipfile.ZIP_DEFLATED)\nz.writestr('a.txt','hello '*100)\nz.close()\"").c_str()) == 0);
    FILE* g = fopen(p2.c_str(), "rb");
    std::string o;
    char b[4096];
    for (size_t n; (n = fread(b, 1, sizeof b, g)) > 0;) o.append(b, n);
    fclose(g);
    std::vector<ZEntry> r;
    std::string err;
    CHECK(!read_all(o, r, &err) && err.find("compressed") != std::string::npos);
  }
  // ---- the 4 GiB limit: refused by the layout (sizes only, nothing written)
  {
    std::vector<ZEntry> big = {ze("manifest.json"), ze("files/a.wav"), ze("files/b.wav")};
    big[0].size = 1000, big[1].size = 0xFFFF0000u, big[2].size = 0x10000;
    Layout B;
    CHECK(!layout(big.data(), 3, 0, 0, &B));
    big[2].size = 0x1000;
    CHECK(!layout(big.data(), 3, 0, 0, &B));   // 4 GiB - 64 KiB at most, headers counted
    big[1].size = 0xFFFE0000u - 3 * kAlign;
    CHECK(layout(big.data(), 3, 0, 0, &B) && B.total <= kZipMax);
  }

  // ---- the manifest: written, read back, written again: the same text
  Manifest m{}, m2{};
  m.alloc(64, 64, 8);
  m2.alloc(64, 64, 8);
  strcpy(m.created, "2026-10-22T14:03:00Z");
  strcpy(m.clock, "browser");
  strcpy(m.sc_version, "0.2.0");
  strcpy(m.board, "pi4");
  strcpy(m.rytm_name, "AR PERC \"PATCH\"");
  strcpy(m.key, "AR PERC _PATCH_");
  m.prints[0] = {1, 1829, 12, ""};
  m.nprints = 1;
  m.fxprints[0] = {1, 77, 0, "SHIMMER"};
  m.nfxprints = 1;
  m.slots[0] = {1, "sc.grain", "1.0.0", "GRAIN", 1829, "plugins/sc.grain-1.0.0.a64.rmp", "a64"};
  m.nslots = 1;
  strcpy(m.sendfx_route, "fx");
  strcpy(m.fx_in, "in");
  m.f_sqp = 3, m.f_song = 1, m.f_slots = 1;
  m.add_format("samplepool", 1);
  m.add_format("poolused", 1);
  m.add_format("samplepool", 2);   // (once a name: the highest)
  int i0 = m.add_entry("project/samples.pool.txt", kKindPool);
  m.e[i0].size = 22;
  sha256("x", 1, m.e[i0].sha);
  strcpy(m.e[i0].fmt, "samplepool"), m.e[i0].fmt_ver = 1;
  int i1 = m.add_entry("files/Samples/Drums/kick.wav", kKindSample);
  m.e[i1].size = 123456;
  CHECK(m.add_path(i1, "/Samples/Drums/kick.wav") && m.add_path(i1, "/Samples/Old/kick copy.wav"));
  int i2 = m.add_entry("files/MULTISAMPLE/Piano/Piano.sfz", kKindSfz);
  m.e[i2].abs_refs = true;
  CHECK(m.add_path(i2, "/MULTISAMPLE/Piano/Piano.sfz"));
  CHECK(!m.add_path(i1, "/x"));   // an entry's paths stay together
  int i3 = m.add_entry("rytm/AR PERC.arprj", kKindRytm);
  m.e[i3].size = 812345;
  strcpy(m.missing[0], "/Samples/gone.wav");
  m.nmissing = 1;
  static char t1[1 << 16], t2[1 << 16];
  JOut o;
  o.begin(t1, sizeof t1);
  CHECK(write_manifest(m, o));
  char err[160];
  CHECK(read_manifest(t1, o.n, &m2, err, sizeof err));
  JOut o2;
  o2.begin(t2, sizeof t2);
  CHECK(write_manifest(m2, o2));
  CHECK(o.n == o2.n && !memcmp(t1, t2, o.n));
  CHECK(m2.ne == 4 && m2.e[1].npaths == 2 && !strcmp(m2.p[m2.e[1].first_path + 1].path, "/Samples/Old/kick copy.wav"));
  CHECK(m2.nformats == 2 && !strcmp(m2.formats[0].name, "samplepool") && m2.formats[0].ver == 2 && m2.formats[1].ver == 1);
  CHECK(strstr(t1, "\"formats\":{\"sqp\":3,\"song\":1,\"slots\":1,\"samplepool\":2,\"poolused\":1}"));
  CHECK(!strcmp(m2.rytm_name, "AR PERC \"PATCH\"") && m2.e[2].abs_refs);
  // an older bundle's "pools" descriptors and "bank_a" record: read past
  {
    const char* old = "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},"
                      "\"pools\":[{\"id\":\"dsp\",\"entry\":\"project/pool.txt\",\"base\":\"/Samples\",\"skip\":0,\"refs\":\"sample\"}],"
                      "\"bank_a\":{\"granular\":[[0,\"@PAD\"]],\"wavetable\":null},\"entries\":[]}";
    CHECK(read_manifest(old, (uint32_t)strlen(old), &m2, err, sizeof err) && m2.ne == 0 && m2.nformats == 0);
  }
  // Python's json reads it the same
  {
    std::string p3 = dir + "/manifest.json";
    FILE* g = fopen(p3.c_str(), "wb");
    fwrite(t1, 1, o.n, g);
    fclose(g);
    CHECK(system(("python3 -I -c \"import json,sys\nm=json.load(open(sys.argv[1]))\nassert m['format']=='sidecar-bundle' and "
                  "m['entries'][1]['paths'][1]=='/Samples/Old/kick copy.wav' and m['totals']['entries']==4\" '" + p3 + "'").c_str()) == 0);
  }
  // every truncation refused (never a crash, never a read past the end)
  for (uint32_t n = 0; n < o.n - 2; ++n) CHECK(!read_manifest(t1, n, &m2, err, sizeof err));
  // huge numbers, deep nesting, bad escapes, a newer reader, a zip slip in a name or a path
  const std::string bad[] = {
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":99999999999999999999,\"entries\":[]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"x\":[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]],\"entries\":[]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":2,\"entries\":[],\"project\":{\"key\":\"K\"}}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[{\"name\":\"../x\",\"kind\":\"pool\",\"size\":1,\"sha256\":\"" + std::string(64, '0') + "\"}]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[{\"name\":\"files/a\",\"kind\":\"sample\",\"size\":1,\"sha256\":\"" + std::string(64, '0') + "\",\"paths\":[\"/a/../../b\"]}]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[{\"name\":\"files/a\",\"kind\":\"sample\",\"size\":-1,\"sha256\":\"" + std::string(64, '0') + "\"}]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[{\"name\":\"files/a\",\"kind\":\"sample\",\"size\":1.5,\"sha256\":\"" + std::string(64, '0') + "\"}]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[{\"name\":\"a\\u0000b\",\"kind\":\"sample\",\"size\":1,\"sha256\":\"" + std::string(64, '0') + "\"}]}",
      "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[]} x",
      "{\"format\":\"zip\",\"version\":1,\"min_reader\":1,\"project\":{\"key\":\"K\"},\"entries\":[]}",
  };
  for (const std::string& b : bad) { if (read_manifest(b.data(), (uint32_t)b.size(), &m2, err, sizeof err)) printf("read: %s\n", b.c_str()); CHECK(!read_manifest(b.data(), (uint32_t)b.size(), &m2, err, sizeof err)); }
  CHECK(read_manifest(bad[2].data(), (uint32_t)bad[2].size(), &m2, err, sizeof err) == false && strstr(err, "newer Sidecar"));
  const char* good = "{\"format\":\"sidecar-bundle\",\"version\":2,\"min_reader\":1,\"future\":{\"a\":[1,2,{\"b\":null}]},"
                     "\"project\":{\"key\":\"K\"},\"entries\":[]}";
  CHECK(read_manifest(good, (uint32_t)strlen(good), &m2, err, sizeof err));   // a newer writer, an unknown member: read
  // random bytes: refused, never a crash
  {
    uint32_t x = 1;
    std::string r(4096, ' ');
    for (int t = 0; t < 2000; ++t) {
      for (auto& c : r) x = x * 1103515245 + 12345, c = "{}[]\",:0123456789aeflnrstu\\ "[(x >> 16) % 29];
      read_manifest(r.data(), (uint32_t)r.size(), &m2, err, sizeof err);
    }
  }
  system(("rm -rf '" + dir + "'").c_str());
  printf("test_bundle_zip: PASS\n");
  return 0;
}
