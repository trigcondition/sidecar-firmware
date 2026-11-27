// pi/engine/tests/test_ob_usb.cpp - the real ob_usb.cpp on a fake Circle (shim_ob/): the descriptors a Digitakt and a Rytm send, the
// sequence on the control endpoint, the URBs' sizes, OUT paced by IN whatever the two sizes are, the bytes of an OUT block. The
// descriptors are what the firmware sent an emulated host (rytm-emu dtusb 692c4cc, dtboot --usb-host: Digitakt OS 1.53 and 1.54,
// OVERBRIDGE; Rytm OS 1.73): the Digitakt's matches a physical capture byte for byte (docs/device-profiles.md, "Audio").
#include "check.h"
#include "device/digitakt.h"
#include "fx/out.h"
#include "ob_block.h"
#include "ob_usb.h"
#include "ob_follow.h"
#include "shared.h"
#include <circle/timer.h>
#include <circle/usb/usbstandardhub.h>

#include <circle/koptions.h>
#include <circle/bcmpciehostbridge.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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
static uint32_t g_slot_uid[2];
namespace core { namespace device {
uint32_t g_attach_uid = 0;   // the UID the last usb_attach was given
int g_midi_slot[2] = {-1, -1};   // set_midi_slot's
void set_midi_slot(int t, int s) { if (t >= 0 && t < 2) g_midi_slot[t] = s; }
int usb_attach(const Profile* p, uint32_t uid) {
  g_attach_uid = uid;
  for (int k = 0; k < 2; ++k)
    if (g_slot_of[k] == p && (!uid || !g_slot_uid[k] || g_slot_uid[k] == uid)) return k;   // (two of a type: by UID)
  for (int k = 0; k < 2; ++k)
    if (!g_slot_of[k]) {
      g_slot_of[k] = p, g_slot_uid[k] = uid;
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
static uint8_t g_digitakt[263];
static const int kOutAlt3Mps = 137;   // 0x70 0x01 -> 0x58 0x00
// The A4 MKII's differs from the Rytm's in both alt 3 sizes (OS 1.55D's table and 1.56's over an emulated USB host, byte for byte)
static uint8_t g_a4[263];
static const int kInAlt3Mps = 80;     // 0x70 0x01 -> 0x00 0x01

struct Bus {
  CUSBDevice dev;
  CUSBHostController host;
  CUSBFunction parent{&dev, &host};
  CUSBFunction* ob = nullptr;
  Bus(const uint8_t* cfg, uint16_t pid, uint32_t uid = 0) {
    dev.config = cfg;
    dev.desc.idVendor = 0x1935, dev.desc.idProduct = pid;
    if (uid)   // the box answers the vendor request 0 (ObReadUid): a word 1, then its UID, big-endian (as the units)
      host.vendor_in = [uid](u8 r, u8* b, unsigned n) {
        if (r != 0 || n != 8) return -1;
        const u8 a[8] = {0, 0, 0, 1, (u8)(uid >> 24), (u8)(uid >> 16), (u8)(uid >> 8), (u8)uid};
        memcpy(b, a, 8);
        return 8;
      };
    ob = RytmObDeviceCreate(&parent);
  }
  ~Bus() { delete ob; }
  // n IN blocks arrive, one URB each, as the device sends them; the next counter is the caller's
  void in(int n, uint16_t& ctr) {
    for (int k = 0; k < n; ++k) {
      CUSBRequest* u = host.in.front();
      host.in.pop_front();
      memset(u->buf, 0, 368);
      u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
      ctr += 7;
      u->SetStatus(1), u->SetResultLen(368);
      u->fn(u, u->param, u->ctx);
    }
  }
  // the A4's: a block of 256 B, a tone on MAIN L and its negative on MAIN R, the other six channels at full scale so a wrong channel shows
  void in_a4(int n, uint16_t& ctr, long& at, double hz) {
    for (int k = 0; k < n; ++k) {
      CUSBRequest* u = host.in.front();
      host.in.pop_front();
      memset(u->buf, 0, 256);
      u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
      ctr += 7;
      for (int f = 0; f < 7; ++f, ++at) {
        int32_t x = (int32_t)(0.5 * 2147483647.0 * sin(2 * M_PI * hz * (double)at / 48000.0));
        for (int c = 0; c < 8; ++c) ob::put_be32(u->buf + 32 + 4 * (8 * f + c), c == 0 ? x : c == 1 ? -x : 0x7FFFFFFF);
      }
      u->SetStatus(1), u->SetResultLen(256);
      u->fn(u, u->param, u->ctx);
    }
  }
  void out_done() {
    CUSBRequest* u = host.out.front();
    host.out.pop_front();
    u->SetStatus(1), u->SetResultLen(u->len);
    u->fn(u, u->param, u->ctx);
  }
};

static void same_sequence(const CUSBHostController& h) {
  const u16 want[4][2] = {{0, 1}, {0, 2}, {3, 1}, {3, 2}};   // alt 0 on 1 and 2, then alt 3
  CHECK(h.ctl.size() == 4);
  for (int k = 0; k < 4; ++k)
    CHECK(h.ctl[k].req == SET_INTERFACE && h.ctl[k].value == want[k][0] && h.ctl[k].index == want[k][1]);
}

// the driver holds its URBs and endpoints for the life of the device; on the Pi nothing frees them
extern "C" const char *__asan_default_options () { return "detect_leaks=0"; }

Output g_OutputSide;   // the side stage (side_out.h): a box beside the Rytm pulls it

// the host link's USB MIDI is box ob's (the same USB strings, as Circle copies them; the kernel's SetMidiDevices each pass):
// its MIDI OUT's failures go with that box's endpoints (MidiHalted, BoxWaits)
static void host_link_is(CUSBFunction* ob) {
  static CUSBMIDIDevice midi;
  ob->SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII"), midi.SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII");
  CObUsb::SetMidiDevices(&midi, nullptr);
}

int main() {
  {  // a VL805 behind the root complex (PollAspm reads it at its first pass): ASPM off, its PCIe capability at 0xc4
    auto& rc = CBcmPCIeHostBridge::s_cfg[0];
    auto& vl = CBcmPCIeHostBridge::s_cfg[1];
    rc[0] = 0x271114e4;
    vl[0] = 0x34831106, vl[4] = 0x00100000, vl[0x34] = 0x80, vl[0x80] = 0x0000c401, vl[0xc4] = 0x00020010, vl[0xd4] = 0x10120040;
  }
  memcpy(g_digitakt, g_rytm, sizeof g_rytm);
  CHECK(g_rytm[kOutAlt3Mps] == 0x70 && g_rytm[kOutAlt3Mps + 1] == 0x01);
  g_digitakt[kOutAlt3Mps] = 0x58, g_digitakt[kOutAlt3Mps + 1] = 0x00;

  {  // the parser of both
    ob::Layout r, d;
    CHECK(ob::find_layout(g_rytm, sizeof g_rytm, ob::kAlt, &r) && r.in_channels == 12 && r.out_channels == 12);
    CHECK(ob::find_layout(g_digitakt, sizeof g_digitakt, ob::kAlt, &d));
    CHECK(d.in.mps == 368 && d.out.mps == 88 && d.in_channels == 12 && d.out_channels == 2 && d.midi_iface == 5);
    CHECK(ob::block_bytes(core::device::kDigitakt.audio->channels) == 88);
    CHECK(core::device::kDigitakt.audio->in_channels == 12);
    CHECK(core::device::kDigitakt.bracket == core::device::kBracketMix && core::device::kDigitakt.audio->ret == core::out::kStereoMix);
  }

  {  // the Rytm: 368 both ways, 12 channels, the old log line, the table untouched
    Bus b(g_rytm, 0x0b48);
    CHECK(b.ob && b.ob->Configure());
    CHECK(g_rytm_usb == 1);
    {  // a second box while the Rytm streams: its own stream in slot 1, beside it; the Rytm stays the primary one (the engine's
       // output and the device table follow it), the second's OUT silent; a third box finds its slot taken
      Bus s(g_digitakt, 0x0b2c);
      CHECK(s.ob != nullptr && s.ob->Configure() && g_rytm_usb == 1);
      CObUsb *so = (CObUsb *)s.ob, *bo = (CObUsb *)b.ob;
      CHECK(so->Slot() == 1 && !so->Primary() && bo->Primary() && CObUsb::Box(1) == so && CObUsb::Box(0) == bo);
      CHECK(&core::out::device() == &core::out::kRytmMk2);
      {
        Bus t(g_digitakt, 0x0b2c);
        CHECK(t.ob == nullptr);
      }
    }
    CHECK(CObUsb::Box(1) == nullptr && ((CObUsb *)b.ob)->Primary() && &core::out::device() == &core::out::kRytmMk2);
    same_sequence(b.host);
    CHECK(b.host.in.size() == 8 && b.host.in.front()->len == 368);
    CHECK(&core::out::device() == &core::out::kRytmMk2);
    CHECK(CLogger::Get()->lines[0] == "Analog Rytm MKII (1935:0b48) claimed");
    g_Output.SetMode(Output::kTracks);
    int16_t fr[32][8] = {};
    for (auto& f : fr) f[2] = 8000;
    g_Output.Push(fr, 32);
    uint16_t c = 0;
    b.in(1, c);
    CHECK(b.host.out.size() == 1 && b.host.out[0]->len == 368);
    // a voice on its track's channel and nowhere else, as ever
    const uint8_t* o = b.host.out[0]->buf;
    for (int ch = 0; ch < 12; ++ch) CHECK((ob::block_sample(o, 12, 6, ch) != 0) == (ch == ob::kVoiceToRytm[2]));
    g_Output.SetMode(Output::kMain);
  }

  CHECK(g_rytm_usb == 0);   // the Rytm gone
  {  // a Digitakt's descriptor under the Rytm's id: refused, as a Rytm with a wrong descriptor always was
    Bus b(g_digitakt, 0x0b48);
    CHECK(b.ob && !b.ob->Configure());
  }
  {  // USB MIDI mode: not this driver's, Circle goes on to the interfaces
    Bus b(g_rytm, 0x000d);
    CHECK(b.ob == nullptr);
    b.ob = nullptr;
  }

  {  // the Digitakt
    Bus b(g_digitakt, 0x0b2c);
    CHECK(b.ob && b.ob->Configure());
    CHECK(g_rytm_usb == 0);   // a Digitakt's id asks for nothing: its hello does
    same_sequence(b.host);
    CHECK(b.host.in.size() == 8 && b.host.in.front()->len == 368);
    CHECK(&core::out::device() == core::device::kDigitakt.audio);
    for (int v = 0; v < 8; ++v) CHECK(core::out::control.route[v] == core::out::kPost);
    CHECK(g_Output.GetMode() == Output::kMain);   // whatever was set: no voice channels

    // voices in, one OUT block per IN block: 88 B, header, counter + 7, two channels
    int16_t fr[64][8] = {};
    for (auto& f : fr) f[0] = 8000, f[7] = 8000;
    g_Output.Push(fr, 64);
    g_Output.Push(fr, 64);
    uint16_t c = 0;
    b.in(1, c);
    CHECK(b.host.out.size() == 1 && b.host.out[0]->len == 88);
    const uint8_t* o = b.host.out[0]->buf;
    CHECK(o[0] == 0x07 && o[1] == 0xFF && o[2] == 0 && o[3] == 0);
    b.out_done();
    b.in(1, c);
    o = b.host.out[0]->buf;
    CHECK(o[3] == 7);

    // eight URBs in flight, the device stops taking them: credit is kept, none is lost, nothing is sent short
    b.out_done();
    b.in(8, c);
    CHECK(b.host.out.size() == 8);
    b.in(4, c);
    CHECK(b.host.out.size() == 8);   // four owed
    for (int k = 0; k < 4; ++k) b.out_done();
    CHECK(b.host.out.size() == 8);   // the four owed went at once
    for (int k = 0; k < 8; ++k) {
      CHECK(b.host.out[k]->len == 88);
      CHECK(((b.host.out[k]->buf[2] << 8 | b.host.out[k]->buf[3]) % 7) == 0);
    }
    uint16_t last = b.host.out[7]->buf[2] << 8 | b.host.out[7]->buf[3];
    CHECK(last == 7 * 13);   // blocks 0..13, one per IN, in order
    // the voices' sum on both channels, L = R (TRK-less: POST, centred), and the block ends at 88 B: frames 7 x 2 x 4 after 32
    const uint8_t* l = b.host.out[7]->buf;
    CHECK(ob::block_sample(l, 2, 6, 0) != 0 && ob::block_sample(l, 2, 6, 0) == ob::block_sample(l, 2, 6, 1));
    CHECK(g_Output.Underflows() == 0);
  }
  {  // oboutlead=4: the first IN block sends 1 + 4 OUT blocks (the box's cushion, T346-T351), then one per IN as ever; the
     // counters run on without a gap; the lead is not given again within the stream
    CKernelOptions::Get()->num["oboutlead"] = 4;
    Bus b(g_rytm, 0x0b48);
    CHECK(b.ob && b.ob->Configure());
    uint16_t c = 0;
    b.in(1, c);
    CHECK(b.host.out.size() == 5);
    for (int k = 0; k < 5; ++k) CHECK((b.host.out[k]->buf[2] << 8 | b.host.out[k]->buf[3]) == 7 * k);
    for (int k = 0; k < 5; ++k) b.out_done();
    CHECK(b.host.out.empty());
    b.in(3, c);
    CHECK(b.host.out.size() == 3);   // one per IN: the lead stays 4 ahead
    CHECK((b.host.out[2]->buf[2] << 8 | b.host.out[2]->buf[3]) == 7 * 7);
    CKernelOptions::Get()->num.erase("oboutlead");
  }
  CHECK(&core::out::device() == &core::out::kRytmMk2);   // the box gone: the Rytm's table again

  // The A4 beside the Rytm
  memcpy(g_a4, g_rytm, sizeof g_rytm);
  CHECK(g_a4[kInAlt3Mps] == 0x70 && g_a4[kInAlt3Mps + 1] == 0x01);
  g_a4[kInAlt3Mps] = 0x00, g_a4[kInAlt3Mps + 1] = 0x01;
  g_a4[kOutAlt3Mps] = 0xc8, g_a4[kOutAlt3Mps + 1] = 0x00;
  {
    ob::Layout a;
    CHECK(ob::find_layout(g_a4, sizeof g_a4, ob::kAlt, &a) && a.in_channels == 8 && a.out_channels == 6 && a.midi_iface == 5);
  }
  CKernelOptions* opt = CKernelOptions::Get();
  {  // a4=0, the default: not claimed, Circle goes on to its interfaces
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob == nullptr);
    a.ob = nullptr;
  }
  core::device::g_a4_chosen = true;
  {  // the A4 as the box in use: CObUsb, 256 / 200, each voice on its own channel, IN widened to 12
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob && a.ob->Configure());
    same_sequence(a.host);
    CHECK(CObUsb::Streaming() && !CObFollow::Streaming());
    CHECK(a.host.in.size() == 8 && a.host.in.front()->len == 256);
    CHECK(&core::out::device() == core::device::kA4.audio);
    CHECK(g_rytm_usb == 0);
    int16_t fr[64][8] = {};
    for (auto& f : fr)
      for (int v = 0; v < 8; ++v) f[v] = (int16_t)(1000 * (v + 1));
    g_Output.SetMode(Output::kTracks);
    g_Output.Push(fr, 64);
    g_Output.Push(fr, 64);
    uint16_t c = 0;
    long at = 0;
    a.in_a4(1, c, at, 1000.0);
    CHECK(a.host.out.size() == 1 && a.host.out[0]->len == 200);
    const uint8_t* o = a.host.out[0]->buf;
    a.out_done();
    for (int k = 0; k < 12; ++k) {         // past the fade-in
      a.in_a4(1, c, at, 1000.0);
      o = a.host.out.front()->buf;
      if (k == 11)
        for (int ch = 0; ch < 4; ++ch) CHECK(ob::block_sample(o, 6, 6, 2 + ch) != 0);
      CHECK(ob::block_sample(o, 6, 6, 0) == 0 && ob::block_sample(o, 6, 6, 1) == 0);
      a.out_done();
    }
    CHECK(g_Output.Underflows() == 0);
    uint8_t in[256], w[368];
    memset(in, 0, 256);
    for (int f = 0; f < 7; ++f)
      for (int ch = 0; ch < 8; ++ch) ob::put_be32(in + 32 + 4 * (8 * f + ch), (f + 1) * 16 + ch);
    memset(w, 0xAA, sizeof w);
    CObUsb::WidenIn(in, w, 8);
    for (int f = 0; f < 7; ++f) {
      for (int ch = 0; ch < 6; ++ch) CHECK(ob::block_sample(w, 12, f, ch) == (f + 1) * 16 + ch);
      for (int ch = 6; ch < 10; ++ch) CHECK(ob::block_sample(w, 12, f, ch) == 0);
      CHECK(ob::block_sample(w, 12, f, 10) == (f + 1) * 16 + 6 && ob::block_sample(w, 12, f, 11) == (f + 1) * 16 + 7);
    }
  }
  CHECK(&core::out::device() == &core::out::kRytmMk2);
  core::device::g_a4_chosen = false;
  {  // the Rytm alone with the A4's code in the build: OUT as ever, PRE silent
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    for (int f = 0; f < 7; ++f) CHECK(ob::block_sample(r.host.out[0]->buf, 12, f, 2) == 0 && ob::block_sample(r.host.out[0]->buf, 12, f, 3) == 0);
  }
  opt->num["a4"] = 1;
  opt->num["a4att"] = 0;
  CXHCIEndpoint::s_valid = false;
  {  // the controller refuses the second stream: no stream, nothing left running
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob && !a.ob->Configure());
    CHECK(a.host.in.empty() && !CObFollow::Streaming());
  }
  CXHCIEndpoint::s_valid = true;
  {  // a wrong descriptor under the A4's id
    Bus a(g_rytm, 0x0b47);
    CHECK(a.ob && !a.ob->Configure());
  }
  {
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    Bus a(g_a4, 0x0b47);                  // one driver each, on one bus each here
    CHECK(a.ob && a.ob->Configure());
    CHECK(CObUsb::Streaming() && CObFollow::Streaming());
    CHECK(&core::out::device() == &core::out::kRytmMk2);   // the A4 changes nothing of the Rytm's
    {
      Bus b(g_a4, 0x0b47);                // a second A4: no
      CHECK(b.ob == nullptr);
      b.ob = nullptr;
    }
    same_sequence(a.host);
    CHECK(a.host.in.size() == 8 && a.host.in.front()->len == 256);
    uint16_t cr = 0, ca = 0;
    long at = 0;
    double peak_l = 0, peak_r = 0, other = 0;
    bool antiphase = true;
    for (int b = 0; b < 8000; ++b) {
      a.in_a4(1, ca, at, 1000.0);
      CHECK(a.host.out.size() == 1 && a.host.out[0]->len == 200);   // its OUT: one silent block for each IN block
      const uint8_t* o = a.host.out[0]->buf;
      CHECK(o[0] == 0x07 && o[1] == 0xFF && (uint16_t)(o[2] << 8 | o[3]) == (uint16_t)(ca - 7));
      for (int ch = 0; ch < 6; ++ch) CHECK(ob::block_sample(o, 6, 6, ch) == 0);
      a.out_done();
      r.in(1, cr);
      const uint8_t* q = r.host.out.back()->buf;
      for (int f = 0; f < 7; ++f) {
        int32_t l = ob::block_sample(q, 12, f, 2), rr = ob::block_sample(q, 12, f, 3);
        peak_l = fmax(peak_l, fabs((double)l)), peak_r = fmax(peak_r, fabs((double)rr));
        antiphase = antiphase && fabs((double)l + (double)rr) < 1e5;
        other = fmax(other, fabs((double)ob::block_sample(q, 12, f, 0)));   // POST stays the Rytm's
      }
      r.out_done();
    }
    CHECK(fabs(peak_l / 2147483648.0 - 0.5) < 0.01 && fabs(peak_r / 2147483648.0 - 0.5) < 0.01);   // unity at a4att=0, the tone on PRE L and R
    CHECK(antiphase);
    CHECK(other < 3e7);                   // what the voices left there, not the A4
    CLogger::Get()->lines.clear();
    CObFollow::LogStats(CLogger::Get());
    uint16_t c2 = cr;
    delete a.ob;                          // the A4 goes: the Rytm's stream goes on, PRE fades out
    a.ob = nullptr;
    CHECK(!CObFollow::Streaming() && CObUsb::Streaming());
    for (int b = 0; b < 200; ++b) {
      r.in(1, c2);
      r.out_done();
    }
    r.in(1, c2);
    const uint8_t* q = r.host.out.back()->buf;
    for (int f = 0; f < 7; ++f) CHECK(ob::block_sample(q, 12, f, 2) == 0);
  }
  opt->num.clear();
  {  // periodic starvation (XV1/XR1, periodic_starve.h): IN full, OUT served one transfer in ten with every OUT URB at the
     // controller: named after 3 s with the iv1 endpoints; over on the first full second. With URBs free: never named
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    CXHCIEndpoint::s_iv1 = 6, CXHCIEndpoint::s_iv1_busy = 6, CXHCIEndpoint::s_iv1_list = "s1:83,03,82 s2:81,01,82";
    auto count = [](const char* what, size_t from) {
      int n = 0;
      auto& L = CLogger::Get()->lines;
      for (size_t i = from; i < L.size(); ++i) n += L[i].find(what) != std::string::npos;
      return n;
    };
    auto second = [&](int every) {   // 1400 IN blocks; an OUT completes after every `every` IN (1: full)
      for (int k = 0; k < 1400; ++k) {
        r.in(1, c);
        if (k % every == every - 1 && !r.host.out.empty()) r.out_done();
      }
      CTimer::s_now += 1000000;
      CObUsb::LogAllStats(CLogger::Get());
    };
    size_t l0 = CLogger::Get()->lines.size();
    second(1);
    second(1);   // the start line (2nd second): the iv1 count and list
    CHECK(count("periodic iv1 6, 6 busy: s1:83,03,82 s2:81,01,82", l0) == 1);
    CHECK(count("periodic starvation", l0) == 0);
    for (int k = 0; k < 2; ++k) second(10);
    CHECK(r.host.out.size() == 8 && count("periodic starvation", l0) == 0);
    second(10);
    CHECK(count("periodic starvation: OUT 140/s under IN 1400/s for 3 s with all 8 OUT URBs at the controller (k 0 "
                "services a ms); periodic iv1 6, 6 busy: s1:83,03,82 s2:81,01,82", l0) == 1);
    second(10);
    CHECK(count("periodic starvation", l0) == 1);   // still short: no second line
    second(1);
    CHECK(count("periodic starvation over after 4 s: OUT", l0) == 1 && count("(1 times this stream)", l0) == 1);
    second(1);
    CHECK(count("periodic starvation", l0) == 2);
    CXHCIEndpoint::s_iv1 = 0, CXHCIEndpoint::s_iv1_busy = 0, CXHCIEndpoint::s_iv1_list = "";
    x->ring = nullptr;
  }
  {  // an OUT transfer error (the tester's "xhciep: Transfer error 4 on endpoint 6", Pi 5, 0.4.0): the xHC halts the
     // endpoint, the URBs behind the failed one never complete. The main loop's PollHalt resets it, drops what the reset
     // skipped and the stream goes on: OUT pulls the voices again (before: OUT stopped for good, the output FIFO stayed
     // full, the render cores idle at 0 %, no underrun counted)
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CHECK(r.host.out.size() == 1);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    for (int k = 0; k < 20; ++k) r.in(1, c), r.out_done();   // streaming: one OUT per IN
    r.in(4, c);
    CHECK(r.host.out.size() == 5);
    {  // the front one fails: code 4, the endpoint halted, the other four stay on the stopped ring
      CUSBRequest* u = r.host.out.front();
      r.host.out.pop_front();
      x->state = 2;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
    }
    r.in(20, c);   // IN goes on; nothing on OUT completes while halted
    CObUsb::PollHalt();
    CHECK(x->resets == 0);   // alone so far: a moment for another endpoint to fail (EpBox)
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();
    CHECK(x->resets == 1);
    CHECK(CLogger::Get()->lines.back().find("OUT endpoint halted") != std::string::npos);
    CHECK(r.host.ctl.back().req == CLEAR_FEATURE && r.host.ctl.back().value == ENDPOINT_HALT && r.host.ctl.back().index == 0x03 &&
          r.host.ctl.back().type == (REQUEST_OUT | REQUEST_TO_ENDPOINT));   // the Rytm's toggle follows the host's
    int16_t fr[64][8] = {};
    g_Output.Push(fr, 64);
    unsigned lvl = g_Output.Level();
    for (int k = 0; k < 8; ++k) {   // OUT again, from the next IN on: every new URB completes, the FIFO is pulled
      r.in(1, c);
      CHECK(!r.host.out.empty());
      while (!r.host.out.empty()) r.out_done();
    }
    CHECK(g_Output.Level() < lvl);
    CObUsb::PollHalt();
    CHECK(x->resets == 1);   // nothing to do while it runs
    {  // a reset the controller refuses: tried again later, not in a loop
      r.in(1, c);
      CUSBRequest* u = r.host.out.front();
      r.host.out.pop_front();
      x->state = 2, x->reset_ok = false;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
      CTimer::s_now += EpBox::kTogetherUs;
      CObUsb::PollHalt(), CObUsb::PollHalt();
      CHECK(x->resets == 2);
      CTimer::s_now += 1000000;
      x->reset_ok = true;
      CObUsb::PollHalt();
      CHECK(x->resets == 3 && x->state == 3);
      r.in(1, c);
      CHECK(r.host.out.size() == 1);
      CTimer::s_now = 0;
    }
    x->ring = nullptr;
  }
  {  // OB IN fails once (DCI 7): IN halts, so does OUT (paced by IN); PollHalt resets it, the 7 queued IN URBs dropped, all 8 again
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    r.out_done();
    CXHCIEndpoint* xi = r.host.in.front()->ep->GetXHCIEndpoint();
    xi->ring = &r.host.in;
    unsigned before = CObUsb::HaltResets(TRUE);
    CUSBRequest* u = r.host.in.front();
    r.host.in.pop_front();
    xi->state = 2;
    u->SetStatus(0), u->SetResultLen(0);
    u->fn(u, u->param, u->ctx);
    CHECK(r.host.in.size() == 7);   // not resubmitted: the 7 behind it stuck
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();
    CHECK(xi->resets == 1 && CObUsb::HaltResets(TRUE) == before + 1);
    CHECK(r.host.ctl.back().req == CLEAR_FEATURE && r.host.ctl.back().index == 0x83);
    CHECK(CLogger::Get()->lines.back().find("IN endpoint halted (state 2, transfer error 4): reset 1, 7 URBs dropped, IN goes on") != std::string::npos);
    CHECK(r.host.in.size() == 8);
    c += 7 * 8;   // (the blocks lost meanwhile: a counter gap)
    for (int k = 0; k < 4; ++k) {
      r.in(1, c);
      CHECK(r.host.out.size() == 1);   // OUT follows IN again
      r.out_done();
    }
    xi->ring = nullptr;
  }
  {  // OB OUT fails twice in a row: the first reset, then the first new URB fails again: a second reset after the
     // back-off's 10 ms (no good completion between them), not at once
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    unsigned before = CObUsb::HaltResets(FALSE);
    auto fail_front = [&] {
      CUSBRequest* u = r.host.out.front();
      r.host.out.pop_front();
      x->state = 2;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
    };
    r.in(3, c);
    fail_front();
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();
    r.in(1, c);
    CHECK(r.host.out.size() == 1);
    fail_front();
    r.in(2, c);
    CHECK(r.host.out.empty());   // none while halted
    CObUsb::PollHalt();
    CHECK(x->resets == 1);       // backing off
    CTimer::s_now += 10000;
    CObUsb::PollHalt();
    CHECK(x->resets == 2 && CObUsb::HaltResets(FALSE) == before + 2);
    for (int k = 0; k < 4; ++k) {
      r.in(1, c);
      CHECK(!r.host.out.empty());
      while (!r.host.out.empty()) r.out_done();
    }
    x->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // the A4 follower: IN fails once, then OUT twice in a row; each reset, both streams go on
    opt->num["a4"] = 1;
    opt->num["a4att"] = 0;
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob && a.ob->Configure() && CObFollow::Streaming());
    uint16_t ca = 0;
    long at = 0;
    a.in_a4(1, ca, at, 1000.0);
    a.out_done();
    CXHCIEndpoint* xi = a.host.in.front()->ep->GetXHCIEndpoint();
    xi->ring = &a.host.in;
    unsigned bi = CObFollow::HaltResets(TRUE), bo = CObFollow::HaltResets(FALSE);
    {
      CUSBRequest* u = a.host.in.front();
      a.host.in.pop_front();
      xi->state = 2;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
    }
    CHECK(a.host.in.size() == 7);
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();   // (the follower's too)
    CHECK(xi->resets == 1 && CObFollow::HaltResets(TRUE) == bi + 1 && a.host.in.size() == 8);
    CHECK(a.host.ctl.back().req == CLEAR_FEATURE && a.host.ctl.back().index == 0x83);
    CHECK(CLogger::Get()->lines.back().find("IN endpoint halted") != std::string::npos);
    a.in_a4(1, ca, at, 1000.0);
    CHECK(a.host.out.size() == 1);
    CXHCIEndpoint* xo = a.host.out.front()->ep->GetXHCIEndpoint();
    xo->ring = &a.host.out;
    for (int t = 0; t < 2; ++t) {   // twice in a row
      CUSBRequest* u = a.host.out.front();
      a.host.out.pop_front();
      xo->state = 2;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
      a.in_a4(1, ca, at, 1000.0);
      CHECK(a.host.out.empty());   // none while halted
      CTimer::s_now += 10000;      // (the second: after the back-off)
      CObFollow::PollHalt();
      CHECK(a.host.out.size() >= 1);
    }
    CHECK(xo->resets == 2 && CObFollow::HaltResets(FALSE) == bo + 2);
    for (int k = 0; k < 4; ++k) {
      a.in_a4(1, ca, at, 1000.0);
      while (!a.host.out.empty()) a.out_done();
    }
    xi->ring = xo->ring = nullptr;
    opt->num.clear();
    CTimer::s_now = 0;
  }
  {  // the Rytm's USB MIDI bulk OUT (DCI 2): a send fails on a halted endpoint: reset, its record dropped, the next goes at
     // once; twice in a row too. A send that fails on an endpoint not halted (a Rytm only slow) keeps the back-off.
    CObUsbTransport t;
    CUSBMIDIDevice m;
    t.SetMIDI(&m);
    auto pass = [&] { CObUsb::PollHalt(), t.Poll(); };   // a main-loop pass: PollHalt, then the transport
    const std::vector<uint8_t> rec = {0xF0, 0x00, 0x20, 0x3C, 0x24, 0x01, 0x02, 0xF7};
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.resets == 1 && t.MIDIOutResets() == 1 && t.MIDIOutFailed() == 1 && m.sent == 0);
    CHECK(CLogger::Get()->lines.back().find("USB MIDI OUT endpoint was halted by a transfer error: reset 1") != std::string::npos);
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.sent >= 1);   // sending again with no back-off
    m.fail = 2;           // twice in a row: each failed send resets it
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.resets == 2);
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.resets == 3 && t.MIDIOutResets() == 3);
    unsigned sent = m.sent;
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.sent > sent);
    m.reset_ok = false;   // a reset refused: the merge's back-off (5 s), nothing sent meanwhile
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.resets == 4 && t.MIDIOutResets() == 3 && t.MIDIOutRefused() == 1);
    sent = m.sent;
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(m.sent == sent);
    g_ToHost.q.clear();
    t.SetMIDI(nullptr);
  }
  auto count_lines = [](const char* what, size_t from) {
    size_t n = 0;
    auto& L = CLogger::Get()->lines;
    for (size_t i = from; i < L.size(); ++i) n += L[i].find(what) != std::string::npos;
    return n;
  };
  {  // B1: an OUT endpoint that fails every transfer for 5 s (a dying cable, a box that STALLs everything), the main loop
     // passing every 1 ms: the back-off (0, 10 ms, 100 ms, then 1 s) keeps it to a few resets, each one CLEAR_FEATURE, and
     // three log lines; IN goes on throughout
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    size_t l0 = CLogger::Get()->lines.size(), c0 = r.host.ctl.size();
    unsigned passes = 0;
    for (int ms = 0; ms < 5000; ++ms) {
      CTimer::s_now += 1000;
      r.in(1, c);
      if (!r.host.out.empty() && x->state != 2) {   // the front one fails, the rest stuck behind it
        CUSBRequest* u = r.host.out.front();
        r.host.out.pop_front();
        x->state = 2;
        u->SetStatus(0), u->SetResultLen(0);
        u->fn(u, u->param, u->ctx);
      }
      CObUsb::PollHalt();
      ++passes;
    }
    CHECK(passes == 5000 && r.host.in.size() == 8);
    CHECK(x->resets >= 4 && x->resets <= 10);
    CHECK(r.host.ctl.size() - c0 == (size_t)x->resets);   // one CLEAR_FEATURE a reset, none in between
    CHECK(count_lines("OUT endpoint halted", l0) <= 4);
    x->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // R1: a completion the IRQ timed just after PollHalt read its clock (last = now + 5 us) is not a 4e9 us stall
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    CTimer::s_now = 1000000;
    r.in(1, c);
    r.in(1, c);                         // two OUT URBs pending
    CHECK(r.host.out.size() == 2);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    unsigned before = CObUsb::HaltResets(FALSE);
    size_t l0 = CLogger::Get()->lines.size();
    CTimer::s_now = 1000005;
    r.out_done();                       // OUT's last completion: now + 5, one still pending
    CTimer::s_now = 1000000;
    CObUsb::PollHalt();
    CHECK(count_lines("quiet", l0) == 0 && count_lines("halted", l0) == 0 && x->resets == 0);
    CHECK(CObUsb::HaltResets(FALSE) == before);
    CTimer::s_now = 0;
  }
  {  // R2: a marginal link, an OUT error every 3 ms with good transfers between, for 3 s: the good ones clear the
     // back-off each time, but the resets stay within kMaxPerSec (5) a second and the log within 3 lines in 10 s
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    size_t l0 = CLogger::Get()->lines.size();
    for (int ms = 0; ms < 3000; ++ms) {
      CTimer::s_now += 1000;
      r.in(1, c);
      if (!r.host.out.empty() && x->state != 2) {
        if (ms % 3 == 0) {               // the error
          CUSBRequest* u = r.host.out.front();
          r.host.out.pop_front();
          x->state = 2;
          u->SetStatus(0), u->SetResultLen(0);
          u->fn(u, u->param, u->ctx);
        } else {
          while (!r.host.out.empty()) r.out_done();   // good transfers between
        }
      }
      CObUsb::PollHalt();
    }
    CHECK(x->resets >= 3 && x->resets <= 3 * 5 + 1);
    CHECK(count_lines("OUT endpoint halted", l0) <= 3);
    x->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // B2: an endpoint that goes quiet with no error completion (halted, the event lost): URBs pending, none completed
     // for 50 ms: a stall, reset as a halt; and one quiet but running (state 1): resubmitted only, no reset counted
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    unsigned before = CObUsb::HaltResets(FALSE);
    x->state = 2;                       // halted, and no completion comes
    CTimer::s_now += 40000;
    r.in(1, c);
    CObUsb::PollHalt();
    CHECK(x->resets == 0);              // 40 ms: not yet
    CTimer::s_now += 20000;
    r.in(1, c);
    CObUsb::PollHalt();
    CHECK(x->resets == 1 && CObUsb::HaltResets(FALSE) == before + 1);
    CHECK(CLogger::Get()->lines.back().find("OUT endpoint halted (state 2") != std::string::npos);
    while (!r.host.out.empty()) r.out_done();
    r.in(1, c);
    CHECK(r.host.out.size() == 1);      // OUT again
    r.out_done();
    r.in(1, c);                         // one pending; the endpoint running but never completing it
    x->state = 1;
    CTimer::s_now += 60000;
    r.in(1, c);
    CObUsb::PollHalt();
    CHECK(x->resets == 1 && CObUsb::HaltResets(FALSE) == before + 1);   // B5: not a reset
    CHECK(CLogger::Get()->lines.back().find("OUT endpoint quiet or failed but not halted (state 1") != std::string::npos);
    x->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // B2: a reset that says OK but leaves the endpoint halted (Circle's ResetFromHalted returns !code): refused, counted,
     // and tried again after the back-off; the URBs are not resubmitted onto the halted ring
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    unsigned refused = CObUsb::HaltRefused(), before = CObUsb::HaltResets(FALSE);
    CUSBRequest* u = r.host.out.front();
    r.host.out.pop_front();
    x->state = 2, x->reset_ok = false;
    u->SetStatus(0), u->SetResultLen(0);
    u->fn(u, u->param, u->ctx);
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();
    CHECK(x->resets == 1 && x->state == 2 && CObUsb::HaltRefused() == refused + 1 && CObUsb::HaltResets(FALSE) == before);
    CHECK(CLogger::Get()->lines.back().find("reset refused") != std::string::npos);
    r.in(2, c);
    CHECK(r.host.out.empty());          // nothing onto the halted ring
    x->reset_ok = true;
    CTimer::s_now += 10000;
    CObUsb::PollHalt();
    CHECK(x->resets == 2 && CObUsb::HaltResets(FALSE) == before + 1);
    r.in(1, c);
    CHECK(!r.host.out.empty());
    x->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // B3: IN and OUT halted in the same pass (failed 6 ms apart: not together, EpBox): one reset a pass, the other at the next
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* xi = r.host.in.front()->ep->GetXHCIEndpoint();
    CXHCIEndpoint* xo = r.host.out.front()->ep->GetXHCIEndpoint();
    xi->ring = &r.host.in, xo->ring = &r.host.out;
    for (auto* q : {&r.host.in, &r.host.out}) {
      CUSBRequest* u = q->front();
      q->pop_front();
      (q == &r.host.in ? xi : xo)->state = 2;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
      CTimer::s_now += EpBox::kTogetherUs + 1000;
    }
    CObUsb::PollHalt();
    CHECK(xi->resets == 1 && xo->resets == 0 && CObUsb::ResetDone());
    CObUsb::PollHalt();
    CHECK(xi->resets == 1 && xo->resets == 1);
    CObUsb::PollHalt();
    CHECK(!CObUsb::ResetDone());
    xi->ring = xo->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // B3: the MIDI reset waits when PollHalt reset an Overbridge endpoint in the same pass; B1: a MIDI OUT that keeps
     // failing for 5 s gets a few resets (two at once, then the merge's back-off paces them), and three log lines
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* x = r.host.out.front()->ep->GetXHCIEndpoint();
    x->ring = &r.host.out;
    CObUsbTransport t;
    CUSBMIDIDevice m;
    t.SetMIDI(&m);
    host_link_is(r.ob);
    auto pass = [&] { CObUsb::PollHalt(), t.Poll(); };
    const std::vector<uint8_t> rec = {0xF0, 0x00, 0x20, 0x3C, 0x24, 0x01, 0x02, 0xF7};
    {
      CUSBRequest* u = r.host.out.front();
      r.host.out.pop_front();
      x->state = 2;
      u->SetStatus(0), u->SetResultLen(0);
      u->fn(u, u->param, u->ctx);
    }
    CTimer::s_now += EpBox::kTogetherUs;   // (the MIDI send fails after the OUT's wait: not together)
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    pass();
    CHECK(x->resets == 1 && m.resets == 0);   // the Overbridge reset took this pass
    CTimer::s_now += EpBox::kTogetherUs;   // the MIDI OUT's own wait (alone)
    pass();
    CHECK(m.resets == 1 && t.MIDIOutResets() == 1);
    while (!r.host.out.empty()) r.out_done();
    size_t l0 = CLogger::Get()->lines.size();
    m.fail = 1000000;
    for (int ms = 0; ms < 5000; ++ms) {
      CTimer::s_now += 1000;
      g_ToHost.q.push_back(rec);
      r.in(1, c);
      while (!r.host.out.empty()) r.out_done();
      pass();
    }
    CHECK(m.resets >= 3 && m.resets <= 6);
    CHECK(count_lines("USB MIDI OUT endpoint", l0) <= 3);
    g_ToHost.q.clear();
    t.SetMIDI(nullptr);
    x->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // EpBox alone: a lone failure waits kTogetherUs; two within it hold for kGraceUs from the first error, then go
     // (once: not again until none is failed); two failed further apart go at once
    EpBox b;
    EpFails one, two, apart, none;
    one.Add(true, 1000), one.Add(false, 1200);
    CHECK(one.n == 1 && b.Wait(1000, one) == 1 && b.Wait(1000 + EpBox::kTogetherUs - 1, one) == 1);
    CHECK(b.Wait(1000 + EpBox::kTogetherUs, one) == 0 && b.held == 0);
    two.Add(true, 2000), two.Add(true, 2000 + EpBox::kTogetherUs);
    CHECK(b.Wait(2000 + EpBox::kTogetherUs, two) == 3 && b.held == 1);
    CHECK(b.Wait(2000 + EpBox::kGraceUs - 1, none) == 2);   // held whatever comes meanwhile
    CHECK(b.Wait(2000 + EpBox::kGraceUs, two) == 0);        // still there: as before
    CHECK(b.Wait(2000 + EpBox::kGraceUs + 1, two) == 0 && b.held == 1);   // spent: not held again
    CHECK(b.Wait(2000 + EpBox::kGraceUs + 2, none) == 0);   // all reset: the next burst may hold again
    apart.Add(true, 0xFFFFF000u), apart.Add(true, 0xFFFFF000u + EpBox::kTogetherUs + 1);   // (across the clock's wrap)
    CHECK(b.Wait(0xFFFFF000u + EpBox::kTogetherUs + 1, apart) == 1 && b.held == 1);   // the newest: its moment
    CHECK(b.Wait(0xFFFFF000u + 2 * EpBox::kTogetherUs + 1, apart) == 0 && b.held == 1);
    CHECK(b.Wait(0, none) == 0);
    EpFails w;
    unsigned first = 0;
    w.Add(true, 0x100), w.Add(true, 0xFFFFFF00u);   // 512 us apart across the wrap: together, the earlier first
    CHECK(w.Together(EpBox::kTogetherUs, &first) && first == 0xFFFFFF00u && w.last_us == 0x100);
    CHECK(b.Wait(0x200, w) == 3 && b.held == 2 && b.Wait(0x300, none) == 2);
    EpBox c;                                         // M1: a stale failure (a refused reset, 2 s old) then a pull: held
    EpFails st;
    st.Add(true, 100000), st.Add(true, 2100000), st.Add(true, 2100300);
    CHECK(st.Together(EpBox::kTogetherUs, &first) && first == 2100000);
    CHECK(c.Wait(2100300, st) == 3 && c.since_us == 2100000);
    EpFails lone;                                    // the stale one and a new lone one: the new one waits its moment
    lone.Add(true, 100000), lone.Add(true, 3000000);
    EpBox d;
    CHECK(d.Wait(3000000, lone) == 1 && d.Wait(3000000 + EpBox::kTogetherUs, lone) == 0 && d.held == 0);
  }
  auto fail_one = [](std::deque<CUSBRequest*>& q, CXHCIEndpoint* x) {   // the front URB fails (code 4), the endpoint halted
    CUSBRequest* u = q.front();
    q.pop_front();
    x->state = 2;
    u->SetStatus(0), u->SetResultLen(0);
    u->fn(u, u->param, u->ctx);
  };
  {  // P1: a pull: IN and OUT fail in the same ms. No CLEAR_FEATURE and no reset for the grace (500 ms), one log line,
     // usbhalt held + 1; the hub then reports the disconnect and the box is removed: nothing more, nothing touches the
     // removed driver's endpoints; a box plugged in again starts afresh (a lone error waits only kTogetherUs)
    CTimer::s_now = 7000000;
    unsigned held = CObUsb::HaltHeld(), bi = CObUsb::HaltResets(TRUE), bo = CObUsb::HaltResets(FALSE);
    size_t l0 = CLogger::Get()->lines.size();
    {
      Bus r(g_rytm, 0x0b48);
      CHECK(r.ob && r.ob->Configure());
      uint16_t c = 0;
      r.in(1, c);
      CXHCIEndpoint* xi = r.host.in.front()->ep->GetXHCIEndpoint();
      CXHCIEndpoint* xo = r.host.out.front()->ep->GetXHCIEndpoint();
      xi->ring = &r.host.in, xo->ring = &r.host.out;
      size_t c0 = r.host.ctl.size();
      fail_one(r.host.in, xi);
      CTimer::s_now += 300;
      fail_one(r.host.out, xo);
      for (int ms = 0; ms < 300; ++ms) {   // the main loop passing every 1 ms; the hub says nothing for 300 ms
        CTimer::s_now += 1000;
        CObUsb::PollHalt();
      }
      CHECK(xi->resets == 0 && xo->resets == 0 && r.host.ctl.size() == c0);   // nothing sent to the departing box
      CHECK(CObUsb::HaltHeld() == held + 1);
      CHECK(count_lines("a likely unplug, their resets held up to 500 ms", l0) == 1);
      xi->ring = xo->ring = nullptr;
      delete r.ob;                        // the hub's disconnect: Circle removes the device (main loop, before PollHalt)
      r.ob = nullptr;
      CHECK(!CObUsb::Streaming());
      for (int ms = 0; ms < 600; ++ms) CTimer::s_now += 1000, CObUsb::PollHalt();   // past the grace: nothing
      CHECK(CObUsb::HaltResets(TRUE) == bi && CObUsb::HaltResets(FALSE) == bo);
    }
    Bus r(g_rytm, 0x0b48);               // plugged in again
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* xo = r.host.out.front()->ep->GetXHCIEndpoint();
    xo->ring = &r.host.out;
    fail_one(r.host.out, xo);
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();
    CHECK(xo->resets == 1 && CObUsb::HaltResets(FALSE) == bo + 1 && CObUsb::HaltHeld() == held + 1);
    xo->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // M1: OUT halted and its reset refused (it stays failed, its time old); 2 s later a pull fails IN and OUT: still held
    CTimer::s_now = 9000000;
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* xi = r.host.in.front()->ep->GetXHCIEndpoint();
    CXHCIEndpoint* xo = r.host.out.front()->ep->GetXHCIEndpoint();
    xi->ring = &r.host.in, xo->ring = &r.host.out;
    unsigned held = CObUsb::HaltHeld();
    xo->reset_ok = false;
    fail_one(r.host.out, xo);
    CTimer::s_now += EpBox::kTogetherUs;
    CObUsb::PollHalt();
    CHECK(xo->resets == 1 && xo->state == 2);   // refused: OUT stays failed
    CTimer::s_now += 300;                      // within OUT's back-off (10 ms): no retry yet
    size_t c0 = r.host.ctl.size();
    fail_one(r.host.in, xi);                   // a pull: IN fails, and OUT's next retry would go to a departing box
    CTimer::s_now += 200;
    xo->reset_ok = true;
    CObUsb::PollHalt();                        // OUT alone is stale; IN alone is new: it waits its moment
    CHECK(CObUsb::HaltHeld() == held && r.host.ctl.size() == c0);
    // the MIDI OUT fails with IN (same ms): two together, the stale OUT does not spread them apart
    CObUsbTransport t;
    CUSBMIDIDevice m;
    t.SetMIDI(&m);
    host_link_is(r.ob);
    CHECK(CObUsb::MidiSlot(0) == 0);
    const std::vector<uint8_t> rec = {0xF0, 0x00, 0x20, 0x3C, 0x24, 0x01, 0x02, 0xF7};
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    t.Poll();
    CHECK(CObUsb::HaltHeld() == held + 1 && m.resets == 0);
    for (int ms = 0; ms < 400; ++ms) CTimer::s_now += 1000, CObUsb::PollHalt(), t.Poll();
    CHECK(r.host.ctl.size() == c0 && xi->resets == 0 && xo->resets == 1 && m.resets == 0);   // nothing sent meanwhile
    g_ToHost.q.clear();
    t.SetMIDI(nullptr);
    CObUsb::SetMidiDevices(nullptr, nullptr);
    xi->ring = xo->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // P2: IN and OUT fail together but the box stays (an error burst): held for the grace, then recovered as before, IN
     // then OUT (one a pass), each with its CLEAR_FEATURE; both streams go on
    CTimer::s_now = 5000000;
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* xi = r.host.in.front()->ep->GetXHCIEndpoint();
    CXHCIEndpoint* xo = r.host.out.front()->ep->GetXHCIEndpoint();
    xi->ring = &r.host.in, xo->ring = &r.host.out;
    unsigned held = CObUsb::HaltHeld();
    size_t c0 = r.host.ctl.size();
    unsigned t0 = CTimer::s_now;
    fail_one(r.host.out, xo);
    fail_one(r.host.in, xi);
    while (CTimer::s_now - t0 < EpBox::kGraceUs - 1000) {
      CTimer::s_now += 1000;
      CObUsb::PollHalt();
    }
    CHECK(xi->resets == 0 && xo->resets == 0 && CObUsb::HaltHeld() == held + 1);
    CTimer::s_now = t0 + EpBox::kGraceUs;
    CObUsb::PollHalt();
    CHECK(xi->resets == 1 && xo->resets == 0);
    CObUsb::PollHalt();
    CHECK(xi->resets == 1 && xo->resets == 1 && r.host.ctl.size() == c0 + 2);
    CHECK(r.host.ctl[c0].req == CLEAR_FEATURE && r.host.ctl[c0].index == 0x83 && r.host.ctl[c0 + 1].index == 0x03);
    CHECK(r.host.in.size() == 8 && CObUsb::HaltHeld() == held + 1);
    c += 7 * 8;
    for (int k = 0; k < 4; ++k) {
      CTimer::s_now += 1000;
      r.in(1, c);
      CHECK(r.host.out.size() == 1);   // OUT follows IN again
      r.out_done();
      CObUsb::PollHalt();
    }
    xi->ring = xo->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // P3: the Rytm's OB OUT and its USB MIDI OUT fail together: the MIDI reset is held with the Overbridge one, and goes
     // after the grace (the failed send stays unseen until then)
    CTimer::s_now = 3000000;
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    uint16_t c = 0;
    r.in(1, c);
    CXHCIEndpoint* xo = r.host.out.front()->ep->GetXHCIEndpoint();
    xo->ring = &r.host.out;
    CObUsbTransport t;
    CUSBMIDIDevice m;
    t.SetMIDI(&m);
    host_link_is(r.ob);
    auto pass = [&] { CObUsb::PollHalt(), t.Poll(); };
    const std::vector<uint8_t> rec = {0xF0, 0x00, 0x20, 0x3C, 0x24, 0x01, 0x02, 0xF7};
    unsigned held = CObUsb::HaltHeld(), t0 = CTimer::s_now;
    fail_one(r.host.out, xo);
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    pass();                               // the MIDI send fails in the same ms
    CHECK(m.resets == 0 && xo->resets == 0 && CObUsb::HaltHeld() == held + 1);
    while (CTimer::s_now - t0 < EpBox::kGraceUs - 1000) CTimer::s_now += 1000, pass();
    CHECK(m.resets == 0 && xo->resets == 0);
    CTimer::s_now = t0 + EpBox::kGraceUs;
    pass();
    CHECK(xo->resets == 1 && m.resets == 0);   // one reset a pass: the Overbridge one first
    pass();
    CHECK(m.resets == 1 && t.MIDIOutResets() >= 1);
    g_ToHost.q.clear();
    t.SetMIDI(nullptr);
    xo->ring = nullptr;
    CTimer::s_now = 0;
  }
  {  // P4: the A4 follower: IN and OUT together held for the grace (usbhalt held), then each reset; one alone after kTogetherUs
    opt->num["a4"] = 1;
    opt->num["a4att"] = 0;
    CTimer::s_now = 2000000;
    Bus r(g_rytm, 0x0b48);
    CHECK(r.ob && r.ob->Configure());
    Bus a(g_a4, 0x0b47);
    CHECK(a.ob && a.ob->Configure() && CObFollow::Streaming());
    uint16_t ca = 0;
    long at = 0;
    a.in_a4(1, ca, at, 1000.0);
    CHECK(a.host.out.size() == 1);
    CXHCIEndpoint* xi = a.host.in.front()->ep->GetXHCIEndpoint();
    CXHCIEndpoint* xo = a.host.out.front()->ep->GetXHCIEndpoint();
    xi->ring = &a.host.in, xo->ring = &a.host.out;
    unsigned held = CObFollow::HaltHeld(), t0 = CTimer::s_now;
    size_t l0 = CLogger::Get()->lines.size();
    fail_one(a.host.in, xi);
    fail_one(a.host.out, xo);
    CObFollow::PollHalt();
    CHECK(CObFollow::HaltHeld() == held + 1 && count_lines("IN and OUT failed together: a likely unplug", l0) == 1);
    CTimer::s_now = t0 + EpBox::kGraceUs - 1;
    CHECK(!CObFollow::PollHalt() && xi->resets == 0 && xo->resets == 0);
    CTimer::s_now = t0 + EpBox::kGraceUs;
    CHECK(CObFollow::PollHalt() && xi->resets == 1);
    CHECK(CObFollow::PollHalt() && xo->resets == 1);
    a.in_a4(1, ca, at, 1000.0);
    while (!a.host.out.empty()) a.out_done();
    a.in_a4(1, ca, at, 1000.0);
    fail_one(a.host.out, xo);             // alone: after kTogetherUs, not held
    CObFollow::PollHalt();
    CHECK(xo->resets == 1);
    CTimer::s_now += EpBox::kTogetherUs;
    CObFollow::PollHalt();
    CHECK(xo->resets == 2 && CObFollow::HaltHeld() == held + 1);
    xi->ring = xo->ring = nullptr;
    opt->num.clear();
    CTimer::s_now = 0;
  }
  {  // SR1 / SR2 (T469, T472): the box that stays when the other is unplugged can freeze with its device still there:
     // nothing completes, the endpoint is not halted (state 1), the resubmits never bring it back. After ob_freeze.h's
     // kHoldMs it is removed and its port enumerated again (the software replug); a short stall is not
    struct Port : CUSBHCIRootPort {
      int rescans = 0;
      boolean ReScanDevices() override { ++rescans; return TRUE; }
    } port;
    CTimer::s_now64 = 0, CTimer::s_now = 1000000;
    auto has = [](const char* t) { for (auto& l : CLogger::Get()->lines) if (l.find(t) != std::string::npos) return true; return false; };
    {
      Bus r(g_rytm, 0x0b48);
      r.dev.root = &port;
      CHECK(r.ob && r.ob->Configure());
      uint16_t c = 0;
      for (int k = 0; k < 20; ++k) r.in(1, c), r.out_done();
      CXHCIEndpoint* xi = r.host.in.front()->ep->GetXHCIEndpoint();
      xi->state = 1;
      // a short stall (about 846 ms): resubmits, then the stream goes on: nothing removed
      for (int t = 0; t < 9; ++t) CTimer::s_now += 100000, CObUsb::PollHalt();
      CHECK(r.dev.removes == 0);
      r.in(1, c), r.out_done();
      // frozen: nothing completes for 5 s
      for (int t = 0; t < 50 && r.dev.removes == 0; ++t) CTimer::s_now += 100000, CObUsb::PollHalt();
      CHECK(r.dev.removes == 1 && port.rescans == 1 && CObUsb::FreezeReEnums() == 1);
      CHECK(has("its stream froze with the box still on the bus: enumerating it again (try 1 of 3"));
    }   // (the Bus deletes the function, as RemoveDevice would)
    {  // obreenum=0: at the next pass, whatever the stream does
      Bus r(g_rytm, 0x0b48);
      r.dev.root = &port;
      CHECK(r.ob && r.ob->Configure());
      uint16_t c = 0;
      r.in(1, c), r.out_done();
      CHECK(CObUsb::Command("obreenum=0") && !CObUsb::Command("obreen"));
      CObUsb::PollHalt();
      CHECK(r.dev.removes == 1 && port.rescans == 2 && has("asked (obreenum): enumerating it again"));
      CHECK(CObUsb::Command("obreenum"));   // the counts line
    }
    CTimer::s_now = 0;
  }
  {  // RL1 (T516): the frozen box hangs off the Pi's own hub (2109:3431) and the hub no longer answers: the port disable
     // waits its 3 s and removes nothing. No rescan (it would wait 3 s on each port); the hub's root port is reset
     // instead (obhubreset=1, twice a boot, a minute apart); otherwise nothing goes to the hub for a
     // minute. A quick refusal (the hub answered no) is a try spent, without the rescan.
    struct Port : CUSBHCIRootPort {
      int rescans = 0;
      boolean ReScanDevices() override { ++rescans; return TRUE; }
    } port;
    CUSBDevice hd;
    hd.desc.idVendor = 0x2109, hd.desc.idProduct = 0x3431;
    hd.root = &port;
    CUSBStandardHub vl805(&hd);
    CTimer::s_now64 = 0, CTimer::s_now = 200000000;   // 200 s: the SR1 budget forgotten once a box streams
    size_t l0 = CLogger::Get()->lines.size();
    // one frozen spell of the slot-0 Rytm on the hub: streams, the budget forgotten, then nothing completes
    auto freeze = [&](CUSBDevice& dev, Bus& r) {
      uint16_t c = 0;
      for (int k = 0; k < 20; ++k) r.in(1, c), r.out_done();
      CObUsb::PollHalt();   // streaming, no failures: a budget older than kForgetMs is forgotten
      r.host.in.front()->ep->GetXHCIEndpoint()->state = 1;
      int n0 = dev.removes;
      for (int t = 0; t < 60 && dev.removes == n0; ++t) CTimer::s_now += 100000, CObUsb::PollHalt();
      return dev.removes - n0;
    };
    {  // 1: no answer (3 s): the hub reset at its root port, once (a try: the box is enumerated again with the hub)
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805, r.dev.remove_ok = FALSE, r.dev.remove_us = 3000000;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1);
      CHECK(vl805.rescans == 0 && port.removes == 1 && port.rescans == 1 && CObUsb::HubResets() == 1);
      CHECK(count_lines("the Pi's USB hub did not answer (the port disable waited 3000 ms): resetting it at its root "
                        "port (reset 1 of 2 since boot)", l0) == 1);
      CHECK(count_lines("the Pi's USB hub is back after its reset", l0) == 1);
      CHECK(CObUsb::Command("obreenum") && count_lines("budget slot 0 1/3, slot 1 0/3; the Pi's hub reset 1/2)", l0) == 1);
    }   // (the Bus deletes the function, as the hub's removal would)
    {  // 2: again 10 s later: no second reset within the minute; nothing sent to the hub for a minute after
      CTimer::s_now += 10000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805, r.dev.remove_ok = FALSE, r.dev.remove_us = 3000000;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1);
      CHECK(port.removes == 1 && vl805.rescans == 0 && CObUsb::HubResets() == 1);
      CHECK(count_lines("nothing removed, nothing sent to the hub for 60 s (its last reset was less than a minute ago)", l0) == 1);
      // still frozen: the watch asks again every kHoldMs, but no request goes out, and the budget is untouched (its 1:
      // the reset's try 10 s ago)
      for (int t = 0; t < 200; ++t) CTimer::s_now += 100000, CObUsb::PollHalt();
      CHECK(r.dev.removes == 1 && port.removes == 1);
      CHECK(CObUsb::Command("obreenum") && count_lines("budget slot 0 1/3, slot 1 0/3; the Pi's hub reset 1/2, it did not answer)", l0) == 1);
    }
    {  // a minute on, the hub may be asked again: it answers no at once (a refusal): a try spent, no rescan, no reset
      CTimer::s_now += 60000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805, r.dev.remove_ok = FALSE, r.dev.remove_us = 1000;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1);
      CHECK(vl805.rescans == 0 && port.removes == 1);
      CHECK(count_lines("its hub refused to disable the port (1 ms): nothing removed, not enumerated again", l0) == 1);
      size_t l1 = CLogger::Get()->lines.size();
      CHECK(CObUsb::Command("obreenum") && count_lines("budget slot 0 1/3, slot 1 0/3; the Pi's hub reset 1/2)", l1) == 1);
    }
    {  // 3: obhubreset=0: no reset, said so
      CTimer::s_now += 120000000;
      opt->num["obhubreset"] = 0;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805, r.dev.remove_ok = FALSE, r.dev.remove_us = 3000000;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1);
      CHECK(port.removes == 1 && CObUsb::HubResets() == 1 && count_lines("(obhubreset=0)", l0) == 1);
      opt->num.erase("obhubreset");
    }
    {  // 4: the second and last reset of the boot, then spent
      CTimer::s_now += 120000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805, r.dev.remove_ok = FALSE, r.dev.remove_us = 3000000;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1 && port.removes == 2 && CObUsb::HubResets() == 2);
    }
    {
      CTimer::s_now += 120000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805, r.dev.remove_ok = FALSE, r.dev.remove_us = 3000000;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1 && port.removes == 2 && count_lines("(its resets for this boot spent)", l0) == 1);
    }
    {  // 5: a hub that answers: removed and rescanned as before (the SR1 path), no reset
      CTimer::s_now += 120000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805;
      CHECK(r.ob && r.ob->Configure());
      CHECK(freeze(r.dev, r) == 1 && vl805.rescans == 1 && port.removes == 2);
    }
    {  // 6: obhubreset!: the root-port reset at the next pass, the hub answering; outside the hub's budget
      CTimer::s_now += 120000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805;
      CHECK(r.ob && r.ob->Configure());
      uint16_t c = 0;
      r.in(1, c), r.out_done();
      size_t l1 = CLogger::Get()->lines.size();
      CHECK(CObUsb::Command("obhubreset!"));
      CObUsb::PollHalt();
      CHECK(port.removes == 3 && port.rescans == 3 && CObUsb::HubResets() == 2 && r.dev.removes == 0);
      CHECK(count_lines("obhubreset!: resetting the Pi's USB hub at its root port (Analog Rytm MKII in slot 0 on it; 1 asked", l1) == 1);
      CHECK(count_lines("the Pi's USB hub is back after its reset", l1) == 1);
    }
    {  // 7: behind another hub (not the Pi's own): never re-enumerated, and the budget is spent as before: three lines, then
       // the "frozen again" line once (no try given back: the hub was never asked)
      CTimer::s_now += 120000000;
      CUSBDevice ed;
      ed.desc.idVendor = 0x05e3, ed.desc.idProduct = 0x0608, ed.root = &port;
      CUSBStandardHub ext(&ed);
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &ext;
      CHECK(r.ob && r.ob->Configure());
      size_t l1 = CLogger::Get()->lines.size();
      uint16_t c = 0;
      for (int k = 0; k < 20; ++k) r.in(1, c), r.out_done();
      CObUsb::PollHalt();
      r.host.in.front()->ep->GetXHCIEndpoint()->state = 1;
      for (int t = 0; t < 400; ++t) CTimer::s_now += 100000, CObUsb::PollHalt();   // 40 s frozen
      CHECK(count_lines("but it hangs off another hub: no re-enumeration", l1) == 3);
      CHECK(count_lines("its stream is frozen again after 3 re-enumerations", l1) == 1);
      CHECK(r.dev.removes == 0 && ext.rescans == 0);
    }
    {  // 8: obhubnoanswer=0 (a test of HubNoAnswer on a hub that answers): nothing sent to the hub, the no-answer handling
       // runs (here the boot's two resets are spent: the quiet spell); armed again inside the spell it is used up, not kept;
       // past the spell the hub is asked as usual
      CTimer::s_now += 120000000;
      Bus r(g_rytm, 0x0b48);
      r.dev.hub = &vl805;
      CHECK(r.ob && r.ob->Configure());
      uint16_t c = 0;
      r.in(1, c), r.out_done();
      size_t l1 = CLogger::Get()->lines.size();
      int pr = port.removes, vr = vl805.rescans;
      CHECK(CObUsb::Command("obhubnoanswer=0") && !CObUsb::Command("obhubnoanswer=2"));
      CHECK(CObUsb::Command("obreenum=0"));
      CObUsb::PollHalt();
      CHECK(r.dev.removes == 0 && port.removes == pr && vl805.rescans == vr);
      CHECK(count_lines("obhubnoanswer: slot 0's port disable not sent, taken as not answered (a test)", l1) == 1);
      CHECK(count_lines("the Pi's USB hub did not answer (the port disable waited 3000 ms): nothing removed, nothing sent "
                        "to the hub for 60 s (its resets for this boot spent)", l1) == 1);
      CTimer::s_now += 10000000;
      CHECK(CObUsb::Command("obhubnoanswer=0") && CObUsb::Command("obreenum=0"));
      CObUsb::PollHalt();
      CHECK(r.dev.removes == 0 && count_lines("obhubnoanswer: not used: the hub did not answer 10 s ago", l1) == 1);
      CTimer::s_now += 60000000;
      CHECK(CObUsb::Command("obreenum=0"));
      CObUsb::PollHalt();
      CHECK(r.dev.removes == 1 && vl805.rescans == vr + 1 && port.removes == pr);   // asked as usual: answered, rescanned
    }
    CTimer::s_now = 0;
  }
  {  // a Rytm at full speed (issue 11, the A4 replug): refused before any request, its root port enumerates it again
    struct Port : CUSBHCIRootPort {
      int rescans = 0;
      boolean ReScanDevices() override { ++rescans; return TRUE; }
    } port;
    CTimer::s_now64 = 10000000;   // 10 s
    auto lines = CLogger::Get()->lines.size();
    {
      Bus b(g_rytm, 0x0b48);
      b.dev.speed = USBSpeedFull, b.dev.root = &port;
      CHECK(b.ob && !b.ob->Configure());
      CHECK(b.host.ctl.empty() && b.host.in.empty());   // no SET_INTERFACE, no URB
      CHECK(CLogger::Get()->lines.size() > lines);
      CHECK(CLogger::Get()->lines.back().find("came up at FS, Overbridge needs HS: enumerating it again in 2 s (try 1 of 3)") != std::string::npos);
    }
    CTimer::s_now64 = 11999000, CObUsb::PollReEnum();
    CHECK(port.rescans == 0);
    CTimer::s_now64 = 12000000, CObUsb::PollReEnum(), CObUsb::PollReEnum();
    CHECK(port.rescans == 1);   // once
    CHECK(port.removes == 1);   // the refused box removed first: its MIDI kept it on the port (SR4, T482)
    {  // the port's new reset gives high speed: streams, and the budget is back
      Bus b(g_rytm, 0x0b48);
      b.dev.root = &port;
      CHECK(b.ob && b.ob->Configure());
    }
    CTimer::s_now64 = 30000000, CObUsb::PollReEnum();
    CHECK(port.rescans == 1);
    {  // behind an external hub: it could be unplugged while a retry waits, so none; the log says to replug
      CUSBDevice hd;
      hd.desc.idVendor = 0x05e3, hd.desc.idProduct = 0x0608;   // some hub, on a root port
      hd.root = &port;
      CUSBStandardHub hub(&hd);
      Bus b(g_rytm, 0x0b48);
      b.dev.speed = USBSpeedFull, b.dev.hub = &hub;
      CHECK(b.ob && !b.ob->Configure());
      CHECK(CLogger::Get()->lines.back().find("behind a hub, so no retry") != std::string::npos);
      auto n = CLogger::Get()->lines.size();
      CTimer::s_now64 = 60000000, CObUsb::PollReEnum();
      CHECK(hub.rescans == 0 && port.rescans == 1 && CLogger::Get()->lines.size() == n);   // nothing was scheduled
    }
    {  // the Pi 4: every box hangs off the VL805's own hub (2109:3431 on a root port), which is retried
      CUSBDevice hd;
      hd.desc.idVendor = 0x2109, hd.desc.idProduct = 0x3431;
      hd.root = &port;
      CUSBStandardHub vl805(&hd);
      for (int k = 0; k < 3; ++k) {
        Bus b(g_rytm, 0x0b48);
        b.dev.speed = USBSpeedFull, b.dev.hub = &vl805, b.dev.hub_port = 3;   // Pi port 4 (0-based 3), as SR4's Rytm
        CHECK(b.ob && !b.ob->Configure());
        CHECK(CLogger::Get()->lines.back().find("(try " + std::to_string(k + 1) + " of 3)") != std::string::npos);
        CTimer::s_now64 += 10000000, CObUsb::PollReEnum();
        CHECK(vl805.rescans == k + 1);
        CHECK(vl805.removes == k + 1 && vl805.removed_at == 3);   // that port emptied before the rescan, else it is skipped
      }
      Bus b(g_rytm, 0x0b48);   // still at full speed after three: stays off, says so
      b.dev.speed = USBSpeedFull, b.dev.hub = &vl805;
      CHECK(b.ob && !b.ob->Configure());
      CHECK(CLogger::Get()->lines.back().find("still at FS after 3 tries") != std::string::npos);
      CTimer::s_now64 += 60000000, CObUsb::PollReEnum();
      CHECK(vl805.rescans == 3 && port.rescans == 1);
    }
    {  // the hub refuses to disable the port: no rescan (it would skip the port anyway), the log says so
      CUSBDevice hd;
      hd.desc.idVendor = 0x2109, hd.desc.idProduct = 0x3431;
      hd.root = &port;
      CUSBStandardHub vl805(&hd);
      vl805.remove_ok = FALSE;
      CTimer::s_now64 += 60000000;   // a new plug: the budget is full again
      Bus b(g_rytm, 0x0b48);
      b.dev.speed = USBSpeedFull, b.dev.hub = &vl805, b.dev.hub_port = 2;
      CHECK(b.ob && !b.ob->Configure());
      CTimer::s_now64 += 2000000, CObUsb::PollReEnum();
      CHECK(vl805.removes == 1 && vl805.removed_at == 2 && vl805.rescans == 0);
      CHECK(CLogger::Get()->lines.back().find("its hub refused to disable the port (0 ms): the box stays as it is") != std::string::npos);
    }
    {  // the hub does not answer the port disable (its 3 s timeout): #51's no-answer handling, the hub reset at its root
       // port (that enumerates the box again with the rest); with the boot's resets spent, nothing more sent to it
      struct RPort : CUSBHCIRootPort {
        int rescans = 0;
        boolean ReScanDevices() override { ++rescans; return TRUE; }
      } rport;
      CUSBDevice hd;
      hd.desc.idVendor = 0x2109, hd.desc.idProduct = 0x3431;
      hd.root = &rport;
      CUSBStandardHub vl805(&hd);
      vl805.remove_ok = FALSE, vl805.remove_us = 3000000;
      CObUsb::ForgetHub();
      size_t l1 = CLogger::Get()->lines.size();
      for (int k = 0; k < 3; ++k) {   // two resets a boot, a minute apart; the third finds them spent
        CTimer::s_now64 += 60000000;   // a new plug each time
        Bus b(g_rytm, 0x0b48);
        b.dev.speed = USBSpeedFull, b.dev.hub = &vl805, b.dev.hub_port = 1;
        CHECK(b.ob && !b.ob->Configure());
        CTimer::s_now64 += 2000000, CObUsb::PollReEnum();
        CHECK(vl805.removes == k + 1 && vl805.rescans == 0);
        CHECK(rport.removes == (k < 2 ? k + 1 : 2) && rport.rescans == (k < 2 ? k + 1 : 2));
      }
      CHECK(CObUsb::HubResets() == 2);
      CHECK(count_lines("Analog Rytm MKII (slot 0): the Pi's USB hub did not answer (the port disable waited 3000 ms): resetting "
                        "it at its root port", l1) == 2);
      CHECK(count_lines("nothing sent to the hub for 60 s (its resets for this boot spent)", l1) == 1);
      {  // inside the quiet spell the next retry sends nothing to it
        Bus b(g_rytm, 0x0b48);
        b.dev.speed = USBSpeedFull, b.dev.hub = &vl805, b.dev.hub_port = 1;
        CHECK(b.ob && !b.ob->Configure());
        CTimer::s_now64 += 5000000, CObUsb::PollReEnum();   // (try 2 of this plug: 5 s)
        CHECK(vl805.removes == 3 && rport.removes == 2);
        CHECK(count_lines("the Pi's USB hub did not answer 5 s ago: nothing sent to it, the box stays off", l1) == 1);
      }
      CObUsb::ForgetHub();
    }
    {  // obfsnext=<slot> (a test): a box at high speed in that slot refused once as if at full speed, then enumerated again
       // as SR4's needs (removed, rescanned); it comes back at high speed and streams. Used once; another slot untouched.
       // With obhubnoanswer=<slot> too, the retry's port disable is taken as not answered: the hub reset
      struct RPort : CUSBHCIRootPort {
        int rescans = 0;
        boolean ReScanDevices() override { ++rescans; return TRUE; }
      } rport;
      CUSBDevice hd;
      hd.desc.idVendor = 0x2109, hd.desc.idProduct = 0x3431;
      hd.root = &rport;
      CUSBStandardHub vl805(&hd);
      size_t l1 = CLogger::Get()->lines.size();
      CHECK(CObUsb::Command("obfsnext=0") && !CObUsb::Command("obfsnext=2") && !CObUsb::Command("obfsnext="));
      CTimer::s_now64 += 60000000;
      {
        Bus b(g_rytm, 0x0b48);
        b.dev.hub = &vl805, b.dev.hub_port = 3;   // at high speed
        CHECK(b.ob && !b.ob->Configure());
        CHECK(b.host.ctl.empty() && b.host.in.empty());
        CHECK(count_lines("obfsnext: slot 0 taken as full speed (a test; it is at HS)", l1) == 1);
        CHECK(CLogger::Get()->lines.back().find("came up at FS, Overbridge needs HS: enumerating it again in 2 s (try 1 of 3)") != std::string::npos);
      }
      CTimer::s_now64 += 2000000, CObUsb::PollReEnum();
      CHECK(vl805.removes == 1 && vl805.removed_at == 3 && vl805.rescans == 1);
      {
        Bus b(g_rytm, 0x0b48);   // back at high speed: streams; the hook is used up
        b.dev.hub = &vl805, b.dev.hub_port = 3;
        CHECK(b.ob && b.ob->Configure());
      }
      CTimer::s_now64 += 60000000, CObUsb::PollReEnum();
      CHECK(vl805.rescans == 1);
      CHECK(CObUsb::Command("obfsnext=1"));
      {
        Bus b(g_rytm, 0x0b48);   // slot 0 again: not this one's
        b.dev.hub = &vl805;
        CHECK(b.ob && b.ob->Configure());
      }
      CHECK(CObUsb::Command("obfsnext=0") && CObUsb::Command("obhubnoanswer=0"));   // (slot 1's overwritten)
      CTimer::s_now64 += 60000000;
      {
        Bus b(g_rytm, 0x0b48);
        b.dev.hub = &vl805, b.dev.hub_port = 2;
        CHECK(b.ob && !b.ob->Configure());
      }
      CTimer::s_now64 += 2000000, CObUsb::PollReEnum();
      CHECK(vl805.removes == 1 && rport.removes == 1 && rport.rescans == 1 && CObUsb::HubResets() == 1);
      CHECK(count_lines("obhubnoanswer: slot 0's port disable not sent, taken as not answered (a test)", l1) == 1);
      CObUsb::ForgetHub();
    }
    CTimer::s_now64 = 0;
  }

  {  // obaspmoff (default 1): the VL805 ASPM bits written back to off within 10 ms whenever they read set
    auto& lnk = CBcmPCIeHostBridge::s_cfg[1][0xd4];
    size_t l0 = CLogger::Get()->lines.size();
    CTimer::s_now = 50000000;
    CObUsb::PollHalt();
    CHECK(count_lines("VL805 ASPM was on", l0) == 0 && lnk == 0x10120040);
    lnk = 0x10120043;                                   // the VL805 turns L0s+L1 on
    CTimer::s_now += 5000;
    CObUsb::PollHalt();
    CHECK(lnk == 0x10120043);                           // not within 10 ms of the last look
    CTimer::s_now += 6000;
    CObUsb::PollHalt();
    CHECK(lnk == 0x10120040);
    CHECK(count_lines("VL805 ASPM was on (Link Control/Status 10120043): set off, reads 10120040 (", l0) == 1);
    CHECK(count_lines(" did not take)", l0) == 1 && count_lines(", 0 did not take)", l0) == 1);
    CBcmPCIeHostBridge::s_write_ok = false;             // a write that does not take: counted, said
    lnk = 0x10120041;
    CTimer::s_now += 11000;
    CObUsb::PollHalt();
    CHECK(count_lines("reads 10120041 - DID NOT TAKE (", l0) == 1 && count_lines(", 1 did not take)", l0) == 1);
    CBcmPCIeHostBridge::s_write_ok = true;
    CTimer::s_now += 11000;
    CObUsb::PollHalt();
    CHECK(lnk == 0x10120040);
    CHECK(count_lines("VL805 ASPM kept off (obaspmoff=1): Link Control/Status 10120040", 0) == 1);
    CTimer::s_now = 0;
  }
  {  // same type twice (design 2026-11-26): two Rytms, told apart by the UID of the vendor request 0, each its own slot; their
     // USB MIDI devices carry the same strings, and each is matched to its own stream by its USB device
    Bus a(g_rytm, 0x0b48, 0x1234abcd);
    CHECK(a.ob && core::device::g_attach_uid == 0x1234abcd && ((CObUsb*)a.ob)->Uid() == 0x1234abcd);
    CHECK(a.host.vendor.size() == 1 && a.host.vendor[0].type == 0xC0 && a.host.vendor[0].req == 0 && a.host.vendor[0].index == 0);
    Bus b(g_rytm, 0x0b48, 0x9f003fa2);
    CHECK(b.ob && ((CObUsb*)b.ob)->Uid() == 0x9f003fa2);
    const unsigned sa = ((CObUsb*)a.ob)->Slot(), sb = ((CObUsb*)b.ob)->Slot();
    CHECK(sa != sb && CObUsb::Box(sa) == (CObUsb*)a.ob && CObUsb::Box(sb) == (CObUsb*)b.ob);
    CUSBMIDIDevice ma, mb;   // the same strings, as Circle copies them from two Rytms
    for (CDevice* d : {(CDevice*)&ma, (CDevice*)&mb, (CDevice*)a.ob, (CDevice*)b.ob}) d->SetProperty(CDevice::PropertyProduct, "Analog Rytm MKII");
    ma.usb = &a.dev, mb.usb = &b.dev;
    CObUsb::SetMidiDevices(&mb, &ma);   // B's MIDI on the host link, A's on link1
    CHECK(CObUsb::MidiSlot(0) == (int)sb && CObUsb::MidiSlot(1) == (int)sa);
    CHECK(core::device::g_midi_slot[0] == (int)sb && core::device::g_midi_slot[1] == (int)sa);
    CObUsb::SetMidiDevices(&ma, &mb);
    CHECK(CObUsb::MidiSlot(0) == (int)sa && CObUsb::MidiSlot(1) == (int)sb);
    CObUsb::SetMidiDevices(nullptr, nullptr);
    CHECK(CObUsb::MidiSlot(0) == -1 && core::device::g_midi_slot[0] == -1);
    // a box that does not answer the vendor request: no UID (the core asks by MIDI)
    Bus c(g_rytm, 0x0b48);
    CHECK(c.ob == nullptr && core::device::g_attach_uid == 0 && c.host.vendor.size() == 1);   // (a third box: no slot)
  }
  printf("test_ob_usb: ok\n");
  return 0;
}
