// pi/engine/tests/test_ob_mirror.cpp - usb=mirror's box side in the real ob_usb.cpp (CIRCLE_GADGET_MIRROR) on the fake Circle of shim_ob/
// (a recording gadget, sidecar_gadget.h): the pipes opened at attach, a halted pipe cleared and resubmitted by
// MirrorPipePoll and its backoff, MirrorPipePoll leaving EP0 alone while a forwarded request is on it, a forwarded
// request's answer, and the attach generation that drops a completion of an earlier attach. The configuration is the
// Rytm's that test_ob_usb.cpp uses (the emulator's; the pipes on interface 3, USB MIDI on 4-5).
#include "check.h"
#include "device/digitakt.h"
#include "fx/out.h"
#include "ob_block.h"
#include "ob_usb.h"
#include "ob_follow.h"
#include "shared.h"
#include "sidecar_gadget.h"
#include <circle/timer.h>
#include <circle/usb/usbmidi.h>
#include <circle/usb/usbstandardhub.h>

#include <circle/koptions.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>

CLogger* CLogger::Get() { static CLogger l; return &l; }
void CLogger::Write(const char*, int, const char* f, ...) {
  char b[512];
  va_list a;
  va_start(a, f);
  vsnprintf(b, sizeof b, f, a);
  va_end(a);
  lines.push_back(b);
}
#include "device/a4.h"
namespace core { namespace device {
static bool g_a4_chosen = false;      // its hello chose it (device.cpp: audio_by_hello)
const Profile& active() {             // (only ob_follow.cpp asks)
  static Profile none;
  return g_a4_chosen ? kA4 : none;
}
const Profile* by_usb(uint16_t vid, uint16_t pid, bool without_hello) {   // device.cpp's list, without the engine it links
  static Profile rytm;
  rytm.audio = &out::kRytmMk2;
  if (vid == 0x1935 && pid == 0x0b48) return &rytm;
  if (vid == kA4Vid && pid == kA4Pid) return g_a4_chosen || without_hello ? &kA4 : nullptr;
  return vid == kDigitakt.usb_vid && pid == kDigitakt.usb_pid ? &kDigitakt : nullptr;
}
}}
static int g_rytm_usb = 0;   // the Rytm's slot held (usb_attach / usb_detach)
// device.cpp's attach order without the engine: a box's slot already, else the lowest free one; freed when its device goes
static const core::device::Profile* g_slot_of[2];
namespace core { namespace device {
uint32_t g_attach_uid = 0;   // the UID the last usb_attach was given
int g_midi_slot[2] = {-1, -1};   // set_midi_slot's
void set_midi_slot(int t, int s) { if (t >= 0 && t < 2) g_midi_slot[t] = s; }
int usb_attach(const Profile* p, uint32_t uid) {
  g_attach_uid = uid;
  for (int k = 0; k < 2; ++k)
    if (g_slot_of[k] == p) return k;
  for (int k = 0; k < 2; ++k)
    if (!g_slot_of[k]) {
      g_slot_of[k] = p;
      if (p->audio == &out::kRytmMk2) ++g_rytm_usb;
      return k;
    }
  return -1;
}
void usb_detach(int s) {
  if (s < 0 || s > 1 || !g_slot_of[s]) return;
  if (g_slot_of[s]->audio == &out::kRytmMk2) --g_rytm_usb;
  g_slot_of[s] = nullptr;
}
}}
CUSBFunction* RytmObDeviceCreate(CUSBFunction* p);
extern "C" int core_snprintf(char* b, unsigned long n, const char* f, ...) {
  va_list a;
  va_start(a, f);
  int r = vsnprintf(b, n, f, a);
  va_end(a);
  return r;
}

// The Rytm's, the same as teensy/libraries/RytmHost/extras/ob_block_test.cpp's; the Digitakt's differs in OUT alt 3's size alone.
static uint8_t g_rytm[263] = {
    0x09, 0x02, 0x07, 0x01, 0x06, 0x01, 0x00, 0xc0, 0x00, 0x08, 0x0b, 0x00, 0x04, 0xff, 0x00, 0x00, 0x00, 0x09,
    0x04, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x09, 0x04, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x09,
    0x04, 0x01, 0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x01, 0x09, 0x04, 0x01,
    0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x58, 0x00, 0x01, 0x09, 0x04, 0x01, 0x03, 0x01,
    0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x70, 0x01, 0x01, 0x09, 0x04, 0x02, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x09, 0x04, 0x02, 0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x01,
    0x09, 0x04, 0x02, 0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x58, 0x00, 0x01, 0x09, 0x04,
    0x02, 0x03, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x70, 0x01, 0x01, 0x09, 0x04, 0x03, 0x00,
    0x02, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00, 0x07, 0x05, 0x82, 0x03, 0x40, 0x00,
    0x01, 0x08, 0x0b, 0x04, 0x02, 0x01, 0x01, 0x00, 0x00, 0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x05, 0x09, 0x04, 0x05, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00, 0x06, 0x24, 0x02, 0x01, 0x01, 0x00, 0x06, 0x24, 0x02, 0x02, 0x02,
    0x00, 0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00, 0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01,
    0x00, 0x09, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x01, 0x09, 0x05, 0x81,
    0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x03};

// leak checks on: the driver frees its URBs, endpoints and buffers when the box goes, so a leak here is a bug
extern "C" const char *__asan_default_options () { return "detect_leaks=1"; }

static CSidecarGadget g_gadget;

// the A4's: the Rytm's with alt 3's sizes changed (as test_ob_usb.cpp's), made in main
static uint8_t g_a4[263];
static const int kOutAlt3Mps = 137, kInAlt3Mps = 80;

struct Box {
  CUSBDevice dev;
  CUSBHostController host;
  CUSBFunction parent{&dev, &host};
  CUSBFunction* ob = nullptr;
  explicit Box(bool a4 = false) {
    dev.config = a4 ? g_a4 : g_rytm;
    dev.desc.idVendor = a4 ? core::device::kA4Vid : 0x1935, dev.desc.idProduct = a4 ? core::device::kA4Pid : 0x0b48;
    ob = RytmObDeviceCreate(&parent);
    CHECK(ob && ob->Configure());
  }
  CObUsb* box() { return static_cast<CObUsb*>(ob); }
  ~Box() { delete ob; }
  // the pipe's URB in flight (IN: one at a time), or null
  CUSBRequest* pipe(uint8_t addr) {
    auto& q = addr & 0x80 ? host.in : host.out;
    for (CUSBRequest* u : q) if (u->ep && u->ep->desc && u->ep->desc->bEndpointAddress == addr) return u;
    return nullptr;
  }
  // the box answers it (ok: a packet of n bytes; else the transfer failed: the endpoint halted), as the xHCI IRQ would
  void complete(CUSBRequest* u, bool ok, unsigned n = 0) {
    for (auto* q : {&host.in, &host.out, &host.ep0})
      for (auto i = q->begin(); i != q->end(); ++i) if (*i == u) { q->erase(i); break; }
    u->SetStatus(ok ? 1 : 0), u->SetResultLen(ok ? n : 0);
    u->fn(u, u->param, u->ctx);
  }
  int clears(uint8_t addr) {
    int n = 0;
    for (const TCtl& c : host.ctl)
      n += c.type == (REQUEST_OUT | REQUEST_TO_ENDPOINT) && c.req == CLEAR_FEATURE && c.value == ENDPOINT_HALT && c.index == addr;
    return n;
  }
};

static void poll_at(unsigned ticks) { CTimer::s_ticks = ticks; CObUsb::MirrorPipePoll(); }
static bool logged(const char* s) {
  for (const std::string& l : CLogger::Get()->lines) if (l.find(s) != std::string::npos) return true;
  return false;
}

Output g_OutputSide;   // the side stage (side_out.h): a box beside the Rytm pulls it

void ObUsbTestRelayFirst(bool);   // ob_usb.cpp (OB_USB_TEST): cmdline mirrorfirst=relay

// a Teensy relay as the second mirror target: notes what it is handed
struct FakeRelay : IMirrorTarget {
  CObUsb* box = nullptr;
  std::vector<int> up;
  int moved = 0, strings = 0;
  bool shown = true, hold_pipe_in = false;
  std::vector<u8> pipe_in;
  void SetBox(CObUsb* p) override { box = p; if (!p) up.push_back(0); }
  boolean Mirror() const override { return TRUE; }
  boolean MirrorShown() const override { return shown; }
  boolean MirrorSetDescriptors(const u8*, const u8*, unsigned) override { return TRUE; }
  void MirrorSetString(u8, const u8*, unsigned) override { ++strings; }
  void MirrorBoxUp(boolean b) override { up.push_back(b); }
  void MirrorControlDone(unsigned, const u8*, int) override {}
  boolean MirrorPipeIn(u8 a, const u8*, unsigned) override { pipe_in.push_back(a); return hold_pipe_in; }
  void MirrorPipeOutDone(u8) override {}
  boolean MirrorOutExtra(u8*, u16) override { return FALSE; }
  void FromBoxBlock(const u8*) override {}
  void ToBox(s32 (*)[12], const s16 (*)[8], unsigned) override {}
  void MirrorMoved() override { ++moved; }
};

int main() {
  CSidecarGadget::s_pThis = &g_gadget;
  memcpy(g_a4, g_rytm, sizeof g_rytm);
  g_a4[kInAlt3Mps] = 0x00, g_a4[kInAlt3Mps + 1] = 0x01;
  g_a4[kOutAlt3Mps] = 0xc8, g_a4[kOutAlt3Mps + 1] = 0x00;
  CTimer::s_ticks = 1000;
  CKernelOptions::Get()->num["obgate"] = 0;   // the attach gate off (a box shown at once) but in its own block below

  {  // attach: the pipes of interface 3 (bulk OUT 0x02, interrupt IN 0x82), nothing on the audio interfaces' alternates
     // and no SET_INTERFACE beyond the stream's four (interface 3 has only alt 0); MIDI is not a pipe
    Box b;
    CHECK(g_gadget.box.size() == 1 && g_gadget.box.back() == 1);
    CHECK(b.host.ctl.size() == 4);
    CUSBRequest* in = b.pipe(0x82);
    CHECK(in && in->len == 64 && b.pipe(0x81) == nullptr);
    CHECK(logged("usb=mirror: 0 strings (language 0409), 2 pipes"));

    // a packet of the box: to the gadget, the next IN request at once (the gadget did not keep it)
    b.complete(in, true, 12);
    CHECK(g_gadget.pipe_in.size() == 1 && g_gadget.pipe_in[0].addr == 0x82 && g_gadget.pipe_in[0].n == 12);
    CHECK(b.pipe(0x82) == in);

    // the computer's pipe OUT packet: one at a time, the gadget told when the box has it
    static const u8 pkt[10] = {1, 2, 3};
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    CUSBRequest* out = b.pipe(0x02);
    CHECK(out && out->len == 10 && memcmp(out->buf, pkt, 3) == 0);
    CHECK(!CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));   // busy: the gadget keeps it
    b.complete(out, true, 10);
    CHECK(g_gadget.pipe_out_done.size() == 1 && g_gadget.pipe_out_done[0] == 0x02);

    // MirrorPipePoll with nothing halted: nothing on EP0
    poll_at(2000);
    CHECK(b.clears(0x82) == 0 && CObUsb::MirrorPipeResets() == 0);

    // the IN pipe halts (the box STALLed it): not resubmitted until MirrorPipePoll has cleared it on the box and on the
    // xHC (Reset Endpoint, as the endpoint context is Halted: Stopped after it, as on Circle); then running again,
    // logged once
    in = b.pipe(0x82);
    in->ep->GetXHCIEndpoint()->state = 2;
    b.complete(in, false);
    CHECK(b.pipe(0x82) == nullptr && g_gadget.pipe_in.size() == 1);
    poll_at(2000);
    CHECK(b.clears(0x82) == 1 && in->ep->GetXHCIEndpoint()->resets == 1 && in->ep->GetXHCIEndpoint()->state == 3);
    CHECK(b.pipe(0x82) != nullptr && CObUsb::MirrorPipeResets() == 1);
    CHECK(logged("usb=mirror: pipe 0x82 halted: cleared, running again"));

    // an OUT pipe halts: the computer's packets are refused (the gadget drops them) until it is cleared; not resubmitted
    // (the computer's next packet is the next transfer)
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    out = b.pipe(0x02);
    b.complete(out, false);
    CHECK(g_gadget.pipe_out_done.size() == 2);   // the gadget still takes the computer's next one
    CHECK(!CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    poll_at(2100);
    CHECK(b.clears(0x02) == 1 && b.pipe(0x02) == nullptr);
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, sizeof pkt));
    b.complete(b.pipe(0x02), true, 10);

    // a box that keeps failing: every 100 ms (HZ / 10) for the first five tries, then once a second (a good packet
    // first: the count of tries starts again)
    b.complete(b.pipe(0x82), true, 1);
    in = b.pipe(0x82);
    CXHCIEndpoint* x = in->ep->GetXHCIEndpoint();
    // Reset Endpoint refused: Circle's ResetFromHalted still says TRUE and the context stays Halted (2). The pipe stays
    // stopped (nothing submitted into the halted ring, where it would never complete and leave the pipe busy for good),
    // logged once as refused, never as running again, and tried again after the backoff
    x->state = 2, x->reset_ok = false;
    b.complete(in, false);
    size_t nRunning = std::count_if(CLogger::Get()->lines.begin(), CLogger::Get()->lines.end(),
                                    [](const std::string& l) { return l.find("running again") != std::string::npos; });
    unsigned t = 3000;
    poll_at(t);
    CHECK(b.clears(0x82) == 2 && x->resets == 2 && x->state == 2 && b.pipe(0x82) == nullptr);
    CHECK(logged("usb=mirror: pipe 0x82 halted: the host refused its reset (state 2), trying again"));
    for (int k = 2; k <= 5; ++k) {
      poll_at(t + 9);                             // too soon
      CHECK(b.clears(0x82) == k);
      t += 10;
      poll_at(t);
      CHECK(b.clears(0x82) == k + 1);
    }
    poll_at(t + 10);                              // five tries in: 100 ms is no longer enough
    poll_at(t + 99);
    CHECK(b.clears(0x82) == 6);
    t += 100;
    poll_at(t);
    CHECK(b.clears(0x82) == 7 && b.pipe(0x82) == nullptr && x->resets == 7 && x->state == 2);
    CHECK(std::count_if(CLogger::Get()->lines.begin(), CLogger::Get()->lines.end(),
                        [](const std::string& l) { return l.find("running again") != std::string::npos; }) == (long) nRunning);

    // a forwarded request holds EP0: MirrorPipePoll waits for its answer before it clears anything
    x->reset_ok = true;
    TSetupData s = {0xC0, 0x01, 0, 0, 16};        // a vendor IN request
    CHECK(CObUsb::MirrorControl(&s, nullptr, 41));
    CHECK(b.host.ep0.size() == 1 && b.host.ep0[0]->len == 16 && b.host.ep0[0]->GetSetupData()->bRequest == 0x01);
    CHECK(!CObUsb::MirrorControl(&s, nullptr, 42));   // one at a time
    t += 100;
    poll_at(t);
    CHECK(b.clears(0x82) == 7);
    b.complete(b.host.ep0[0], true, 5);
    CHECK(g_gadget.done.size() == 1 && g_gadget.done[0].seq == 41 && g_gadget.done[0].n == 5);
    poll_at(t);
    CHECK(b.clears(0x82) == 8 && b.pipe(0x82) != nullptr && x->state == 3);   // the reset took: running again, logged
    CHECK(std::count_if(CLogger::Get()->lines.begin(), CLogger::Get()->lines.end(),
                        [](const std::string& l) { return l.find("running again") != std::string::npos; }) == (long) nRunning + 1);

    // the box STALLs a forwarded request: the gadget is told (< 0), and STALLs it to the computer
    CHECK(CObUsb::MirrorControl(&s, nullptr, 43));
    b.complete(b.host.ep0[0], false);
    CHECK(g_gadget.done.size() == 2 && g_gadget.done[1].seq == 43 && g_gadget.done[1].n == -1);
    TSetupData z = {0xC0, 0x01, 0, 0, 0};          // IN with no data stage: never forwarded
    CHECK(!CObUsb::MirrorControl(&z, nullptr, 44) && b.host.ep0.empty());

    // the box goes away with a pipe IN request, a pipe OUT packet and a forwarded request in flight. On the Pi they
    // never complete (~CXHCIUSBDevice has freed the slot before ~CUSBDevice deletes the driver), so ~CObUsb frees them
    // all, with the stream's URBs: back to no CUSBRequest at all (2026-11-09: they were leaked, ~17 a Rytm restart, and
    // Circle's 256 ran out after ~15 restarts on one boot)
    in = b.pipe(0x82);
    CHECK(CObUsb::MirrorPipeOut(0x02, pkt, 4));
    CHECK(CObUsb::MirrorControl(&s, nullptr, 45));
    CUSBRequest* ctl = b.host.ep0[0];
    CHECK(b.pipe(0x02) != nullptr);
    void *in_param = in->param, *ctl_param = ctl->param;      // the old attach's, for the generation check below
    TURBCompletionRoutine *in_fn = in->fn, *ctl_fn = ctl->fn;
    b.host.in.clear(), b.host.out.clear(), b.host.ep0.clear();     // the controller forgets them (the slot is gone)
    unsigned nFreed = CObUsb::FreedInFlight(), nDet = CObUsb::Detaches();
    delete b.ob;
    b.ob = nullptr;
    CHECK(CUSBRequest::live() == 0);
    CHECK(CObUsb::Detaches() == nDet + 1);
    CHECK(CObUsb::FreedInFlight() - nFreed == 8 + 3);   // the 8 IN stream URBs (paced OUT: none in flight), pipe IN, OUT, EP0
    CHECK(logged("box gone: 11 URBs in flight freed, 0 kept (USB requests now 0)"));
    CHECK(g_gadget.box.size() == 2 && g_gadget.box.back() == 0);
    CHECK(CObUsb::MirrorPipeResets() == 9);   // every clear sent, both pipes (the mirror line's "reset")

    Box c;                                       // the box back: a new CObUsb, a new generation
    CHECK(g_gadget.box.back() == 1);
    CUSBRequest* nin = c.pipe(0x82);
    CHECK(nin);
    // a completion of the old attach with the new object as its context (the Pi's heap can put the new one at the old
    // one's address; a defence only now, since no old completion comes after the slot is freed): ignored (no packet to
    // the gadget, nothing resubmitted, the new pipe untouched). Stand-ins carry the old attach's parameters.
    size_t nPkt = g_gadget.pipe_in.size(), nIn = c.host.in.size(), nDone = g_gadget.done.size();
    CUSBRequest* old_in = new CUSBRequest(nin->ep, nin->buf, 64);
    old_in->SetCompletionRoutine(in_fn, in_param, nin->ctx);
    old_in->SetStatus(1), old_in->SetResultLen(7);
    old_in->fn(old_in, old_in->param, old_in->ctx);
    CHECK(g_gadget.pipe_in.size() == nPkt && c.host.in.size() == nIn && c.pipe(0x82) == nin);
    old_in->SetStatus(0);                        // nor does an error mark the new pipe halted
    old_in->fn(old_in, old_in->param, old_in->ctx);
    poll_at(t + 1000);
    CHECK(c.clears(0x82) == 0);
    delete old_in;
    TSetupData* old_s = new TSetupData(s);
    CUSBRequest* old_ctl = new CUSBRequest(nin->ep, nin->buf, 4, old_s);
    old_ctl->SetCompletionRoutine(ctl_fn, ctl_param, nin->ctx);
    old_ctl->SetStatus(1), old_ctl->SetResultLen(4);
    old_ctl->fn(old_ctl, old_ctl->param, old_ctl->ctx);   // the old request's answer: not passed on (deletes the URB)
    CHECK(g_gadget.done.size() == nDone);
    CHECK(CObUsb::MirrorControl(&s, nullptr, 46));   // and EP0 of the new attach is free
    c.complete(c.host.ep0[0], true, 3);
    CHECK(g_gadget.done.size() == nDone + 1 && g_gadget.done.back().seq == 46);
    // the new attach's own completion still goes through
    c.complete(nin, true, 9);
    CHECK(g_gadget.pipe_in.size() == nPkt + 1 && g_gadget.pipe_in.back().n == 9);
  }
  CHECK(CUSBRequest::live() == 0);

  {  // 40 Rytm restarts on one boot (the test target on a unit): each leaves with everything in flight that can be, and
     // the count of CUSBRequest instances is the same after every one
    unsigned nDet = CObUsb::Detaches();
    for (int k = 0; k < 40; k++) {
      Box b;
      CHECK(CUSBRequest::live() == 8 + 8 + 1);   // 8 IN + 8 OUT stream URBs, the pipe IN's
      static const u8 pkt[4] = {1, 2, 3, 4};
      TSetupData s = {0xC0, 0x01, 0, 0, 16};
      CHECK(CObUsb::MirrorPipeOut(0x02, pkt, 4) && CObUsb::MirrorControl(&s, nullptr, 100 + k));
      CHECK(CUSBRequest::live() == 8 + 8 + 1 + 2);
      b.host.in.clear(), b.host.out.clear(), b.host.ep0.clear();
      delete b.ob;
      b.ob = nullptr;
      CHECK(CUSBRequest::live() == 0);
    }
    CHECK(CObUsb::Detaches() == nDet + 40);
  }

  {  // The Pi 5 hardware order at a boot with usb=mirror and the box plugged in: the box is configured and read while
     // the computer still sees the Sidecar device (MirrorAttach), the gadget leaves the bus and comes back as the box
     // (MirrorPoll: shown), the computer's SET_CONFIGURATION activates the box's endpoints, and only then does the
     // computer's Overbridge driver start its handshake on the async channel. The box has an async IN packet waiting from
     // the Pi's own attach. Its IN request must be out from the attach and that packet drained while the Sidecar device
     // is shown ("parked", as 428907d dropped it: "drop 1"); left in the box, it is the computer's first async packet,
     // ahead of its own handshake (4ecee9a: "pipe in 1"). And nothing may hold the pipe once the box is shown: 3b80b2f's
     // latched gate waited for CMirrorEP::OnActivate, which the hardware logs cannot show fired (rc3: parked 1, pipe in 0);
     // whether the box's packets reach the gadget now follows only what the computer sees.
    g_gadget.mirror = true;
    g_gadget.hold_pipe_in = false;
    auto packet = [&](Box& b, unsigned n) {    // the box answers the pipe IN request in flight, if any
      CUSBRequest* u = b.pipe(0x82);
      if (u == nullptr) return false;
      b.complete(u, true, n);
      return true;
    };
    const unsigned kStale = 33, kReply = 12;
    g_gadget.shown = false;                    // the Sidecar device until the box has been read and the switch made
    size_t nPkt = g_gadget.pipe_in.size();
    unsigned nParked = CObUsb::MirrorParked();
    Box b;                                     // the box configured and read (MirrorAttach)
    CHECK(packet(b, kStale));                  // its request is out: the waiting packet comes now
    CHECK(g_gadget.pipe_in.size() == nPkt);    // not to the gadget
    CHECK(CObUsb::MirrorParked() == nParked + 1 && b.pipe(0x82) != nullptr);   // drained, renewed
    poll_at(50000);                            // nothing on EP0 while the Sidecar device is shown
    CHECK(b.clears(0x82) == 0);
    // the switch (SoftDisconnect, 300 ms, SoftConnect), the hub and the computer's enumeration, SET_CONFIGURATION
    g_gadget.shown = true;
    CHECK(packet(b, kReply));                  // the computer's handshake: the box's reply
    CHECK(g_gadget.pipe_in.size() == nPkt + 1);
    CHECK(g_gadget.pipe_in[nPkt].n == kReply); // the computer's first async packet is the box's reply, not the stale one
    CHECK(b.pipe(0x82) != nullptr);            // and the pipe goes on (the gadget did not keep it: renewed)
    for (int i = 0; i < 100; i++) CHECK(packet(b, 20));
    CHECK(g_gadget.pipe_in.size() == nPkt + 101 && CObUsb::MirrorParked() == nParked + 1);
    // CLASS COMPLIANT (back as the Sidecar device): drained again, counted as parked, never stuck
    g_gadget.shown = false;
    for (int i = 0; i < 50; i++) CHECK(packet(b, 20));
    CHECK(g_gadget.pipe_in.size() == nPkt + 101 && CObUsb::MirrorParked() == nParked + 51);
    g_gadget.shown = true;
    CHECK(packet(b, kReply) && g_gadget.pipe_in.size() == nPkt + 102);
  }

  {  // The Rytm's USB MIDI OUT halts while a forwarded request of the computer is at the box (its EP0, async): the MIDI
     // reset (circle-midi-reset2: a synchronous CLEAR_FEATURE on the same EP0) waits until the request is answered. Sent
     // meanwhile, it would queue behind the forwarded URB, and if the box never answers, Circle's 3 s timeout drops the
     // forwarded URB's slot and leaves the caller's stack URB on the ring, to complete later into a dead object.
    g_gadget.mirror = true, g_gadget.shown = true;
    CTimer::s_now = 1000;
    Box b;
    CObUsbTransport t;
    CUSBMIDIDevice m;
    t.SetMIDI(&m);
    TSetupData s = {0xC0, 0x01, 0, 0, 16};
    CHECK(CObUsb::MirrorControl(&s, nullptr, 60) && CObUsb::MirrorCtlBusy());
    const std::vector<uint8_t> rec = {0xF0, 0x00, 0x20, 0x3C, 0x24, 0x01, 0x02, 0xF7};
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    auto pass = [&] { CObUsb::PollHalt(), t.Poll(); };
    pass();                                    // the send fails: the MIDI OUT is halted
    CHECK(m.halted && m.resets == 0);
    for (int k = 0; k < 10; ++k) CTimer::s_now += 1000, pass();   // past EpBox's moment (kTogetherUs): due, but EP0 is busy
    CHECK(m.resets == 0 && b.host.ep0.size() == 1);
    b.complete(b.host.ep0[0], true, 4);        // the box answers: EP0 free
    CHECK(!CObUsb::MirrorCtlBusy() && g_gadget.done.back().seq == 60);
    pass();
    CHECK(m.resets == 1 && !m.halted && t.MIDIOutResets() == 1);   // then reset, on the next Poll
    g_ToHost.q.clear();
    t.SetMIDI(nullptr);
    CTimer::s_now = 0;
  }

  {  // Two boxes, one build (mirror_assign.h, ob_usb.cpp PollAssign): USB-C first, whichever box is alone; a box that
     // arrives while USB-C shows the other goes to the relay; nobody is moved while there; the USB-C box gone past the
     // hold: the relay's box moves to USB-C (the relay told at once, a pipe packet it held asked for again)
    const unsigned long long S = 1000000;
    g_gadget.mirror = true, g_gadget.shown = true, g_gadget.hold_pipe_in = false;
    core::device::g_a4_chosen = true;                      // (the shim's usb= is not "mirror": the A4 claimed as one with the mod)
    CTimer::s_now64 = 1000 * S;                // long after boot: the earlier tests' Rytm is past its hold
    CObUsb::PollAssign();
    CHECK(CObUsb::UsbcSlot() == -1 && CObUsb::RelaySlot() == -1);
    FakeRelay relay;
    CObUsb::SetRelayTarget(&relay);
    CHECK(relay.box == nullptr);
    auto sl = [](Box& b) { for (unsigned k = 0; k < CObUsb::Slots; ++k) if (CObUsb::Box(k) == b.box()) return (int)k; return -1; };
    {
      size_t nBox = g_gadget.box.size();
      Box a(true);                             // the A4 alone: USB-C at once, read for the gadget (attach order: slot 0)
      const int sa = sl(a);
      CHECK(sa == 0);
      CHECK(CObUsb::UsbcSlot() == sa && CObUsb::GadgetSlot() == (unsigned)sa && CObUsb::RelaySlot() == 1 - sa);
      CHECK(g_gadget.box.size() == nBox + 1 && g_gadget.box.back() == 1 && relay.up.empty());
      CHECK(logged("usb=mirror: USB-C shows a4, the relay nothing (attach order, mirrorfirst=usbc)"));
      Box r;                                   // the Rytm (the preferred box) after it: the relay, the A4 stays
      CHECK(sl(r) == 1 - sa && CObUsb::UsbcSlot() == sa && relay.box == r.box() && relay.up.back() == 1);
      // the MIDI routes follow the boxes, not the slot numbers (RXQ1/2/4, T470/T471/T478: the A4 first, the Rytm's host link
      // in slot 1 went to USB-C and the relay got none): the host link is slot 1's box, shown by the relay; link1 slot 0's on USB-C
      CUSBMIDIDevice hm, lm;
      a.box()->SetProperty(CDevice::PropertyProduct, "Analog Four MKII"), lm.SetProperty(CDevice::PropertyProduct, "Analog Four MKII");
      r.box()->SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII"), hm.SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII");
      CObUsb::SetMidiDevices(&hm, &lm);
      CHECK(CObUsb::MidiSlot(0) == 1 - sa && CObUsb::MidiSlot(1) == sa && r.box()->OnTransport(0) && a.box()->OnTransport(1));
      CHECK(!CObUsb::UsbcShowsHostLink() && CObUsb::UsbcShowsLink1() && CObUsb::RelayShowsHostLink() && !CObUsb::RelayShowsLink1());
      CHECK(logged("midi: the host link is the box of slot 1 (rytm)") && logged("midi: link1 is the box of slot 0 (a4)"));
      CObUsb::SetMidiDevices(&hm, nullptr);    // the A4's MIDI not up yet: link1 nowhere, the host link's box still the relay's
      CHECK(CObUsb::MidiSlot(1) == -1 && !CObUsb::UsbcShowsLink1() && !CObUsb::UsbcShowsHostLink() && CObUsb::RelayShowsHostLink());
      CObUsb::SetMidiDevices(nullptr, nullptr);
      CHECK(g_gadget.box.size() == nBox + 1);
      CUSBRequest* rin = r.pipe(0x82);         // the Rytm's async pipe at its own interval on the relay too (PA1, T353, PV1);
      CHECK(rin && rin->ep->desc->bInterval == 1 && a.pipe(0x82)->ep->desc->bInterval == 4);	// the A4's at 4 on USB-C (T416/T417)
      r.complete(rin, true, 5);                // its pipe packets to the relay, not the gadget
      CHECK(relay.pipe_in.size() == 1 && relay.pipe_in[0] == 0x82);
      delete r.ob;                             // the Rytm leaves: gone (held on the relay), nobody moves
      r.ob = nullptr;
      CHECK(relay.box == nullptr && relay.moved == 0 && CObUsb::UsbcSlot() == sa);
      CTimer::s_now64 += 100 * S;
      CObUsb::PollAssign();
      CHECK(CObUsb::UsbcSlot() == sa);
    }
    CTimer::s_now64 += 100 * S;                // the A4 gone past the hold too: USB-C free
    CObUsb::PollAssign();
    CHECK(CObUsb::UsbcSlot() == -1);
    {
      Box r;                                   // the Rytm, then the A4: Rytm USB-C, A4 the relay
      Box a(true);
      CHECK(CObUsb::UsbcSlot() == 0 && relay.box == a.box() && relay.up.back() == 1);
      {   // the Rytm first (slot 0, USB-C), the A4 the relay (slot 1): as before two boxes, now by the boxes' own MIDI
        CUSBMIDIDevice hm, lm;
        a.box()->SetProperty(CDevice::PropertyProduct, "Analog Four MKII"), lm.SetProperty(CDevice::PropertyProduct, "Analog Four MKII");
        r.box()->SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII"), hm.SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII");
        CObUsb::SetMidiDevices(&hm, &lm);
        CHECK(CObUsb::MidiSlot(0) == 0 && CObUsb::MidiSlot(1) == 1);
        CHECK(CObUsb::UsbcShowsHostLink() && !CObUsb::UsbcShowsLink1() && !CObUsb::RelayShowsHostLink() && CObUsb::RelayShowsLink1());
        CObUsb::SetMidiDevices(nullptr, nullptr);
        CHECK(CObUsb::MidiSlot(0) == -1 && CObUsb::MidiSlot(1) == -1);
      }
      CHECK(a.pipe(0x82)->ep->desc->bInterval == 4);   // the A4's beside the relay's link: 1 ms at most (T330)
      relay.hold_pipe_in = true;               // the relay keeps the A4's next pipe packet for its computer
      CUSBRequest* ain = a.pipe(0x82);
      a.complete(ain, true, 7);
      CHECK(a.pipe(0x82) == nullptr);          // waits for the relay's computer (MirrorPipeInNext)
      size_t nBox = g_gadget.box.size();
      delete r.ob;                             // the Rytm leaves: held for it (a reboot comes back as itself)
      r.ob = nullptr;
      CHECK(g_gadget.box.back() == 0);
      CTimer::s_now64 += 5 * S;
      CObUsb::PollAssign();
      CHECK(CObUsb::UsbcSlot() == 0 && relay.box == a.box() && relay.moved == 0);
      CTimer::s_now64 += 6 * S;                // past the hold (10 s): the A4 moves to USB-C
      CObUsb::PollAssign();
      CHECK(CObUsb::UsbcSlot() == 1 && CObUsb::RelaySlot() == 0 && relay.moved == 1 && relay.box == nullptr);
      CHECK(a.pipe(0x82) != nullptr);          // its pipe asks the box again (the relay's computer never will)
      CHECK(logged(": moved from the relay"));
      CObUsb::MirrorAttachLate();              // the gadget's MirrorPoll: the A4 read for USB-C
      CHECK(g_gadget.box.size() == nBox + 2 && g_gadget.box.back() == 1);
      CObUsb::MirrorAttachLate();              // once
      CHECK(g_gadget.box.size() == nBox + 2);
      Box r2;                                  // the Rytm back: the relay now
      CHECK(CObUsb::UsbcSlot() == 1 && relay.box == r2.box());
    }
    CTimer::s_now64 += 100 * S;                // everyone gone past the hold
    CObUsb::PollAssign();
    CHECK(CObUsb::UsbcSlot() == -1);
    ObUsbTestRelayFirst(true);                 // mirrorfirst=relay (testing the relay): the relay filled first
    {
      size_t nBox = g_gadget.box.size();
      Box a(true);                             // the A4 alone: the relay, USB-C stays the Sidecar device
      const int sa = sl(a);
      CHECK(CObUsb::RelaySlot() == sa && CObUsb::UsbcSlot() == 1 - sa && relay.box == a.box() && relay.up.back() == 1);
      CHECK(g_gadget.box.size() == nBox && CObUsb::Box(CObUsb::GadgetSlot()) == nullptr);
      Box r;                                   // the Rytm: USB-C
      CHECK(CObUsb::RelaySlot() == sa && relay.box == a.box() && g_gadget.box.back() == 1);
      g_gadget.hold_pipe_in = true;            // the gadget keeps the Rytm's next pipe packet for its computer
      CUSBRequest* rin = r.pipe(0x82);
      r.complete(rin, true, 3);
      CHECK(r.pipe(0x82) == nullptr);
      delete a.ob;                             // the A4 leaves; past the hold the Rytm moves from USB-C to the relay
      a.ob = nullptr;
      CHECK(relay.box == nullptr);
      CTimer::s_now64 += 11 * S;
      size_t nUp = relay.up.size(), nG = g_gadget.box.size();
      CObUsb::PollAssign();
      CHECK(CObUsb::RelaySlot() == sl(r) && relay.box == r.box() && relay.moved == 1);
      CHECK(g_gadget.box.size() == nG + 1 && g_gadget.box.back() == 0);   // USB-C: the box gone (then the Sidecar device)
      CHECK(r.pipe(0x82) != nullptr && logged(": moved from USB-C"));
      CObUsb::PollTargets();                   // (the fake relay's MirrorPoll does nothing; read it as the relay's would)
      r.box()->AttachLate();
      CHECK(relay.up.size() == nUp + 1 && relay.up.back() == 1);
      g_gadget.hold_pipe_in = false;
    }
    ObUsbTestRelayFirst(false);
    CObUsb::SetRelayTarget(nullptr);
    core::device::g_a4_chosen = false;
    CTimer::s_now64 = 0;
  }

  {  // obrelaylead (default 7): the paced OUT lead while the box is the relay's (T369: lead 4 ran out before each relay
     // slip; T371, T373: 7 had none); USB-C keeps oboutlead. A box that moves to the relay while it streams gets the
     // rest of the relay's lead with its next IN block, once; moving back takes nothing back (those blocks are queued)
    const unsigned long long S = 1000000;
    g_gadget.mirror = true, g_gadget.shown = true, g_gadget.hold_pipe_in = false;
    core::device::g_a4_chosen = true;
    CTimer::s_now64 = 2000 * S;
    CObUsb::PollAssign();
    FakeRelay relay;
    CObUsb::SetRelayTarget(&relay);
    uint16_t ctr = 0;
    auto in = [&](Box& b, int n) {             // n audio IN blocks (0x83) from the box, in order
      for (int k = 0; k < n; ++k) {
        CUSBRequest* u = b.pipe(0x83);
        CHECK(u != nullptr);
        if (!u) return;
        memset(u->buf, 0, u->len);
        u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
        ctr += 7;
        b.complete(u, true, u->len);
      }
    };
    auto sl = [](Box& b) { for (unsigned k = 0; k < CObUsb::Slots; ++k) if (CObUsb::Box(k) == b.box()) return (int)k; return -1; };   // (attach order)
    auto outs = [](Box& b) {                   // audio OUT URBs (0x03) in flight
      int n = 0;
      for (CUSBRequest* u : b.host.out) n += u->ep && u->ep->desc && u->ep->desc->bEndpointAddress == 0x03;
      return n;
    };
    auto out_done = [](Box& b) {               // every audio OUT URB in flight completes
      for (CUSBRequest* u; (u = b.pipe(0x03)) != nullptr;) b.complete(u, true, u->len);
    };
    ObUsbTestRelayFirst(true);
    {
      Box a(true);                             // the A4 alone, mirrorfirst=relay: the relay's
      CHECK(relay.box == a.box() && CObUsb::RelaySlot() == sl(a));
      in(a, 1);
      CHECK(outs(a) == 8);                     // 1 + 7
      out_done(a);
      in(a, 3);
      CHECK(outs(a) == 3);                     // then one per IN: the lead not given again
      out_done(a);
    }
    ObUsbTestRelayFirst(false);
    CTimer::s_now64 += 100 * S;
    CObUsb::PollAssign();
    CHECK(CObUsb::UsbcSlot() == -1);
    CKernelOptions::Get()->num["oboutlead"] = 4;
    {
      Box a(true);                             // the A4 alone: USB-C, oboutlead's 4
      CHECK(CObUsb::UsbcSlot() == sl(a) && relay.box == nullptr);
      in(a, 1);
      CHECK(outs(a) == 5);
      out_done(a);
      Box r;                                   // the Rytm: the relay; the A4 stays on USB-C, its lead too
      in(a, 2);
      CHECK(outs(a) == 2);
      out_done(a);
      delete r.ob;                             // the Rytm leaves; past the hold nothing moves (the A4 is USB-C's)
      r.ob = nullptr;
    }
    CTimer::s_now64 += 100 * S;
    CObUsb::PollAssign();
    {
      Box r;                                   // the Rytm USB-C's, the A4 the relay's
      Box a(true);
      CHECK(CObUsb::UsbcSlot() == 0 && relay.box == a.box());
      in(a, 1);
      CHECK(outs(a) == 8);                     // 1 + the relay's 7
      out_done(a);
      delete r.ob;                             // the Rytm leaves; past the hold the A4 moves from the relay to USB-C
      r.ob = nullptr;
      CTimer::s_now64 += 11 * S;
      CObUsb::PollAssign();
      CHECK(relay.box == nullptr && CObUsb::UsbcSlot() == 1);
      in(a, 2);
      CHECK(outs(a) == 2);                     // nothing taken back, nothing added: still 7 ahead
      out_done(a);
    }
    CTimer::s_now64 += 100 * S;
    CObUsb::PollAssign();
    ObUsbTestRelayFirst(true);
    {
      Box a(true);                             // mirrorfirst=relay: the A4 the relay's, the Rytm USB-C's (oboutlead 4)
      Box r;
      CHECK(relay.box == a.box() && CObUsb::UsbcSlot() == sl(r));
      in(r, 1);
      CHECK(outs(r) == 5);
      out_done(r);
      delete a.ob;                             // the A4 leaves; past the hold the Rytm moves from USB-C to the relay
      a.ob = nullptr;
      CTimer::s_now64 += 11 * S;
      CObUsb::PollAssign();
      CHECK(relay.box == r.box());
      in(r, 1);
      CHECK(outs(r) == 4);                     // 1 + the 3 more, with its next IN
      out_done(r);
      in(r, 2);
      CHECK(outs(r) == 2);                     // once
      out_done(r);
    }
    ObUsbTestRelayFirst(false);
    CKernelOptions::Get()->num.erase("oboutlead");
    CObUsb::SetRelayTarget(nullptr);
    core::device::g_a4_chosen = false;
    CTimer::s_now64 = 0;
  }
  CHECK(CUSBRequest::live() == 0);

  {  // the attach gate (attach_gate.h, obgate=10000): a box is held from the computer until its IN and OUT both run full
     // for 2 s, then shown; a held box starved for 3 s is restarted on the Pi (alt 0 and back, new endpoints), then (here
     // its device hangs off no root port or VL805 hub: no re-enumeration) shown late at the hold's limit; a shown box
     // whose OUT starves is restarted, its target told the box is up again (a new stream), twice a stream at most
    CKernelOptions::Get()->num["obgate"] = 10000;
    static std::deque<CUSBRequest*>* s_q[2];
    CXHCIEndpoint::s_on_stop = [](CXHCIEndpoint* x) {   // the controller forgets that endpoint's URBs (Stop + drop)
      for (auto* q : s_q)
        for (auto i = q->begin(); i != q->end();) i = (*i)->ep && (*i)->ep->GetXHCIEndpoint() == x ? q->erase(i) : i + 1;
    };
    uint16_t ctr = 0;
    auto in = [&](Box& b) {
      CUSBRequest* u = b.pipe(0x83);
      CHECK(u != nullptr);
      if (!u) return;
      memset(u->buf, 0, u->len);
      u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
      ctr += 7;
      b.complete(u, true, u->len);
    };
    auto second = [&](Box& b, bool out_ok) {   // a second of the box's stream (6,860 blocks); OUT served or starved
      for (int k = 0; k < 6860; ++k) {
        in(b);
        if (out_ok) for (CUSBRequest* u; (u = b.pipe(0x03)) != nullptr;) b.complete(u, true, u->len);
      }
      CTimer::s_now += 1000000;
      CObUsb::LogAllStats(CLogger::Get());
      CObUsb::PollHalt();
    };
    auto ring = [&](Box& b) {                   // the OUT endpoint's queue at the controller (periodic_starve.h asks it)
      in(b);                                   // (an OUT URB in flight to find the endpoint by)
      CUSBRequest* u = b.pipe(0x03);
      CHECK(u != nullptr);
      if (u) u->ep->GetXHCIEndpoint()->ring = &b.host.out;
    };
    auto count = [](const char* what) {
      int n = 0;
      for (const std::string& l : CLogger::Get()->lines) n += l.find(what) != std::string::npos;
      return n;
    };
    {
      const size_t n0 = g_gadget.box.size();
      Box b;
      s_q[0] = &b.host.in, s_q[1] = &b.host.out;
      CHECK(g_gadget.box.size() == n0 && CObUsb::GateHolds(0) && count("held from the computer until its audio runs both ways") == 1);
      second(b, true);
      CHECK(g_gadget.box.size() == n0 && CObUsb::GateHolds(0));
      second(b, true);
      CHECK(g_gadget.box.size() == n0 + 1 && g_gadget.box.back() == 1 && !CObUsb::GateHolds(0));
      CHECK(count("gate: show (IN 6860/s, OUT 6860/s, held 2000 ms)") == 1 && count("shown to the computer after 2000 ms") == 1);
      CHECK(CObUsb::AttachGate(0).holds >= 1 && CObUsb::AttachGate(0).total_restarts == 0);

      // shown, then its OUT starves: named after 3 s, restarted; the gadget told the box is up again (its offset anew)
      const size_t ctl0 = b.host.ctl.size(), n1 = g_gadget.box.size();
      const unsigned r0 = CObUsb::StreamRestarts();
      CUSBRequest* old_in = b.pipe(0x83);
      CUSBEndpoint* old_ep = old_in ? old_in->ep : nullptr;
      ring(b);
      for (int k = 0; k < 4; ++k) second(b, false);
      CHECK(count("periodic starvation: OUT") == 1 && CObUsb::StreamRestarts() == r0 + 1);
      CHECK(count("its OUT starved by the host controller: its audio stream restarted on the Pi") == 1);
      CHECK(b.host.ctl.size() == ctl0 + 4);   // interfaces 1 and 2 to alt 0, then alt 3
      CHECK(b.host.ctl[ctl0].req == SET_INTERFACE && b.host.ctl[ctl0].value == 0 && b.host.ctl[ctl0 + 2].value == 3);
      CUSBRequest* new_in = b.pipe(0x83);
      CHECK(new_in != nullptr && new_in->ep != old_ep && count("restart: new endpoints (") == 1);
      CHECK(g_gadget.box.size() == n1 + 1 && g_gadget.box.back() == 1);
      ring(b);
      second(b, true), second(b, true);         // it streams again
      for (int k = 0; k < 4; ++k) second(b, false);
      CHECK(CObUsb::StreamRestarts() == r0 + 2);
      ring(b);
      second(b, true), second(b, true);
      for (int k = 0; k < 4; ++k) second(b, false);
      CHECK(CObUsb::StreamRestarts() == r0 + 2);   // kShownRestarts a stream: not a third
      second(b, true);
    }
    CHECK(CUSBRequest::live() == 0);
    {
      const size_t n0 = g_gadget.box.size();
      const unsigned r0 = CObUsb::StreamRestarts();
      Box b;
      s_q[0] = &b.host.in, s_q[1] = &b.host.out;
      const size_t ctl0 = b.host.ctl.size();
      for (int k = 0; k < 3; ++k) second(b, false);
      CHECK(CObUsb::StreamRestarts() == r0 + 1 && b.host.ctl.size() == ctl0 + 4 && CObUsb::GateHolds(0));
      CHECK(count("held from the computer, its audio not running both ways: its audio stream restarted") == 1);
      CHECK(g_gadget.box.size() == n0);
      for (int k = 0; k < 3; ++k) second(b, false);
      CHECK(count("gate: reenum") == 1 && count("hangs off another hub: no re-enumeration") >= 1 && CObUsb::GateHolds(0));
      for (int k = 0; k < 3; ++k) second(b, false);
      CHECK(g_gadget.box.size() == n0);
      second(b, false);                         // 10 s: shown anyway
      CHECK(g_gadget.box.size() == n0 + 1 && g_gadget.box.back() == 1 && !CObUsb::GateHolds(0));
      CHECK(count("although its audio does not run both ways") == 1 && CObUsb::AttachGate(0).shown_late == 1);
    }
    CHECK(CUSBRequest::live() == 0);
    {  // gone while held: nothing shown; the next attach holds again
      const size_t n0 = g_gadget.box.size();
      { Box b; s_q[0] = &b.host.in, s_q[1] = &b.host.out; CHECK(CObUsb::GateHolds(0)); }
      CHECK(!CObUsb::AttachGate(0).held && g_gadget.box.size() >= n0);
    }
    CXHCIEndpoint::s_on_stop = nullptr;
    CKernelOptions::Get()->num["obgate"] = 0;
  }
  CHECK(CUSBRequest::live() == 0);

  {  // usb=mirror off (the default): a box is streamed as ever, no pipes, nothing told to the gadget
    g_gadget.mirror = false;
    size_t nBox = g_gadget.box.size();
    Box b;
    CHECK(b.pipe(0x82) == nullptr && b.host.ctl.size() == 4 && g_gadget.box.size() == nBox);
    TSetupData s = {0xC0, 0x01, 0, 0, 16};
    CHECK(!CObUsb::MirrorControl(&s, nullptr, 1));
  }

  puts("test_ob_mirror: ok");
  return 0;
}
