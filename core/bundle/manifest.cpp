// core/bundle/manifest.cpp - see manifest.h.
#include "manifest.h"

#include <string.h>
#include <strings.h>

#include "../platform.h"
#include "../lib/sha256.h"

namespace core {
namespace bundle {

namespace {
const char* const kKindNames[kKinds] = {"pool", "project", "sample", "sfz", "take", "song", "seq", "package", "rytm"};
void cpy(char* d, const char* s, int cap) { sniprintf(d, cap, "%s", s ? s : ""); }
bool path_ok(const char* p) {   // a card path in a manifest: absolute, a bundle-safe name after the '/'
  return p[0] == '/' && strlen(p) < (size_t)kCardPath && name_ok(p + 1);
}
}  // namespace

int reader_for(const char* fmt, int ver) {
  (void)fmt, (void)ver;   // every format and version written today reads with bundle reader 1
  return 1;
}
const char* kind_name(int k) { return k >= 0 && k < kKinds ? kKindNames[k] : "?"; }
int kind_of(const char* s) {
  for (int k = 0; k < kKinds; ++k)
    if (!strcmp(s, kKindNames[k])) return k;
  return -1;
}

void Manifest::alloc(int entries, int paths, int miss) {
  if (!e) e = new MEntry[entries], cap_e = entries;
  if (!p) p = new MPath[paths], cap_p = paths;
  if (!missing) missing = new char[miss][kCardPath], cap_missing = miss;
  clear();
}
void Manifest::clear() {
  MEntry* e0 = e;
  MPath* p0 = p;
  char(*m0)[kCardPath] = missing;
  int ce = cap_e, cp = cap_p, cm = cap_missing;
  memset((void*)this, 0, sizeof *this);
  e = e0, p = p0, missing = m0, cap_e = ce, cap_p = cp, cap_missing = cm;
  version = kFormat, min_reader = 1;
}
void Manifest::add_format(const char* name, int ver) {
  for (int i = 0; i < nformats; ++i)
    if (!strcmp(formats[i].name, name)) {
      if (ver > formats[i].ver) formats[i].ver = ver;
      return;
    }
  if (nformats >= kFormats || !name[0] || strlen(name) >= sizeof formats[0].name) return;
  cpy(formats[nformats].name, name, sizeof formats[nformats].name);
  formats[nformats++].ver = ver;
}
int Manifest::add_entry(const char* name, int kind) {
  if (ne >= cap_e || strlen(name) > (size_t)kNameMax) return -1;
  MEntry& x = e[ne];
  memset(&x, 0, sizeof x);
  cpy(x.name, name, sizeof x.name);
  x.kind = (uint8_t)kind;
  x.first_path = np;
  x.group = -1;
  return ne++;
}
bool Manifest::add_path(int i, const char* path) {
  if (np >= cap_p || i < 0 || i >= ne || strlen(path) >= (size_t)kCardPath) return false;
  if (e[i].npaths == 0) e[i].first_path = np;
  else if (e[i].first_path + e[i].npaths != np) return false;   // an entry's paths are contiguous
  cpy(p[np].path, path, kCardPath);
  p[np].entry = i;
  ++np, ++e[i].npaths;
  return true;
}
int Manifest::find_entry(const char* name) const {
  for (int i = 0; i < ne; ++i)
    if (!strcmp(e[i].name, name)) return i;
  return -1;
}
int Manifest::find_path(const char* path) const {
  for (int i = 0; i < np; ++i)
    if (!strcasecmp(p[i].path, path)) return i;
  return -1;
}
uint64_t Manifest::bytes() const {
  uint64_t b = 0;
  for (int i = 0; i < ne; ++i) b += e[i].size;
  return b;
}
int Manifest::count(int kind) const {
  int c = 0;
  for (int i = 0; i < ne; ++i) c += e[i].kind == kind;
  return c;
}

bool write_manifest(const Manifest& m, JOut& o) {
  char h[65];
  o.raw("{\"format\":\"sidecar-bundle\",\"version\":");
  o.num((uint64_t)m.version);
  o.raw(",\"min_reader\":");
  o.num((uint64_t)m.min_reader);
  o.raw(",\"created\":");
  o.str(m.created);
  o.raw(",\"clock\":");
  o.str(m.clock);
  o.raw(",\n\"sidecar\":{\"version\":");
  o.str(m.sc_version);
  o.raw(",\"board\":");
  o.str(m.board);
  o.raw("},\n\"project\":{\"rytm_name\":");
  o.str(m.rytm_name);
  o.raw(",\"key\":");
  o.str(m.key);
  o.raw("},\n\"prints\":[");
  for (int i = 0; i < m.nprints; ++i) {
    if (i) o.raw(",");
    o.raw("{\"slot\":"), o.num((uint64_t)m.prints[i].slot), o.raw(",\"fp\":"), o.num((uint64_t)m.prints[i].fp);
    o.raw(",\"n\":"), o.num((uint64_t)m.prints[i].n), o.raw("}");
  }
  o.raw("],\n\"fxprints\":[");
  for (int i = 0; i < m.nfxprints; ++i) {
    if (i) o.raw(",");
    o.raw("{\"slot\":"), o.num((uint64_t)m.fxprints[i].slot), o.raw(",\"fp\":"), o.num((uint64_t)m.fxprints[i].fp);
    o.raw(",\"name\":"), o.str(m.fxprints[i].name), o.raw("}");
  }
  o.raw("],\n\"slots\":[");
  for (int i = 0; i < m.nslots; ++i) {
    const Slot& s = m.slots[i];
    if (i) o.raw(",");
    o.raw("{\"slot\":"), o.num((uint64_t)s.slot), o.raw(",\"id\":"), o.str(s.id), o.raw(",\"ver\":"), o.str(s.ver);
    o.raw(",\"name\":"), o.str(s.name), o.raw(",\"fp\":"), o.num((uint64_t)s.fp), o.raw(",\"packages\":{");
    if (s.pkg[0]) o.str(s.target), o.raw(":"), o.str(s.pkg);
    o.raw("}}");
  }
  o.raw("],\n\"fxslots\":[],\n\"global_info\":{\"sendfx_route\":");
  o.str(m.sendfx_route);
  o.raw(",\"fx_in\":");
  o.str(m.fx_in);
  o.raw("},\n\"formats\":{\"sqp\":");
  o.num((uint64_t)m.f_sqp);
  o.raw(",\"song\":");
  o.num((uint64_t)m.f_song);
  o.raw(",\"slots\":");
  o.num((uint64_t)m.f_slots);
  for (int i = 0; i < m.nformats; ++i) o.raw(","), o.key(m.formats[i].name), o.num((uint64_t)m.formats[i].ver);
  o.raw("},\n\"entries\":[");
  int rytm = -1;
  for (int i = 0; i < m.ne; ++i) {
    const MEntry& x = m.e[i];
    if (x.kind == kKindRytm) rytm = i;
    o.raw(i ? ",\n" : "\n");
    o.raw("{\"name\":"), o.str(x.name), o.raw(",\"kind\":"), o.str(kind_name(x.kind)), o.raw(",\"size\":");
    o.num(x.size);
    hex(x.sha, 32, h);
    o.raw(",\"sha256\":"), o.str(h);
    if (x.npaths) {
      o.raw(",\"paths\":[");
      for (int j = 0; j < x.npaths; ++j) {
        if (j) o.raw(",");
        o.str(m.p[x.first_path + j].path);
      }
      o.raw("]");
    }
    if (x.kind == kKindSfz) o.raw(x.abs_refs ? ",\"abs_refs\":true" : ",\"abs_refs\":false");
    if (x.fmt[0]) o.raw(",\"format\":{\"name\":"), o.str(x.fmt), o.raw(",\"version\":"), o.num((uint64_t)x.fmt_ver), o.raw("}");
    o.raw("}");
  }
  o.raw("],\n\"missing\":[");
  for (int i = 0; i < m.nmissing; ++i) {
    if (i) o.raw(",");
    o.str(m.missing[i]);
  }
  o.raw("],\n\"rytm_file\":");
  if (rytm < 0) o.raw("null");
  else {
    hex(m.e[rytm].sha, 32, h);
    o.raw("{\"name\":"), o.str(m.e[rytm].name), o.raw(",\"size\":"), o.num(m.e[rytm].size), o.raw(",\"sha256\":");
    o.str(h), o.raw("}");
  }
  o.raw(",\n\"totals\":{\"entries\":");
  o.num((uint64_t)m.ne);
  o.raw(",\"bytes\":");
  o.num(m.bytes());
  o.raw(",\"samples\":");
  o.num((uint64_t)(m.count(kKindSample) + m.count(kKindTake)));
  o.raw(",\"songs\":");
  o.num((uint64_t)m.count(kKindSong));
  o.raw(",\"patterns\":");
  o.num((uint64_t)m.count(kKindSeq));
  o.raw(",\"packages\":");
  o.num((uint64_t)m.count(kKindPackage));
  o.raw("}}\n");
  return !o.over;
}

// ---- the reader
namespace {
struct R {
  JIn& j;
  Manifest* m;
  char* err;
  int cap;
  bool bad(const char* what) {
    sniprintf(err, cap, "manifest: %s", what);
    return false;
  }
  bool str(char* out, int c, const char* what) {
    if (!j.str(out, (uint32_t)c)) return bad(what);
    return true;
  }
  bool num(int64_t* v, int64_t lo, int64_t hi, const char* what) {
    if (!j.num(v) || *v < lo || *v > hi) return bad(what);
    return true;
  }
};
}  // namespace

bool read_manifest(const char* text, uint32_t n, Manifest* m, char* err, int cap) {
  JIn j;
  j.begin(text, n);
  R r{j, m, err, cap};
  m->clear();
  m->version = 0, m->min_reader = 0;
  char k[64], v[kCardPath];
  int64_t x;
  bool format = false, have_entries = false;
  if (n > kManifestMax) return r.bad("over 1 MB");
  if (!j.obj_begin()) return r.bad(j.err);
  while (j.next_key(k, sizeof k)) {
    if (!strcmp(k, "format")) {
      if (!r.str(v, sizeof v, "format") || strcmp(v, "sidecar-bundle")) return r.bad("not a Sidecar bundle");
      format = true;
    } else if (!strcmp(k, "version")) {
      if (!r.num(&x, 1, 1000000, "version")) return false;
      m->version = (int)x;
    } else if (!strcmp(k, "min_reader")) {
      if (!r.num(&x, 1, 1000000, "min_reader")) return false;
      m->min_reader = (int)x;
    } else if (!strcmp(k, "created")) {
      if (!r.str(m->created, sizeof m->created, "created")) return false;
    } else if (!strcmp(k, "clock")) {
      if (!r.str(m->clock, sizeof m->clock, "clock")) return false;
    } else if (!strcmp(k, "sidecar")) {
      if (!j.obj_begin()) return r.bad("sidecar");
      while (j.next_key(k, sizeof k)) {
        if (!strcmp(k, "version")) { if (!r.str(m->sc_version, sizeof m->sc_version, "sidecar.version")) return false; }
        else if (!strcmp(k, "board")) { if (!r.str(m->board, sizeof m->board, "sidecar.board")) return false; }
        else if (!j.skip()) return r.bad("sidecar");
      }
    } else if (!strcmp(k, "project")) {
      if (!j.obj_begin()) return r.bad("project");
      while (j.next_key(k, sizeof k)) {
        if (!strcmp(k, "rytm_name")) { if (!r.str(m->rytm_name, sizeof m->rytm_name, "project.rytm_name")) return false; }
        else if (!strcmp(k, "key")) { if (!r.str(m->key, sizeof m->key, "project.key")) return false; }
        else if (!j.skip()) return r.bad("project");
      }
    } else if (!strcmp(k, "prints") || !strcmp(k, "fxprints")) {
      bool fx = k[0] == 'f';
      if (!j.arr_begin()) return r.bad("prints");
      while (j.arr_more()) {
        int& np = fx ? m->nfxprints : m->nprints;
        if (np >= (fx ? kFxSlots : kSlots)) return r.bad("too many prints");
        Print& p = fx ? m->fxprints[np] : m->prints[np];
        memset(&p, 0, sizeof p);
        if (!j.obj_begin()) return r.bad("prints");
        while (j.next_key(k, sizeof k)) {
          if (!strcmp(k, "slot")) { if (!r.num(&x, 1, 8, "prints.slot")) return false; p.slot = (int)x; }
          else if (!strcmp(k, "fp")) { if (!r.num(&x, 0, 16383, "prints.fp")) return false; p.fp = (int)x; }
          else if (!strcmp(k, "n")) { if (!r.num(&x, 0, 100000, "prints.n")) return false; p.n = (int)x; }
          else if (!strcmp(k, "name")) { if (!r.str(p.name, sizeof p.name, "prints.name")) return false; }
          else if (!j.skip()) return r.bad("prints");
        }
        ++np;
      }
    } else if (!strcmp(k, "slots")) {
      if (!j.arr_begin()) return r.bad("slots");
      while (j.arr_more()) {
        if (m->nslots >= kSlots) return r.bad("too many slots");
        Slot& s = m->slots[m->nslots];
        memset(&s, 0, sizeof s);
        if (!j.obj_begin()) return r.bad("slots");
        while (j.next_key(k, sizeof k)) {
          if (!strcmp(k, "slot")) { if (!r.num(&x, 1, kSlots, "slots.slot")) return false; s.slot = (int)x; }
          else if (!strcmp(k, "id")) { if (!r.str(s.id, sizeof s.id, "slots.id")) return false; }
          else if (!strcmp(k, "ver")) { if (!r.str(s.ver, sizeof s.ver, "slots.ver")) return false; }
          else if (!strcmp(k, "name")) { if (!r.str(s.name, sizeof s.name, "slots.name")) return false; }
          else if (!strcmp(k, "fp")) { if (!r.num(&x, 0, 16383, "slots.fp")) return false; s.fp = (int)x; }
          else if (!strcmp(k, "packages")) {
            if (!j.obj_begin()) return r.bad("slots.packages");
            char t[16];
            while (j.next_key(t, sizeof t)) {
              char pk[kNameMax + 1];
              if (!r.str(pk, sizeof pk, "slots.packages")) return false;
              if (!strcmp(t, "a64") || !s.pkg[0]) cpy(s.pkg, pk, sizeof s.pkg), cpy(s.target, t, sizeof s.target);
            }
            if (j.err) return r.bad(j.err);
          } else if (!j.skip()) return r.bad("slots");
        }
        ++m->nslots;
      }
    } else if (!strcmp(k, "global_info")) {
      if (!j.obj_begin()) return r.bad("global_info");
      while (j.next_key(k, sizeof k)) {
        if (!strcmp(k, "sendfx_route")) { if (!r.str(m->sendfx_route, sizeof m->sendfx_route, "global_info")) return false; }
        else if (!strcmp(k, "fx_in")) { if (!r.str(m->fx_in, sizeof m->fx_in, "global_info")) return false; }
        else if (!j.skip()) return r.bad("global_info");
      }
    } else if (!strcmp(k, "formats")) {
      if (!j.obj_begin()) return r.bad("formats");
      while (j.next_key(k, sizeof k)) {
        if (!r.num(&x, 0, 1000, "formats")) return false;
        if (!strcmp(k, "sqp")) m->f_sqp = (int)x;
        else if (!strcmp(k, "song")) m->f_song = (int)x;
        else if (!strcmp(k, "slots")) m->f_slots = (int)x;
        else m->add_format(k, (int)x);
      }
    } else if (!strcmp(k, "entries")) {
      have_entries = true;
      if (!j.arr_begin()) return r.bad("entries");
      while (j.arr_more()) {
        char name[kNameMax + 1] = "", sha[80] = "";
        int kind = -1;
        int64_t size = -1;
        bool abs = false;
        int first = m->np, npaths = 0;
        char fmt[16] = "";
        int64_t fver = 0;
        if (!j.obj_begin()) return r.bad("entries");
        while (j.next_key(k, sizeof k)) {
          if (!strcmp(k, "name")) { if (!r.str(name, sizeof name, "an entry name")) return false; }
          else if (!strcmp(k, "kind")) { if (!r.str(v, 16, "an entry kind")) return false; kind = kind_of(v); }
          else if (!strcmp(k, "size")) { if (!r.num(&size, 0, 0xFFFFFFFFll, "an entry size")) return false; }
          else if (!strcmp(k, "sha256")) { if (!r.str(sha, sizeof sha, "an entry sha256")) return false; }
          else if (!strcmp(k, "abs_refs")) { if (!j.boolean(&abs)) return r.bad("abs_refs"); }
          else if (!strcmp(k, "format")) {
            if (!j.obj_begin()) return r.bad("an entry format");
            while (j.next_key(k, sizeof k)) {
              if (!strcmp(k, "name")) { if (!r.str(fmt, sizeof fmt, "an entry format")) return false; }
              else if (!strcmp(k, "version")) { if (!r.num(&fver, 0, 1000000, "an entry format version")) return false; }
              else if (!j.skip()) return r.bad("an entry format");
            }
            if (j.err) return r.bad(j.err);
          }
          else if (!strcmp(k, "paths")) {
            if (!j.arr_begin()) return r.bad("paths");
            while (j.arr_more()) {
              if (m->np >= m->cap_p) return r.bad("too many paths");
              if (!r.str(m->p[m->np].path, kCardPath, "a path")) return false;
              if (!path_ok(m->p[m->np].path)) return r.bad("an unsafe path");
              m->p[m->np].entry = m->ne;
              ++m->np, ++npaths;
            }
            if (j.err) return r.bad(j.err);
          } else if (!j.skip()) return r.bad("entries");
        }
        if (j.err) return r.bad(j.err);
        if (!name_ok(name)) return r.bad("an unsafe entry name");
        if (kind < 0) return r.bad("an entry of an unknown kind");
        if (size < 0) return r.bad("an entry without a size");
        if (m->find_entry(name) >= 0 || !strcmp(name, "manifest.json")) return r.bad("an entry named twice");
        if (m->ne >= m->cap_e) return r.bad("too many entries");
        MEntry& e = m->e[m->ne];
        memset(&e, 0, sizeof e);
        cpy(e.name, name, sizeof e.name);
        e.kind = (uint8_t)kind;
        e.size = (uint32_t)size;
        e.abs_refs = abs;
        e.first_path = first, e.npaths = npaths;
        e.group = -1;
        cpy(e.fmt, fmt, sizeof e.fmt);
        e.fmt_ver = (int)fver;
        if (!unhex(sha, e.sha, 32)) return r.bad("an entry's sha256");
        ++m->ne;
      }
    } else if (!strcmp(k, "missing")) {
      if (!j.arr_begin()) return r.bad("missing");
      while (j.arr_more()) {
        if (m->nmissing >= m->cap_missing) {   // listed for the summary only: the rest dropped
          if (!r.str(v, sizeof v, "missing")) return false;
          continue;
        }
        if (!r.str(m->missing[m->nmissing], kCardPath, "missing")) return false;
        ++m->nmissing;
      }
    } else if (!j.skip()) {
      return r.bad(j.err ? j.err : "a value");
    }
  }
  if (j.err) return r.bad(j.err);
  j.ws();
  if (j.at != j.n) return r.bad("text after the manifest");
  if (!format) return r.bad("not a Sidecar bundle");
  if (!m->version || !m->min_reader) return r.bad("no version");
  if (m->min_reader > kReader) {
    sniprintf(err, cap, "this bundle needs a newer Sidecar (bundle reader %d, this one reads %d)", m->min_reader, kReader);
    return false;
  }
  if (!have_entries) return r.bad("no entries");
  if (!m->key[0]) return r.bad("no project key");
  return true;
}

}  // namespace bundle
}  // namespace core
