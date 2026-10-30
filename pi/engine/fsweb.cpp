// pi/engine/fsweb.cpp - see fsweb.h. Hosted (the core's flags): the web task on core 0 calls it.
#include "fsweb.h"

#include <string.h>

#ifdef FSWEB_HOST   // the Mac tests (pi/engine/tests/test_fsweb.cpp)
#include <stdio.h>
#define sniprintf snprintf
#else
#include "core_port.h"   // sniprintf: platform_pi.cpp's integer printf
#endif
#include "lib/sha256.h"
#include "rmp_api.h"
#include "rmp_loader.h"

namespace fsweb {
namespace {

// WHAT /fs/* CAN REACH AT ALL (docs/pi-engine.md "Pairing code" > "What the file API can reach"). The split is:
// /fs/* is content, TCP 7310 is firmware, /.rytm is neither and is reachable through neither. Deny by default: a
// path that is not one of these folders, or inside one, does not exist as far as this API is concerned, for every
// operation - list, get, put, del, mkdir and both ends of a mv.
//
// Before this, path_ok() checked syntax only and read_only() guarded writes to the root's boot files, so /fs/get
// served anything on the card (the Wi-Fi key in /.rytm/wifi.txt, and the pairing code when a headless unit wrote
// one down) and read_only()'s "below the root: only overlays/ is the firmware's" let a write land on
// /.rytm/pair.txt - putting a chosen device hash in there is a way past the pairing code itself. The pairing gate
// in web.cpp stops an unpaired client; this stops a paired one, and stops the gate being the only thing in the way.
//
// The set comes from what the engine and the page actually use: the sample library and GRANULAR's takes and pools
// (/Samples, docs/LIBRARY.md, docs/POOLS.md), machine packages and their backups (/Plugins, /MACHINES, /PRESETS,
// docs/plugins.md), recordings (/Recordings), project bundles (/Bundles, docs/BUNDLES.md), the instrument and table
// folders (/MULTISAMPLE, /WAVETABLES, /GRANULAR), chop maps (/CHOP, docs/CHOP.md), AMP captures (/NAM,
// docs/LIBRARY.md) and the JUNO / DX7 banks. Add a folder here when the engine starts using one, not elsewhere.
const char* const kContent[] = {"/Bundles", "/CHOP", "/DX7", "/GRANULAR", "/JUNO", "/MACHINES", "/MULTISAMPLE",
                                "/NAM", "/PRESETS", "/Plugins", "/Recordings", "/Samples", "/WAVETABLES"};

const char* const kBoot[] = {"config.txt", "config.txt.bak", "cmdline.txt", "bootcode.bin", "LICENCE.broadcom", "issue.txt"};
const char* const kBootPrefix[] = {"sdboot", "engine.img", "kernel", "start", "fixup", "armstub", "tryboot.txt", "bcm27"};

bool ieq(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return false;
    if (!x) return true;
  }
  return true;
}
bool ends(const char* s, const char* e) {
  size_t a = strlen(s), b = strlen(e);
  return a >= b && ieq(s + a - b, e, b + 1);
}
void say(Result* r, int st, const char* m) {
  r->status = st;
  sniprintf(r->msg, sizeof r->msg, "%s", m);
}

struct Up { char path[kPath + 1]; uint64_t next, total; uint32_t crc; bool on; core::lib::Sha256 sh; };
Up g_up[4];
Up* up_find(const char* p, bool make) {
  for (Up& u : g_up) if (u.on && strcmp(u.path, p) == 0) return &u;
  if (!make) return nullptr;
  for (Up& u : g_up) if (!u.on) return &u;
  return &g_up[0];   // the oldest slot is reused (an abandoned upload)
}
void part_of(const char* p, char* out) { sniprintf(out, kPath + 8, "%s.part", p); }

uint8_t g_rb[4096];   // the read-back (one web worker writes at a time: Circle's tasks are cooperative)

}  // namespace

uint32_t crc32(uint32_t crc, const uint8_t* p, uint32_t n) { return ~rmp_crc32_update(~crc, p, n); }

bool path_ok(const char* p) {
  size_t n = strlen(p);
  if (n == 0 || n > (size_t)kPath || p[0] != '/') return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x20 || c > 0x7e || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      return false;
    if (c == '/' && p[i + 1] == '/') return false;
  }
  if (strstr(p, "/../") || ends(p, "/..") || strstr(p, "/./") || ends(p, "/.")) return false;
  return true;
}

bool read_only(const char* p) {
  if (!p[0] || strcmp(p, "/") == 0) return true;
  if (ieq(p, "/overlays", 9) && (p[9] == 0 || p[9] == '/')) return true;
  if (ieq(p, "/System Volume Information", 26)) return true;
  const char* name = p + 1;
  if (strchr(name, '/')) return false;   // below the root: only overlays/ is the firmware's
  for (const char* b : kBoot) if (ieq(name, b, strlen(b) + 1)) return true;
  for (const char* b : kBootPrefix) if (ieq(name, b, strlen(b))) return true;
  if (ends(name, ".dtb") || ends(name, ".elf") || ends(name, ".dat")) return true;
  return false;
}

bool kept_dir(const char* p) {
  static const char* const kKept[] = {"/.rytm", "/Samples", "/Plugins", "/Bundles"};   // Bundles: docs/BUNDLES.md
  for (const char* k : kKept) if (ieq(p, k, strlen(k) + 1)) return true;
  return false;
}

int drive_of(const char* p, const char** rest) {
  static const char* const kDrive[] = {"/USB", "/USB2", "/USB3", "/NVME"};   // stor_vol.h kUSB.. kNVME
  for (int i = 0; i < 4; ++i) {
    size_t n = strlen(kDrive[i]);
    if (strncmp(p, kDrive[i], n) == 0 && (p[n] == 0 || p[n] == '/')) {
      if (rest) *rest = p[n] ? p + n : "/";
      return i + 1;
    }
  }
  if (rest) *rest = p;
  return 0;
}

bool volume_path(const char* p, char* out, int cap) {
  static const char* const kVol[] = {"SD:", "USB:", "USB2:", "USB3:", "NVME:"};
  const char* rest;
  int d = drive_of(p, &rest);
  return sniprintf(out, (size_t)cap, "%s%s", kVol[d], rest) < cap;
}

bool live_take(const char* p, const char* id) {
  const char* c = strchr(id, ':');
  if (!id[0] || (c && c == id)) return false;
  char dir[kPath + 1], mark[kPath + 1];   // as this API names them: /Recordings/take_004, /USB/Recordings/take_004
  if (c) {
    sniprintf(dir, sizeof dir, "/%.*s/Recordings/%s", (int)(c - id), id, c + 1);
    sniprintf(mark, sizeof mark, "/%.*s/Recordings/.recording", (int)(c - id), id);
  } else {
    sniprintf(dir, sizeof dir, "/Recordings/%s", id);
    sniprintf(mark, sizeof mark, "/Recordings/.recording");
  }
  size_t a = strlen(p), l = strlen(dir);
  if (a == strlen(mark) && ieq(p, mark, a)) return true;
  if (a >= l) return ieq(p, dir, l) && (p[l] == 0 || p[l] == '/');   // it, or in it
  return ieq(p, dir, a) && dir[a] == '/';                            // a folder above it
}

bool reachable(const char* p) {
  if (!path_ok(p)) return false;
  const char* rest;
  if (drive_of(p, &rest)) return ieq(rest, "/Recordings", 11) && (rest[11] == 0 || rest[11] == '/');   // a drive: its takes only
  if (strcmp(p, "/") == 0) return true;   // the root: listed, but only the content folders show (list_json)
  for (const char* c : kContent) {
    size_t n = strlen(c);
    if (ieq(p, c, n) && (p[n] == 0 || p[n] == '/')) return true;
  }
  return false;
}

bool is_package(const char* p) { return ieq(p, "/Plugins/", 9) && !strchr(p + 9, '/') && ends(p, ".rmp"); }

void fat_time(uint16_t d, uint16_t t, char out[17]) {
  unsigned y = 1980 + (d >> 9), mo = d >> 5 & 15, dd = d & 31, h = t >> 11, mi = t >> 5 & 63;
  if (!d || mo < 1 || mo > 12 || dd < 1 || h > 23 || mi > 59) { out[0] = 0; return; }
  sniprintf(out, 17, "%04u-%02u-%02u %02u:%02u", y, mo, dd, h, mi);
}

int list_json(const Backend& b, const char* path, char* out, int cap, Result* r) {
  static Entry e[256];
  bool more = false;
  if (!path_ok(path)) { say(r, 400, "bad path"); return 0; }
  if (!reachable(path)) { say(r, 404, "no such path (the file browser reaches the content folders only)"); return 0; }
  int n = b.list(path, e, 256, &more);
  if (n < 0) { say(r, 404, "no such folder"); return 0; }
  uint64_t fr = 0, tot = 0;
  b.space(&fr, &tot);
  int k = sniprintf(out, (size_t)cap, "{\"path\":\"%s\",\"free\":%llu,\"total\":%llu,\"more\":%d,\"entries\":[", path,
                   (unsigned long long)fr, (unsigned long long)tot, more ? 1 : 0);
  char full[kPath + 100];
  int nOut = 0;                       // entries actually written: reachable() skips some, so i is not the count
  for (int i = 0; i < n && k < cap - 300; ++i) {
    sniprintf(full, sizeof full, "%s%s%s", path, strcmp(path, "/") ? "/" : "", e[i].name);
    if (!reachable(full)) continue;   // the card's root shows the content folders, not the boot files or /.rytm
    char nm[200];
    int q = 0;
    for (const char* s = e[i].name; *s && q < 190; ++s) {   // JSON-escaped
      if (*s == '"' || *s == '\\') nm[q++] = '\\';
      nm[q++] = (*s >= 0x20 && *s <= 0x7e) ? *s : '_';
    }
    nm[q] = 0;
    char tm[17];
    fat_time(e[i].fdate, e[i].ftime, tm);
    k += sniprintf(out + k, (size_t)(cap - k), "%s{\"n\":\"%s\",\"d\":%d,\"s\":%llu,\"t\":\"%s\",\"ro\":%d}", nOut++ ? "," : "", nm,
                  e[i].dir ? 1 : 0, (unsigned long long)e[i].size, tm, read_only(full) ? 1 : kept_dir(full) ? 2 : 0);
  }
  k += sniprintf(out + k, (size_t)(cap - k), "]}");
  say(r, 200, "ok");
  return k < cap ? k : 0;
}

const char* check_package(const Backend& b, const char* path, uint64_t size) {
  uint8_t h[RMP_HEADER_BYTES];
  uint32_t got = 0;
  if (size < RMP_HEADER_BYTES || !b.read(path, 0, h, sizeof h, &got) || got != sizeof h) return "too short for a machine package";
  if (size > RMP_HEADER_BYTES + (2u << 20)) return "too big for a machine package (over 2 MB)";
  rmp_info info;
  rmp_status st = rmp_verify_header_any(h, (uint32_t)size, &info);
  if (st == RMP_ERR_MAGIC) return "not a machine package (no RMP1/RMPA header)";
  if (st == RMP_ERR_API) return "built for a newer plugin API than this engine runs";
  if (st == RMP_ERR_FORMAT) return "package format not supported (or no id)";
  if (st != RMP_OK) return "sizes in the header do not match the file";
  if (info.target != RMP_TARGET_A64) return "a Teensy package (RMP1, ARM): build it for the Pi with `make a64`";
  if (!rmp_id_ok(info.id)) return "its id is not safe as a file name";
  uint32_t c = 0xFFFFFFFFu;
  for (uint64_t off = RMP_HEADER_BYTES; off < size;) {
    uint32_t n = size - off < sizeof g_rb ? (uint32_t)(size - off) : (uint32_t)sizeof g_rb;
    if (!b.read(path, off, g_rb, n, &got) || got != n) return "read error while checking";
    c = rmp_crc32_update(c, g_rb, n);
    off += n;
    if (b.yield) b.yield();
  }
  if (~c != info.crc32) return "CRC wrong: the file is damaged (upload it again)";
  uint8_t t[8];
  if (!b.read(path, RMP_HEADER_BYTES + info.entry_offset, t, 8, &got) || got != 8) return "read error while checking";
  uint32_t magic, api;
  memcpy(&magic, t, 4);
  memcpy(&api, t + 4, 4);
  if (magic != RMP_PLUGIN_MAGIC || api < RMP_API_MIN || api > RMP_API) return "its entry table is not a plugin's";
  return nullptr;
}

void put(const Backend& b, const char* path, uint64_t off, uint64_t total, uint32_t crc, const uint8_t* data, uint32_t n,
         Result* r) {
  if (!path_ok(path) || ends(path, ".part")) return say(r, 400, "bad path");
  if (!reachable(path) || strcmp(path, "/") == 0) return say(r, 404, "no such path (the file browser reaches the content folders only)");   // content only (fsweb.h)
  if (read_only(path)) return say(r, 403, "a boot file: read-only here (sdfiles installs it)");
  if (drive_of(path)) return say(r, 403, "a drive's recordings: listed, downloaded and deleted here, never written");
  if (b.busy && b.busy(path)) return say(r, 409, "the take is recording: stop it first");   // (the last chunk would
  if (n > kChunkMax || off + n > total) return say(r, 400, "chunk too big or past the total");   // unlink its file)
  char part[kPath + 8];
  part_of(path, part);
  Up* u = up_find(path, off == 0);
  if (off == 0 && u->on && strcmp(u->path, path) != 0) {   // the slot of an abandoned upload: its .part goes
    char old[kPath + 8];
    part_of(u->path, old);
    u->on = false;
    b.remove(old);
  }
  if (off == 0) {
    uint64_t fr = 0, tot = 0;
    if (b.space(&fr, &tot) && total > fr) return say(r, 507, "not enough free space on the card");
    char parent[kPath + 1];
    sniprintf(parent, sizeof parent, "%s", path);
    char* sl = strrchr(parent, '/');
    if (sl && sl != parent) {
      *sl = 0;
      uint64_t sz;
      bool dir = false;
      if (!b.stat(parent, &sz, &dir) || !dir) return say(r, 404, "its folder does not exist");
    }
    bool dir = false;
    uint64_t sz;
    if (b.stat(path, &sz, &dir) && dir) return say(r, 409, "a folder of that name exists");
    sniprintf(u->path, sizeof u->path, "%s", path);
    u->next = 0, u->total = total, u->crc = 0, u->on = true;
    u->sh.init();
  } else if (!u || u->next != off || u->total != total) {
    sniprintf(r->msg, sizeof r->msg, "expected %llu", (unsigned long long)(u && u->total == total ? u->next : 0));
    r->status = 409;
    return;
  }
  for (uint32_t done = 0; done < n || (n == 0 && done == 0);) {   // 4 KB pieces, each read back
    uint32_t k = n - done < 4096 ? n - done : 4096;
    if (!b.write_at(part, off + done, data + done, k, off + done == 0)) { u->on = false; b.remove(part); return say(r, 500, "write failed (card full or removed?)"); }
    uint32_t got = 0;
    if (k && (!b.read(part, off + done, g_rb, k, &got) || got != k || memcmp(g_rb, data + done, k))) {
      u->on = false;
      b.remove(part);
      return say(r, 500, "read-back differs: the card did not keep the bytes");
    }
    u->crc = crc32(u->crc, data + done, k);
    u->sh.update(data + done, k);
    done += k;
    if (b.yield) b.yield();
    if (n == 0) break;
  }
  u->next = off + n;
  if (u->next < total) {
    sniprintf(r->msg, sizeof r->msg, "ok %llu", (unsigned long long)u->next);
    r->status = 200;
    return;
  }
  u->on = false;
  if (u->crc != crc) { b.remove(part); return say(r, 400, "CRC of the upload wrong: not saved (try again)"); }
  if (is_package(path)) {
    const char* why = check_package(b, part, total);
    if (why) { b.remove(part); sniprintf(r->msg, sizeof r->msg, "refused: %s", why); r->status = 422; return; }
  }
  uint64_t sz;
  bool dir;
  if (b.stat(path, &sz, &dir) && !b.remove(path)) { b.remove(part); return say(r, 500, "could not replace the old file"); }
  if (!b.rename(part, path)) return say(r, 500, "could not put the file in place (left as .part)");
  if (b.stored) {
    uint8_t h[32];
    u->sh.final(h);
    b.stored(path, h, u->crc);
  }
  if (b.changed) b.changed(path);
  sniprintf(r->msg, sizeof r->msg, "done %llu", (unsigned long long)total);
  r->status = 200;
}

void remove(const Backend& b, const char* path, Result* r) {
  if (!path_ok(path)) return say(r, 400, "bad path");
  if (!reachable(path) || strcmp(path, "/") == 0) return say(r, 404, "no such path (the file browser reaches the content folders only)");
  if (read_only(path)) return say(r, 403, "a boot file: read-only here");
  if (kept_dir(path)) return say(r, 403, "kept: the Sidecar's own folder (its contents can go)");
  if (b.busy && b.busy(path)) return say(r, 409, "the take is recording: stop it first");
  uint64_t sz;
  bool dir;
  if (!b.stat(path, &sz, &dir)) return say(r, 404, "no such file");
  if (!b.remove(path)) return say(r, 409, dir ? "the folder is not empty" : "could not delete");
  if (b.removed) b.removed(path);
  if (b.changed) b.changed(path);
  say(r, 200, "deleted");
}

void mkdir(const Backend& b, const char* path, Result* r) {
  if (!path_ok(path)) return say(r, 400, "bad path");   // malformed is still malformed
  if (!reachable(path) || strcmp(path, "/") == 0) return say(r, 404, "no such path (the file browser reaches the content folders only)");   // content only (fsweb.h)
  if (read_only(path)) return say(r, 400, "bad path");
  if (drive_of(path)) return say(r, 403, "a drive's recordings: listed, downloaded and deleted here, never written");
  uint64_t sz;
  bool dir;
  if (b.stat(path, &sz, &dir)) return say(r, 409, "exists");
  if (!b.mkdir(path)) return say(r, 500, "could not make the folder");
  say(r, 200, "made");
}

void mkdirs(const Backend& b, const char* path, Result* r) {
  if (!path_ok(path)) return say(r, 400, "bad path");   // malformed is still malformed
  if (!reachable(path) || strcmp(path, "/") == 0) return say(r, 404, "no such path (the file browser reaches the content folders only)");   // content only (fsweb.h)
  if (read_only(path)) return say(r, 400, "bad path");
  if (drive_of(path)) return say(r, 403, "a drive's recordings: listed, downloaded and deleted here, never written");
  char p[kPath + 1];
  sniprintf(p, sizeof p, "%s", path);
  int made = 0;
  for (char* s = p + 1;; ++s) {   // each component from the root: "/a", "/a/b", ...
    if (*s != '/' && *s) continue;
    char c = *s;
    *s = 0;
    uint64_t sz;
    bool dir = false;
    if (read_only(p)) return say(r, 403, "a boot folder: read-only here");
    if (b.stat(p, &sz, &dir)) {
      if (!dir) return say(r, 409, "a file of that name is in the way");
    } else if (!b.mkdir(p)) return say(r, 500, "could not make the folder");
    else ++made;
    if (b.yield) b.yield();
    if (!c) break;
    *s = c;
  }
  say(r, 200, made ? "made" : "exists");
}

void rename(const Backend& b, const char* from, const char* to, Result* r) {
  if (!path_ok(from) || !path_ok(to) || ends(to, ".part")) return say(r, 400, "bad path");
  // both ends: a move must not be able to reach out of the content folders in either direction
  if (!reachable(from) || !reachable(to) || strcmp(from, "/") == 0 || strcmp(to, "/") == 0) return say(r, 404, "no such path (the file browser reaches the content folders only)");
  if (read_only(from) || read_only(to)) return say(r, 403, "a boot file: read-only here");
  if (drive_of(from) || drive_of(to)) return say(r, 403, "a drive's recordings: listed, downloaded and deleted here, never renamed");
  if (kept_dir(from)) return say(r, 403, "kept: the Sidecar's own folder cannot be renamed");
  if (b.busy && (b.busy(from) || b.busy(to))) return say(r, 409, "the take is recording: stop it first");
  uint64_t sz;
  bool dir;
  if (!b.stat(from, &sz, &dir)) return say(r, 404, "no such file");
  if (b.stat(to, &sz, &dir)) return say(r, 409, "the new name exists");
  if (is_package(to) && !dir) {
    const char* why = check_package(b, from, sz);
    if (why) { sniprintf(r->msg, sizeof r->msg, "refused: %s", why); r->status = 422; return; }
  }
  if (!b.rename(from, to)) return say(r, 500, "could not rename");
  if (b.renamed) b.renamed(from, to);
  if (b.changed) { b.changed(from); b.changed(to); }
  say(r, 200, "renamed");
}

}  // namespace fsweb
