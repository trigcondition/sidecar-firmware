// pi/engine/tests/test_ob_usb.cpp - the real ob_usb.cpp on a fake Circle (shim_ob/): the descriptors a Digitakt and a Rytm send, the
// sequence on the control endpoint, the URBs' sizes, OUT paced by IN whatever the two sizes are, the bytes of an OUT block. The
// descriptors are what the firmware sent an emulated host (rytm-emu dtusb 692c4cc, dtboot --usb-host: Digitakt OS 1.53 and 1.54,
// OVERBRIDGE; Rytm OS 1.73): the Digitakt's matches a physical capture byte for byte (docs/device-profiles.md, "Audio").
#include "check.h"
#include "device/digitakt.h"
#include "fx/out.h"
#include "ob_block.h"
#include "ob_usb.h"
#include "shared.h"

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
namespace core { namespace device {
const Profile* by_usb(uint16_t vid, uint16_t pid) {   // device.cpp's list, without the engine it links
  static Profile rytm;
  rytm.audio = &out::kRytmMk2;
  if (vid == 0x1935 && pid == 0x0b48) return &rytm;
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

struct Bus {
  CUSBDevice dev;
  CUSBHostController host;
  CUSBFunction parent{&dev, &host};
  CObUsb* ob = nullptr;
  Bus(const uint8_t* cfg, uint16_t pid) {
    dev.config = cfg;
    dev.desc = {0x1935, pid};
    ob = (CObUsb*)RytmObDeviceCreate(&parent);
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

  printf("test_ob_usb: ok\n");
  return 0;
}
