// pi/engine/box_list.h - the boxes on the USB-A ports, for /status "boxes" (docs/device-profiles.md, "What the page is told"): one list, so two of a kind
// are two rows, each with its own key: type and the box's DeviceUID when known (two of one type, design 2026-11-26; "uid" and its
// name "label" in the row), else type and Circle's umidiN, which holds while the box stays plugged in. The served box is always in it, on:0 while it is unplugged. A row
// carries its profile's counts and feature bits when the type has a profile. No allocation, no Circle headers. Portable: tests/test_usb_box.cpp.
#pragma once
#include <stdint.h>
#include <string.h>

enum TBoxRole : uint8_t {
  BoxHost,        // the host link: the box the Sidecar serves
  BoxWaiting,     // its mod said hello and was told to wait for the box in use
  BoxListening,   // heard for a hello, none so far: a stock box
  BoxSpare,       // a box Sidecar knows, not used: one box at a time
  BoxOther        // an Elektron box with no profile
};

// The type's id (core/device/device.h), from the same USB string usb_box reads; 0 none.
inline uint16_t usb_box_id(const char* product) {
  if (product == 0) return 0;
  if (strstr(product, "Rytm") != 0) return 1;
  if (strstr(product, "Digitakt") != 0 && strstr(product, "Digitakt II") == 0) return 2;
  if (strstr(product, "Analog Four MKII") != 0) return 3;
  return 0;
}

struct BoxCaps {
  uint8_t tracks, voices, params;
  uint32_t features;      // core::device::Feature
  const char* state;      // its folder on the card; 0: /.rytm
};

class BoxList {
 public:
  static constexpr unsigned kMax = 8;
  using Lookup = bool (*)(uint16_t id, BoxCaps* caps);

  void clear() { n_ = 0; }
  // In order of type and umidiN, so a row keeps its place from one scan to the next.
  void add(uint16_t id, uint8_t slot, TBoxRole role, const char* name, uint32_t uid = 0, const char* label = 0) {
    if (n_ >= kMax) return;
    unsigned at = n_;
    while (at > 0 && rank(id, slot) < rank(r_[at - 1].id, r_[at - 1].slot)) --at;
    memmove(&r_[at + 1], &r_[at], (n_ - at) * sizeof r_[0]);
    Row& r = r_[at];
    r.id = id, r.slot = slot, r.role = role;
    strncpy(r.name, name != 0 ? name : "", sizeof r.name - 1);
    r.name[sizeof r.name - 1] = 0;
    r.uid = uid;
    strncpy(r.label, label != 0 ? label : "", sizeof r.label - 1);
    r.label[sizeof r.label - 1] = 0;
    ++n_;
  }

  // ,"boxes":[...] at out, at most cap bytes with its 0; the length, 0 if it does not fit. served: the type this boot serves.
  unsigned json(char* out, unsigned cap, uint16_t served, const char* served_name, Lookup lookup) const {
    Out o = {out, cap, 0, true};
    o.s(",\"boxes\":[");
    bool here = false;
    for (unsigned k = 0; k < n_; ++k) here |= r_[k].role == BoxHost;
    unsigned same = 0;
    for (unsigned k = 0; k < n_; ++k) {
      same = k > 0 && r_[k - 1].id == r_[k].id ? same + 1 : 1;
      if (k) o.s(",");
      row(o, r_[k], same, true, lookup);
    }
    if (!here) {
      Row r = {};
      r.id = served, r.role = BoxHost;
      strncpy(r.name, served_name != 0 ? served_name : "", sizeof r.name - 1);
      if (n_) o.s(",");
      row(o, r, 1, false, lookup);
    }
    o.s("]");
    if (!o.fits || o.n >= cap) return 0;
    out[o.n] = 0;
    return o.n;
  }

 private:
  struct Row {
    uint16_t id;
    uint8_t slot;         // umidiN; 0 the served box, unplugged
    TBoxRole role;
    char name[32];        // USB's product string: "Elektron Analog Four MKII"; 9 full rows still fit /status's 1536 B
    uint32_t uid;         // its DeviceUID, 0 not known (its row keyed by umidiN)
    char label[24];       // its name (the UID folder's name.txt), "" none
  };
  struct Out {
    char* p;
    unsigned cap, n;
    bool fits;
    void c(char x) {
      if (n + 1 < cap) p[n++] = x;
      else fits = false;
    }
    void s(const char* t) {
      while (*t) c(*t++);
    }
    void u(uint32_t v) {
      char b[10];
      int k = 0;
      do b[k++] = (char)('0' + v % 10), v /= 10;
      while (v);
      while (k) c(b[--k]);
    }
    void x8(uint32_t v) {   // 8 lowercase hex digits (a UID, as its folder's name)
      for (int k = 28; k >= 0; k -= 4) c("0123456789abcdef"[v >> k & 15]);
    }
    void text(const char* t) {   // a USB string is the box's own: nothing in it may end the JSON string
      c('"');
      for (; *t; ++t)
        if (*t >= 0x20 && *t != '"' && *t != '\\' && *t != 0x7f) c(*t);
      c('"');
    }
  };

  static uint32_t rank(uint16_t id, uint8_t slot) { return (uint32_t)id << 8 | slot; }

  static void row(Out& o, const Row& r, unsigned same, bool on, Lookup lookup) {
    static const char* const kRole[] = {"host", "waiting", "listening", "spare", "other"};
    o.s("{\"key\":\"");
    o.u(r.id), o.c('-');
    if (r.uid) o.x8(r.uid);
    else o.u(r.slot);
    o.s("\",\"id\":"), o.u(r.id);
    if (r.uid) o.s(",\"uid\":\""), o.x8(r.uid), o.c('"');
    if (r.label[0]) o.s(",\"label\":"), o.text(r.label);
    o.s(",\"n\":"), o.u(same);
    o.s(",\"name\":"), o.text(r.name);
    o.s(",\"role\":\""), o.s(kRole[r.role]);
    o.s("\",\"on\":"), o.u(on);
    BoxCaps c;
    if (r.id && lookup && lookup(r.id, &c)) {
      o.s(",\"tracks\":"), o.u(c.tracks);
      o.s(",\"voices\":"), o.u(c.voices);
      o.s(",\"params\":"), o.u(c.params);
      o.s(",\"feat\":"), o.u(c.features);
      o.s(",\"state\":"), o.text(c.state ? c.state : "/.rytm");
    }
    o.s("}");
  }

  Row r_[kMax];
  unsigned n_ = 0;
};
