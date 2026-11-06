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
const Profile* by_usb(uint16_t vid, uint16_t pid) {   // device.cpp's list, without the engine it links
  static Profile rytm;
  rytm.audio = &out::kRytmMk2;
  if (vid == 0x1935 && pid == 0x0b48) return &rytm;
  if (vid == kA4Vid && pid == kA4Pid) return g_a4_chosen ? &kA4 : nullptr;
  return vid == kDigitakt.usb_vid && pid == kDigitakt.usb_pid ? &kDigitakt : nullptr;
}
}}
static int g_rytm_usb = 0;
namespace core { namespace device { void rytm_on_usb(bool on) { g_rytm_usb += on ? 1 : -1; } }}
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
  Bus(const uint8_t* cfg, uint16_t pid) {
    dev.config = cfg;
    dev.desc.idVendor = 0x1935, dev.desc.idProduct = pid;
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

int main() {
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
    {  // a second box while one streams: no second stream, and its id is still noted when it is a Rytm
      Bus s(g_digitakt, 0x0b2c);
      CHECK(s.ob == nullptr && g_rytm_usb == 1);
    }
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
    const std::vector<uint8_t> rec = {0xF0, 0x00, 0x20, 0x3C, 0x24, 0x01, 0x02, 0xF7};
    m.fail = 1;
    g_ToHost.q.push_back(rec);
    t.Poll();
    CHECK(CObUsb::HaltHeld() == held + 1 && m.resets == 0);
    for (int ms = 0; ms < 400; ++ms) CTimer::s_now += 1000, CObUsb::PollHalt(), t.Poll();
    CHECK(r.host.ctl.size() == c0 && xi->resets == 0 && xo->resets == 1 && m.resets == 0);   // nothing sent meanwhile
    g_ToHost.q.clear();
    t.SetMIDI(nullptr);
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
        b.dev.speed = USBSpeedFull, b.dev.hub = &vl805;
        CHECK(b.ob && !b.ob->Configure());
        CHECK(CLogger::Get()->lines.back().find("(try " + std::to_string(k + 1) + " of 3)") != std::string::npos);
        CTimer::s_now64 += 10000000, CObUsb::PollReEnum();
        CHECK(vl805.rescans == k + 1);
      }
      Bus b(g_rytm, 0x0b48);   // still at full speed after three: stays off, says so
      b.dev.speed = USBSpeedFull, b.dev.hub = &vl805;
      CHECK(b.ob && !b.ob->Configure());
      CHECK(CLogger::Get()->lines.back().find("still at FS after 3 tries") != std::string::npos);
      CTimer::s_now64 += 60000000, CObUsb::PollReEnum();
      CHECK(vl805.rescans == 3 && port.rescans == 1);
    }
    CTimer::s_now64 = 0;
  }

  printf("test_ob_usb: ok\n");
  return 0;
}
