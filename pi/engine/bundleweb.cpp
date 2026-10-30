// pi/engine/bundleweb.cpp - see bundleweb.h.
#include "bundleweb.h"

#include <string.h>

#include "bundle/bundle.h"
#include "bundle/prjslot.h"
#include "bundle/projects.h"
#include "hosted/bundle_sw.h"   // build.sh: www/bundle-sw.js as BundleSw[] (a raw string)

namespace bundleweb {

namespace {

int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
uint64_t num(const char* s, bool* ok) {
  uint64_t v = 0;
  *ok = *s != 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || v > 100000000000000ull) { *ok = false; return 0; }
    v = v * 10 + (uint64_t)(*s - '0');
  }
  return v;
}
void text(Reply* r, uint8_t* out, uint32_t cap, int status, const char* json) {
  uint32_t n = (uint32_t)strlen(json);
  if (n > cap) n = cap;
  memcpy(out, json, n);
  r->status = status, r->type = "application/json", r->len = n;
}
void err(Reply* r, uint8_t* out, uint32_t cap, int status, const char* msg) {
  core::bundle::JOut o;
  o.begin((char*)out, cap);
  o.raw("{\"ok\":false,\"error\":"), o.str(msg), o.raw("}");
  r->status = status, r->type = "application/json", r->len = o.n;
}
void status_json(Reply* r, uint8_t* out, uint32_t cap, bool ok) {
  using namespace core::bundle;
  const Status& s = status();
  static const char* const jobs[] = {"none", "export", "import", "delete"};
  JOut o;
  o.begin((char*)out, cap);
  o.raw("{\"ok\":"), o.raw(ok ? "true" : "false"), o.raw(",\"job\":"), o.str(jobs[s.job < 4 ? s.job : 0]);
  o.raw(",\"phase\":"), o.str(phase_name(s.phase)), o.raw(",\"done\":"), o.num(s.done), o.raw(",\"total\":"), o.num(s.total);
  o.raw(",\"key\":"), o.str(s.key), o.raw(",\"file\":"), o.str(s.file), o.raw(",\"name\":"), o.str(s.phase == kServe ? bundle_name() : "");
  o.raw(",\"msg\":"), o.str(s.msg), o.raw(",\"waiting\":"), o.str(s.waiting), o.raw(",\"bytes\":"), o.num(s.bytes);
  o.raw(",\"seq\":"), o.num(s.seq);
  uint32_t ep;
  int src;
  o.raw(",\"clock\":"), o.str(core::projects::clock_now(&ep, &src) ? core::projects::clock_name(src) : "none"), o.raw("}");
  r->status = 200, r->type = "application/json", r->len = o.n;
}
bool bundle_path_ok(const char* p) {   // a bundle on the card the page may name: /Bundles/..., *.sidecar.zip, no ".."
  size_t n = strlen(p);
  if (strncmp(p, "/Bundles/", 9) || n < 22 || strcmp(p + n - 12, ".sidecar.zip") || strstr(p, "..") || strstr(p, "//")) return false;
  for (const char* c = p; *c; ++c)
    if ((unsigned char)*c < 0x20 || *c == '\\' || *c == ':') return false;
  return core::bundle::name_ok(p + 1);
}

// a bundle on a stick: <drive>/<name>.sidecar.zip or <drive>/SIDECAR/<name>.sidecar.zip (the folders the list shows)
bool stick_path_ok(const char* p) {
  char d[core::bundle::kDriveMax];
  int n = core::bundle::drive_of(p, d);
  if (!n || n == 3 /* "SD:" */ || p[n] != '/') return false;
  const char* rest = p + n + 1;
  if (!strncmp(rest, "SIDECAR/", 8)) rest += 8;
  size_t l = strlen(rest);
  if (l < 13 || strcmp(rest + l - 12, ".sidecar.zip") || strchr(rest, '/') || strstr(rest, "..")) return false;
  for (const char* c = rest; *c; ++c)
    if ((unsigned char)*c < 0x20 || *c == '\\' || *c == ':') return false;
  return true;
}
bool first_stick(char* drive) {   // the first mounted stick ("": none)
  core::bundle::Volume v[4];
  int n = core::bundle::env().volumes ? core::bundle::env().volumes(v, 4) : 0;
  drive[0] = 0;
  if (n > 0) strcpy(drive, v[0].drive);
  return n > 0;
}
// the stick's bundles: its /SIDECAR, then its root, as one array
int stick_list(const char* drive, char* out, int cap) {
  char a[80], b[80];
  size_t dl = strlen(drive) < 16 ? strlen(drive) : 16;
  memcpy(a, drive, dl), strcpy(a + dl, core::bundle::kStickFolder);
  memcpy(b, drive, dl), strcpy(b + dl, "/");
  int n1 = core::bundle::bundles_json(a, out, cap);
  if (n1 <= 0) return n1;
  int n2 = core::bundle::bundles_json(b, out + n1, cap - n1);
  if (n2 <= 0) return n2;
  if (n2 == 2) return n1;                       // "[]" at the root
  if (n1 == 2) return memmove(out, out + n1, (size_t)n2), n2;
  out[n1 - 1] = ',';                            // "[a]" + "[b]" -> "[a,b]"
  memmove(out + n1, out + n1 + 1, (size_t)n2 - 1);
  return n1 + n2 - 1;
}

}  // namespace

bool form_value(const char* form, const char* name, char* out, unsigned cap) {
  if (!form) return false;
  unsigned nl = (unsigned)strlen(name);
  for (const char* p = form; *p;) {
    if (!strncmp(p, name, nl) && p[nl] == '=') {
      p += nl + 1;
      unsigned n = 0;
      while (*p && *p != '&') {
        char c = *p++;
        if (c == '+') c = ' ';
        else if (c == '%' && hexv(p[0]) >= 0 && hexv(p[1]) >= 0) c = (char)(hexv(p[0]) << 4 | hexv(p[1])), p += 2;
        if (n + 1 >= cap) return false;
        out[n++] = c;
      }
      out[n] = 0;
      return true;
    }
    while (*p && *p != '&') ++p;
    if (*p == '&') ++p;
  }
  return false;
}

bool handle(const char* path, const char* params, const char* form, bool from_page, uint8_t* out, uint32_t cap, Reply* r) {
  using namespace core::bundle;
  if (!params) params = "";
  if (!form) form = "";
  char a[64], b[256], c[16];
  bool post_ops = !strcmp(path, "/bundle/export") || !strcmp(path, "/bundle/verify") || !strcmp(path, "/bundle/import") ||
                  !strcmp(path, "/bundle/cancel") || !strcmp(path, "/bundle/finish") || !strcmp(path, "/bundle/delete") ||
                  !strcmp(path, "/time") || !strcmp(path, "/usb/eject") || !strcmp(path, "/bundle/trash/restore") ||
                  !strcmp(path, "/bundle/trash/remove") || !strcmp(path, "/bundle/trash/empty") ||
                  !strcmp(path, "/bundle/remove");
  if (post_ops && !from_page) return err(r, out, cap, 403, "refused: not from this page (the same-origin check)"), true;
  if (!strcmp(path, "/bundle-sw.js")) {
    uint32_t n = (uint32_t)sizeof BundleSw - 1;
    if (n > cap) return err(r, out, cap, 500, "too big"), true;
    memcpy(out, BundleSw, n);
    r->status = 200, r->type = "application/javascript; charset=utf-8\r\nService-Worker-Allowed: /\r\nCache-Control: no-cache", r->len = n;
    return true;
  }
  if (!strcmp(path, "/bundle/projects")) {
    int n = projects_json((char*)out, (int)cap);
    if (n <= 0) return err(r, out, cap, 500, "too many projects to list"), true;
    r->status = 200, r->type = "application/json", r->len = (uint32_t)n;
    return true;
  }
  // 2026-10-27, the project trash (core/bundle/prjslot.h): the list read from the card here (core 0); restore / remove /
  // empty posted to the control core (its next ticks run them: /bundle/prjslot's "last" says how it went)
  if (!strcmp(path, "/bundle/trash")) {
    int n = core::prjslot::trash_json((char*)out, (int)cap);
    if (n <= 0) return err(r, out, cap, 500, "too many trash entries to list"), true;
    r->status = 200, r->type = "application/json", r->len = (uint32_t)n;
    return true;
  }
  if (!strcmp(path, "/bundle/prjslot")) {
    int n = core::prjslot::status_json((char*)out, (int)cap);
    if (n <= 0) return err(r, out, cap, 500, "too long"), true;
    r->status = 200, r->type = "application/json", r->len = (uint32_t)n;
    return true;
  }
  if (!strcmp(path, "/bundle/trash/restore") || !strcmp(path, "/bundle/trash/remove")) {
    bool ok1 = false;
    uint64_t id = form_value(form, "id", b, sizeof b) ? num(b, &ok1) : 0;
    if (!ok1 || !id || id > 999999) return err(r, out, cap, 400, "id=<the trash entry's number> expected"), true;
    if (!strcmp(path, "/bundle/trash/remove")) {   // for good: confirmed with the id again
      if (!form_value(form, "confirm", c, sizeof c) || num(c, &ok1) != id || !ok1)
        return err(r, out, cap, 400, "confirm=<the same id> expected (removed for good)"), true;
      if (!core::prjslot::request_remove((uint32_t)id)) return err(r, out, cap, 409, "busy: try again"), true;
      return text(r, out, cap, 200, "{\"ok\":true,\"msg\":\"removing\"}"), true;
    }
    a[0] = 0;
    if (form_value(form, "to", a, sizeof a)) {
      char k[core::kKeyMax];
      core::project_key(a, k, sizeof k);
      if (!a[0] || strcmp(k, a)) return err(r, out, cap, 400, "to=<a project key, @005> expected"), true;
    }
    bool rep = form_value(form, "replace", c, sizeof c) && c[0] == '1';
    if (!core::prjslot::request_restore((uint32_t)id, a, rep)) return err(r, out, cap, 409, "busy: try again"), true;
    return text(r, out, cap, 200, "{\"ok\":true,\"msg\":\"restoring\"}"), true;
  }
  if (!strcmp(path, "/bundle/trash/empty")) {   // for good: confirm=EMPTY
    if (!form_value(form, "confirm", c, sizeof c) || strcmp(c, "EMPTY")) return err(r, out, cap, 400, "confirm=EMPTY expected (every entry removed for good)"), true;
    if (!core::prjslot::request_empty()) return err(r, out, cap, 409, "busy: try again"), true;
    return text(r, out, cap, 200, "{\"ok\":true,\"msg\":\"emptying\"}"), true;
  }
  if (!strcmp(path, "/bundle/list")) {
    if (form_value(params, "src", c, sizeof c) && !strcmp(c, "usb")) {
      char d[kDriveMax];
      if (!form_value(params, "vol", d, sizeof d) && !first_stick(d)) return text(r, out, cap, 200, "[]"), true;
      if (!env().vol_ok || !env().vol_ok(d)) return text(r, out, cap, 200, "[]"), true;
      IoRule ru = {kPiece, kPiece, 0, ""};
      if (env().rules) env().rules(&ru);
      if (!ru.write_piece)   // a stick only at full rate (its transfers block core 0): asked again after STOP
        return err(r, out, cap, 503, "the USB stick is read when the Rytm stops (and nothing records)"), r->type = "application/json\r\nRetry-After: 2", true;
      int n = stick_list(d, (char*)out, (int)cap);
      if (n <= 0) return err(r, out, cap, 500, "too many bundles to list"), true;
      r->status = 200, r->type = "application/json", r->len = (uint32_t)n;
      return true;
    }
    int n = bundles_json("/Bundles", (char*)out, (int)cap);
    if (n <= 0) return err(r, out, cap, 500, "too many bundles to list"), true;
    r->status = 200, r->type = "application/json", r->len = (uint32_t)n;
    return true;
  }
  if (!strcmp(path, "/bundle/status")) return status_json(r, out, cap, true), true;
  if (!strcmp(path, "/bundle/plan")) {
    int n = plan_json((char*)out, (int)cap);
    if (n <= 0) return err(r, out, cap, 404, "no import planned"), true;
    r->status = 200, r->type = "application/json", r->len = (uint32_t)n;
    return true;
  }
  if (!strcmp(path, "/bundle/zip")) {
    bool ok1 = false, ok2 = true;
    uint64_t off = form_value(params, "off", b, sizeof b) ? num(b, &ok1) : 0;
    uint64_t len = kPiece;
    if (form_value(params, "len", b, sizeof b)) len = num(b, &ok2);
    if (!ok1 || !ok2 || !len) return err(r, out, cap, 400, "off= and len= expected"), true;
    if (len > kPiece) len = kPiece;
    if (len > cap) len = cap;
    uint32_t got = 0;
    if (!serve(off, out, (uint32_t)len, &got)) return err(r, out, cap, 404, "not serving (export again)"), true;
    r->status = 200, r->len = got;
    r->type = got ? "application/octet-stream\r\nCache-Control: no-store" : "application/octet-stream\r\nRetry-After: 1";
    return true;
  }
  if (!strcmp(path, "/bundle/export")) {
    if (!form_value(form, "key", a, sizeof a)) return err(r, out, cap, 400, "key= expected"), true;
    form_value(form, "dest", c, sizeof c) || (strcpy(c, "browser"), true);
    ExportOpts o;
    char t[4];
    if (form_value(form, "takes", t, sizeof t)) o.unused_takes = t[0] != '0';
    if (strcmp(c, "browser") && strcmp(c, "card") && strcmp(c, "usb")) return err(r, out, cap, 400, "dest=browser|card|usb"), true;
    int dest = !strcmp(c, "card") ? kToCard : !strcmp(c, "usb") ? kToVolume : kToBrowser;
    if (dest == kToVolume && !form_value(form, "vol", o.vol, sizeof o.vol) && !first_stick(o.vol)) strcpy(o.vol, "USB:");
    bool ok = start_export(a, dest, o);
    return status_json(r, out, cap, ok), r->status = ok ? 200 : 409, true;
  }
  if (!strcmp(path, "/bundle/verify")) {
    if (!form_value(form, "path", b, sizeof b) || !(bundle_path_ok(b) || stick_path_ok(b)))
      return err(r, out, cap, 400, "path=/Bundles/<name>.sidecar.zip or USB:/SIDECAR/<name>.sidecar.zip expected"), true;
    bool ok = start_verify(b);
    return status_json(r, out, cap, ok), r->status = ok ? 200 : 409, true;
  }
  if (!strcmp(path, "/bundle/import")) {
    ImportOpts o;
    if (form_value(form, "name", a, sizeof a)) {
      if (strlen(a) > 23) return err(r, out, cap, 400, "a project name has at most 23 characters"), true;
      strcpy(o.name, a);
    }
    if (form_value(form, "mode", c, sizeof c)) o.mode = !strcmp(c, "replace") ? kReplace : kNew;
    if (form_value(form, "packages", c, sizeof c)) o.packages = c[0] != '0';
    bool okn;
    if (form_value(form, "skip", c, sizeof c)) o.skip_slots = (uint8_t)num(c, &okn);
    bool has_slot = false;   // (present at all: an over-long value is refused too, never taken as absent)
    for (const char* p = form; *p && !has_slot; p = strchr(p, '&') ? strchr(p, '&') + 1 : "") has_slot = !strncmp(p, "slot=", 5);
    if (has_slot) {   // slot mode: the Rytm's slot it goes to (the plan's "slots")
      uint64_t sl = form_value(form, "slot", c, sizeof c) ? num(c, &okn) : (okn = false, 0);
      if (!okn || sl < 1 || sl > (uint64_t)core::prjslot::kSlots) return err(r, out, cap, 400, "slot=<1..128> expected"), true;
      o.slot = (int)sl;
    }
    bool ok = start_import(o);
    return status_json(r, out, cap, ok), r->status = ok ? 200 : 409, true;
  }
  if (!strcmp(path, "/bundle/cancel")) return cancel(), status_json(r, out, cap, true), true;
  if (!strcmp(path, "/bundle/finish")) return finish(), status_json(r, out, cap, true), true;
  if (!strcmp(path, "/bundle/remove")) {   // a bundle file (the lists' Delete): the card's or a stick's, .part too
    if (!form_value(form, "path", b, sizeof b)) return err(r, out, cap, 400, "path= expected"), true;
    const char* why = remove_bundle(b);
    if (why) return err(r, out, cap, 409, why), true;
    return status_json(r, out, cap, true), true;
  }
  if (!strcmp(path, "/bundle/delete")) {
    if (!form_value(form, "key", a, sizeof a) || !form_value(form, "confirm", b, sizeof b)) return err(r, out, cap, 400, "key= and confirm= expected"), true;
    bool ok = start_delete(a, b);
    return status_json(r, out, cap, ok), r->status = ok ? 200 : 409, true;
  }
  if (!strcmp(path, "/time")) {
    bool ok = false;
    uint64_t ep = form_value(form, "epoch", b, sizeof b) ? num(b, &ok) : 0;
    if (!ok || ep < 1600000000ull || ep > 4000000000ull) return err(r, out, cap, 400, "epoch=<seconds since 1970> expected"), true;
    int zone = core::projects::clock_zone();
    if (form_value(form, "tz", b, sizeof b)) {   // optional: minutes east of UTC (the browser's -getTimezoneOffset())
      bool neg = b[0] == '-', zok = false;
      uint64_t z = num(b + (neg ? 1 : 0), &zok);
      if (!zok || z > 840) return err(r, out, cap, 400, "tz=<minutes east of UTC, -840..840> expected"), true;
      zone = neg ? -(int)z : (int)z;
    }
    core::projects::set_clock((uint32_t)ep, core::projects::kClockBrowser);
    core::projects::set_clock_zone(zone);
    return text(r, out, cap, 200, "{\"ok\":true}"), true;
  }
  if (!strcmp(path, "/usb/status")) {
    Volume v[4];
    int n = env().volumes ? env().volumes(v, 4) : 0;
    JOut o;
    o.begin((char*)out, cap);
    o.raw("{\"present\":"), o.raw(n > 0 ? "true" : "false"), o.raw(",\"vols\":[");
    for (int i = 0; i < n; ++i) {
      if (i) o.raw(",");
      o.raw("{\"drive\":"), o.str(v[i].drive), o.raw(",\"name\":"), o.str(v[i].name), o.raw(",\"free\":"), o.num(v[i].free);
      o.raw(",\"total\":"), o.num(v[i].total), o.raw(",\"free_known\":"), o.raw(v[i].free_known ? "true" : "false"), o.raw("}");
    }
    o.raw("],\"job\":"), o.str(status().vol), o.raw("}");
    r->status = 200, r->type = "application/json", r->len = o.n;
    return true;
  }
  if (!strcmp(path, "/usb/eject")) {
    char d[kDriveMax];
    if (!form_value(form, "vol", d, sizeof d) && !first_stick(d)) return err(r, out, cap, 409, "no USB stick mounted"), true;
    const char* why = eject(d);
    if (why) return err(r, out, cap, 409, why), true;
    return text(r, out, cap, 200, "{\"ok\":true,\"msg\":\"safe to remove\"}"), true;
  }
  return false;
}

}  // namespace bundleweb
