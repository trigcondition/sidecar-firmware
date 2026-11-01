// core/device/device.cpp - see device.h. Per message, never per sample: cold.
#include "device/device.h"
#include "device/digitakt.h"
#include "device/strip.h"

#include <string.h>

#include "fmt/fmt.h"
#include "fx/out.h"
#include "host_link.h"
#include "platform.h"

namespace core {
namespace device {

// The Rytm's table: what the rest of the engine and rytm-mods 0021 / 0026 assume. Its reports are the engine's own, so
// map / fixed / out_allow are never read for it.
static constexpr Profile kRytm = {
    kIdRytm, "Analog Rytm MKII", 0x1935, 0x0b48,
    12, 8, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    kRemoteUi | kProject | kKits | kPatterns | kSendFx | kUserSlots | kSrcView | kFxIn | kPools,
    0, 0, {}, {}, {}, nullptr, nullptr, {},
    kBracketFull, &out::kRytmMk2, nullptr};
static_assert(kRytm.voices == kVoices, "the Rytm's voices are the engine's");
static_assert(kRytm.params == kMaxParams, "the Rytm's reports carry P1..P16");

static constexpr int kMax = 8;
static const Profile* s_list[kMax] = {&kRytm, &kDigitakt};
static int s_n = 2;
Link g_link = {};
static volatile bool s_rytm_usb;          // core 0 writes (rytm_on_usb), the control core reads
static bool s_said;                       // a box's hello was taken this boot
static bool s_by_usb;                     // this change of folder comes from the Rytm's USB id
static bool s_settle;                     // this boot came from one
static uint32_t s_boot_ms;
static constexpr char kDeviceFile[] = "/.rytm/device";   // the last box's id, decimal; none: the Rytm
// The Pi's own files, the same for every box: the network, the pairing, the screen, the black box, this choice.
static const char* const kMachine[] = {"wifi", "pair", "display", "prevboot", "blackbox", "device", "recspeed", "storbench", "wpa.conf"};

const Profile& rytm() { return kRytm; }
const Profile& active() { return g_link.box ? *g_link.box : kRytm; }
uint32_t dropped() { return g_link.dropped; }
void rytm_on_usb(bool on) { s_rytm_usb = on; }

CORE_COLD const Profile* by_usb(uint16_t vid, uint16_t pid) {
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->usb_vid == vid && s_list[k]->usb_pid == pid && s_list[k]->audio) return s_list[k];
  return nullptr;
}

void strip_tick(uint32_t us) { strip::tick(*g_link.box->controls, us); }

static inline bool is_sidecar(const uint8_t* d, int len) { return len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54; }

static CORE_COLD void filtered_out(const uint8_t* m, int n) {
  int c = n > 4 ? m[4] & 0x7f : 0;
  Link& l = g_link;   // (the hook has no link to pass)
  if (l.box && (l.box->out_allow[c >> 5] >> (c & 31) & 1)) {
    if (l.out) l.out(m, n);
  } else {
    ++l.dropped;
  }
}

CORE_COLD const char* state_path_box(const char* path, char* buf, int cap) {
  if (strncmp(path, "/.rytm", 6) || (path[6] && path[6] != '/')) return path;
  if (path[6])
    for (const char* k : kMachine)
      if (!strncmp(path + 7, k, strlen(k))) return path;
  sniprintf(buf, cap, "%s%s", g_link.state, path + 6);
  return buf;
}

// The folder p wants is another than the one this boot runs on.
static inline bool other_folder(const Link& l, const Profile* p) {
  const char* w = p ? p->state_dir : nullptr;
  return w != l.state && (!w || !l.state || strcmp(w, l.state));
}

// Boot, no voices yet: the box of the last run, its folder, its output filter (select's without the reset).
CORE_COLD void boot() {
  fmt::recover(kDeviceFile);
  File f;
  if (!f.open(kDeviceFile, kRead)) return;
  char b[8] = {0};
  int got = f.read(b, sizeof b - 1);
  f.close();
  s_settle = strstr(b, "usb") != nullptr, s_boot_ms = now_ms();
  int id = 0;
  for (int k = 0; k < got && b[k] >= '0' && b[k] <= '9'; ++k) id = id * 10 + b[k] - '0';
  for (int k = 1; k < s_n; ++k) {
    if (s_list[k]->id != id) continue;
    Link& l = g_link;
    l.state = s_list[k]->state_dir;
    if (l.state) Fs().mkdir(l.state);
    l.out = hooks.to_rytm;
    hooks.to_rytm = filtered_out;
    l.box = s_list[k];
    log(CORE_STR("device: %s, state in %s"), l.box->name, l.state ? l.state : "/.rytm");
  }
}

static CORE_COLD void change_folder(Link& l, const Profile* p) {
  if (l.saving) return;
  l.next = p, l.saving = true, l.posted = false;
  log(CORE_STR("device: %s wants its own state folder: the Sidecar restarts once the saves are done"), p ? p->name : kRytm.name);
}

CORE_COLD bool leaving() { return g_link.saving; }

CORE_COLD bool step(bool settled) {
  Link& l = g_link;
  if (s_rytm_usb && !s_said && !s_by_usb && !l.saving && hooks.device_restart && other_folder(l, nullptr)) {
    s_by_usb = true;
    change_folder(l, nullptr);
  }
  if (!l.saving || !settled) return false;
  if (!l.posted) {
    char t[16];
    uint32_t n = (uint32_t)sniprintf(t, sizeof t, s_by_usb ? "%u usb\n" : "%u\n", (unsigned)(l.next ? l.next->id : kIdRytm));
    l.posted = fmt::post_queue(kDeviceFile, t, n, &l.post_id);
    if (l.posted) return true;   // (refused: below, as a failed write)
  } else {
    fmt::PostResult r = fmt::post_result(l.post_id);
    if (r == fmt::kPostPending) return false;
    if (r == fmt::kPostOk) {
      l.saving = false;
      if (hooks.device_restart) hooks.device_restart();
      return false;
    }
  }
  // Not saved: no restart, it would only come back to the old folder and be asked again. A box's next hello tries again.
  l.saving = false, l.posted = false;
  log(CORE_STR("device: the choice of state folder was not saved: no restart"));
  return false;
}

static CORE_COLD void select(Link& l, const Profile* p) {   // p null: the Rytm
  if (p == l.box) return;
  if (!l.box) l.out = hooks.to_rytm;
  hooks.to_rytm = p ? filtered_out : l.out;
  l.box = p;
  log(CORE_STR("device: %s"), active().name);
  link_reset("device");   // the box before it: its notes and transport unknown
}

CORE_COLD void reset() {
  select(g_link, nullptr);
  strip::reset();
  s_rytm_usb = s_said = s_by_usb = s_settle = false;
  g_link.state = nullptr;   // (a board restart, for the tests: the choice is made once a boot)
}

CORE_COLD bool add(const Profile* p) {
  if (!p || p->voices < 1 || p->voices > kVoices || p->params > kMaxParams || s_n >= kMax) return false;
  for (int k = 0; k < kMaxParams; ++k)
    if (p->map[k] >= p->params) return false;
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->id == p->id) return false;
  s_list[s_n++] = p;
  return true;
}

CORE_COLD void hello(Link& l, const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return;
  int id = d[6] << 7 | d[7];
  const Profile* p = nullptr;
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->id == id) p = s_list[k];
  uint8_t st = !p ? kUnknown : d[8] != kProto ? kBadProto : kOk;
  const Profile* to = p == &kRytm ? nullptr : p;
  if (st == kOk && other_folder(l, to) && !hooks.device_restart) st = kUnknown;   // (a board that cannot restart: stays)
  else if (st == kOk && other_folder(l, to) && to && (s_rytm_usb || (s_settle && now_ms() - s_boot_ms < kSettleMs))) st = kWait;
  else if (st == kOk && other_folder(l, to)) {
    st = kRestart;
    change_folder(l, to);
  } else if (st == kOk) select(l, to), s_said = to != nullptr;
  if (st != kOk && st != kRestart && st != kWait) log(CORE_STR("device: hello from id %d proto %d: %s"), id, d[8], p ? "another protocol" : "no profile");
  const uint8_t a[10] = {0xF0, 0x7D, 0x52, 0x54, kCmdHello, 1, d[6], d[7], st, 0xF7};
  void (*out)(const uint8_t*, int) = l.box ? l.out : hooks.to_rytm;   // the answer is never filtered
  if (out) out(a, (int)sizeof a);
}

// The table's translation: the box's message into the engine's 0x04 / 0x03 (P1..P16, the sound tag) or 0x11.
static CORE_COLD int table(const Profile& p, const uint8_t* d, int len, uint8_t* m) {
  if (len < 7 || d[1] != p.head[0] || d[2] != p.head[1] || d[3] != p.head[2]) return 0;
  int cmd = d[4], track = d[5];
  if (track >= p.voices) return 0;
  if (cmd == p.cmd_note_off) {
    if (len != 7) return 0;
    const uint8_t off[7] = {0xF0, 0x7D, 0x52, 0x54, 0x11, (uint8_t)track, 0xF7};
    memcpy(m, off, 7);
    return 7;
  }
  bool trig = cmd == p.cmd_trig;
  if (!trig && cmd != p.cmd_params) return 0;
  int from = trig ? 8 : 6;
  if (len != from + 2 * p.params + 1) return 0;
  int n = 0;
  m[n++] = 0xF0, m[n++] = 0x7D, m[n++] = 0x52, m[n++] = 0x54;
  m[n++] = trig ? 0x04 : 0x03;
  m[n++] = (uint8_t)(p.machine | track);
  if (trig) m[n++] = d[6], m[n++] = d[7];
  for (int k = 0; k < kMaxParams; ++k) {
    int s = p.map[k];
    m[n++] = s >= 0 ? d[from + 2 * s] : p.fixed[k];
    m[n++] = s >= 0 ? d[from + 2 * s + 1] : 0;
  }
  m[n++] = (p.sound_tag >> 7) & 0x7f;
  m[n++] = p.sound_tag & 0x7f;
  m[n++] = 0xF7;
  return n;
}

CORE_COLD bool translate(Link& l, const uint8_t*& d, int& len, uint8_t* m) {
  if (len < 5 || d[len - 1] != 0xF7) return ++l.dropped, false;
  if (is_sidecar(d, len) && d[4] == kCmdHello) return hello(l, d, len), false;
  if (is_sidecar(d, len) && d[4] == 0x35 && !(l.box->features & kRemoteUi)) {   // a Rytm's USB port came up
    if (other_folder(l, nullptr)) {
      if (hooks.device_restart) change_folder(l, nullptr);
      return ++l.dropped, false;
    }
    select(l, nullptr);
    return true;
  }
  if (is_sidecar(d, len) && d[4] == kCmdControls && l.box->controls) {
    const Controls& c = *l.box->controls;
    if (len != 7 + 2 * c.n || d[5] >= l.box->voices) return ++l.dropped, false;
    strip::report(c, d[5], d + 6);
    return false;
  }
  if (is_sidecar(d, len) && (l.box->in_allow[(d[4] & 0x7f) >> 5] >> (d[4] & 31) & 1)) {
    bool voiced = d[4] == 0x03 || d[4] == 0x04 || d[4] == 0x11;
    if (voiced && (d[5] & 7) >= l.box->voices) return ++l.dropped, false;
    if (l.box->controls && d[4] == 0x04) strip::trig(d[5] & 7);
    if (l.box->controls && d[4] == 0x11) strip::off(d[5] & 7);
    return true;
  }
  int n = l.box->translate ? l.box->translate(*l.box, d, len, m) : table(*l.box, d, len, m);
  if (n <= 0) return ++l.dropped, false;
  d = m, len = n;
  return true;
}

}  // namespace device
}  // namespace core
