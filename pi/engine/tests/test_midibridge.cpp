// pi/engine/tests/test_midibridge.cpp - the Rytm MIDI bridge's portable parts (midibridge.h, usbmidi_merge.h): ob_usb.cpp's
// Poll itself with a fake endpoint (the order, no packet inside a SysEx, the pace, the hold-back cap, a failed send),
// the parser, the OS upgrade and +Drive write blocks, the challenge (HMAC, the per-address lockout), the hello line,
// the status JSON (no token, no address in /status), and the non-blocking copy to the client.
#include "check.h"
#include "midibridge.h"
#include "usbmidi_merge.h"

#include <stdio.h>
#include <string.h>
#include <vector>

using namespace midibridge;
using Bytes = std::vector<uint8_t>;

static ToRytm g_to;			// 320 KB each: not on the stack
static ToRytm g_to2;
static FromRytm<1024> g_from;
static FromRytm<1 << 16> g_from_big;

// ---- USB MIDI packets back to a byte stream, checking each packet's CIN against its bytes
static Bytes unpack(const uint8_t* p, unsigned n) {
  Bytes out;
  for (unsigned i = 0; i < n; i += 4) {
    uint8_t cin = p[i] & 15;
    CHECK((p[i] >> 4) == 0);		// cable 0
    unsigned k = cin == 4 || cin == 7 || cin == 3 || (cin >= 8 && cin <= 0xE && cin != 0xC && cin != 0xD) ? 3
		 : cin == 6 || cin == 2 || cin == 0xC || cin == 0xD ? 2 : 1;
    for (unsigned j = 0; j < k; j++) out.push_back(p[i + 1 + j]);
  }
  return out;
}

// the packet stream's rule: between a CIN 4 run's start (F0) and its end (CIN 5/6/7 with F7) only CIN 4 packets or
// single-byte realtime (CIN F)
static bool no_interleave(const std::vector<uint8_t>& pk) {
  bool in = false;
  for (size_t i = 0; i < pk.size(); i += 4) {
    uint8_t cin = pk[i] & 15;
    if (cin == 4) { if (!in && pk[i + 1] != 0xF0) return false; in = true; continue; }
    if (cin >= 5 && cin <= 7 && (in || pk[i + 1] == 0xF0)) {
      bool has_f7 = false;
      for (int j = 1; j <= 3; j++) if (pk[i + j] == 0xF7) has_f7 = true;
      if (!has_f7 && cin != 5) return false;
      if (cin == 5 && pk[i + 1] != 0xF7 && pk[i + 1] != 0xF6) return false;
      in = false;
      continue;
    }
    if (cin == 0xF) continue;
    if (in) return false;		// a note / CC / the Sidecar's SysEx start inside a SysEx
  }
  return !in;
}

static Bytes sysex(unsigned n, uint8_t seed) {	// F0, n-2 data bytes, F7
  Bytes m(n);
  m[0] = 0xF0;
  for (unsigned i = 1; i + 1 < n; i++) m[i] = (uint8_t)((i * 7 + seed) & 0x7F);
  m[n - 1] = 0xF7;
  return m;
}

static Bytes pack7(const Bytes& d) {
  Bytes out;
  for (size_t i = 0; i < d.size(); i += 7) {
    uint8_t top = 0;
    size_t g = d.size() - i < 7 ? d.size() - i : 7;
    for (size_t k = 0; k < g; k++) if (d[i + k] & 0x80) top |= 0x40 >> k;
    out.push_back(top);
    for (size_t k = 0; k < g; k++) out.push_back(d[i + k] & 0x7F);
  }
  return out;
}
static Bytes rpc(uint8_t cmd, const Bytes& args = {}) {	// elkflash's frame(): F0 00 20 3C 10 00, packed, F7
  Bytes msg = {0x00, 0x05, 0x00, 0x00, cmd};
  msg.insert(msg.end(), args.begin(), args.end());
  Bytes m = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00};
  Bytes p = pack7(msg);
  m.insert(m.end(), p.begin(), p.end());
  m.push_back(0xF7);
  return m;
}

static Bytes rpc_dev(uint8_t dev, uint8_t cmd, const Bytes& args = {}) {
  Bytes m = rpc(cmd, args);
  m[4] = dev;
  return m;
}

static std::vector<uint8_t> drain(ToRytm& t) {
  std::vector<uint8_t> all;
  uint8_t pk[4 * 400];
  for (int i = 0; t.pending() && i < 100000; i++) { unsigned k = t.take(pk, sizeof pk, kPollBytes); all.insert(all.end(), pk, pk + k); }
  return all;
}

// ---- ob_usb.cpp's Poll itself (usbmidi_merge.h) with a fake endpoint
struct FakeIo : UsbMidiMerge::Io {
  std::vector<uint8_t> wire;		// every packet the Rytm got, in order
  std::vector<uint32_t> keys, comp;
  std::vector<Bytes> engine;
  bool computer_sx = false, gadget = true;
  int fail_after = -1;			// the n-th send from now on fails (-1 never)
  bool fail_always = false;		// a Rytm that is enumerated but not reading
  unsigned sends = 0, warns = 0, bytes_this_poll = 0;
  bool send(const uint8_t* p, unsigned n) override {
    CHECK(n > 0 && n <= UsbMidiMerge::kChunk && n % 4 == 0);
    sends++;
    if (fail_always) return false;
    if (fail_after == 0) { fail_after = -1; return false; }
    if (fail_after > 0) fail_after--;
    wire.insert(wire.end(), p, p + n);
    bytes_this_poll += n;
    return true;
  }
  bool key(uint32_t* w) override { if (keys.empty()) return false; *w = keys.front(); keys.erase(keys.begin()); return true; }
  bool has_computer() override { return gadget; }
  void computer_connected(bool) override {}
  // the gadget's own rule: its SysEx is open from the F0 packet it handed over until the one with the F7
  bool computer(uint32_t* w) override {
    if (comp.empty()) return false;
    *w = comp.front();
    comp.erase(comp.begin());
    uint8_t cin = *w & 15;
    if (cin == 4) computer_sx = true;
    else if (cin >= 5 && cin <= 7) computer_sx = false;
    return true;
  }
  bool computer_in_sysex() override { return computer_sx; }
  Bytes cur;
  const uint8_t* engine_next(int* len) override {
    if (engine.empty()) return nullptr;
    cur = engine.front();
    engine.erase(engine.begin());
    *len = (int)cur.size();
    return cur.data();
  }
  unsigned engine_drain() override { unsigned n = 0; for (auto& r : engine) n += r.size(); engine.clear(); return n; }
  void warn(const char*) override { warns++; }
};
static uint32_t pk(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { return (uint32_t)a | b << 8 | c << 16 | (uint32_t)d << 24; }

static UsbMidiMerge g_merge;

static void test_merge_poll() {
  ToRytm& t = g_to;
  Pacer pace;
  UsbMidiMerge& m = g_merge;
  FakeIo io;
  // the bridge's: a note, a 5000-byte SysEx with a clock in it, running-status CCs, a program change, a tune request
  Bytes stream = {0x90, 0x3C, 0x7F};
  Bytes sx = sysex(5000, 3);
  for (size_t i = 0; i < sx.size(); i++) { stream.push_back(sx[i]); if (i == 2500) stream.push_back(0xF8); }
  Bytes tail = {0xB0, 0x10, 0x20, 0x11, 0x21, 0xC0, 0x05, 0xF6};
  stream.insert(stream.end(), tail.begin(), tail.end());
  for (size_t i = 0; i < stream.size(); i += 7) {
    unsigned k = stream.size() - i < 7 ? stream.size() - i : 7;
    CHECK(t.feed(&stream[i], k) == k);
  }
  CHECK(t.stats().msgs == 7 && t.stats().junk == 0);
  // 1 ms Polls; the Sidecar's own traffic every Poll: a key, the computer's SysEx in three Polls, the engine's
  // song dump in 3069-byte pieces and its 13-byte replies
  uint32_t now = 1000000;
  Bytes dump = sysex(3069 * 2 + 100, 7);
  int polls = 0, held = 0, held_max = 0;
  unsigned max_poll = 0;
  uint64_t sent0 = t.stats().sent;
  while (t.pending() || !io.engine.empty() || !io.keys.empty() || !io.comp.empty() || polls < 8) {
    io.keys.push_back(pk(0x09, 0x90, 0x24, 0x40));
    if (polls == 2) {				// the engine's dump: three pieces
      io.engine.push_back(Bytes(dump.begin(), dump.begin() + 3069));
      io.engine.push_back(Bytes(dump.begin() + 3069, dump.begin() + 6138));
      io.engine.push_back(Bytes(dump.begin() + 6138, dump.end()));
    }
    if (polls % 5 == 0) io.engine.push_back(sysex(13, 1));
    if (polls == 4) io.comp.push_back(pk(0x04, 0xF0, 0x7D, 0x01));
    if (polls == 6) { io.comp.push_back(pk(0x04, 0x02, 0x03, 0x04)); io.comp.push_back(pk(0x05, 0xF7, 0, 0)); }
    size_t keys_before = io.keys.size();
    io.bytes_this_poll = 0;
    uint64_t s0 = t.stats().sent;
    m.poll(io, true, true, &t, &pace, now);
    unsigned bridge_bytes = (unsigned)(t.stats().sent - s0);
    if (bridge_bytes > max_poll) max_poll = bridge_bytes;
    if (io.keys.size() >= keys_before) { held++; if (held > held_max) held_max = held; } else held = 0;
    now += 1000;
    polls++;
    CHECK(polls < 2000);
  }
  CHECK(no_interleave(io.wire));
  CHECK(io.warns == 0 && !m.failed());
  CHECK(max_poll <= kPollBytes);
  // paced: 5014 bytes at 250 KB/s take ~20 ms of Polls
  CHECK(t.stats().sent - sent0 == 3 + 1 + sx.size() + 9);
  CHECK(polls >= (int)((t.stats().sent - sent0 - kPollBytes) * 1000 / kRate));
  CHECK((unsigned)held_max <= kMaxHoldMs + 2);
  // everything arrived, each in one piece: the bridge's SysEx, the engine's dump, the bridge's tail
  Bytes all = unpack(io.wire.data(), io.wire.size());
  auto has = [&](const Bytes& b) {
    for (size_t k = 0; k + b.size() <= all.size(); k++) if (memcmp(&all[k], b.data(), b.size()) == 0) return true;
    return false;
  };
  Bytes want_tail = {0xB0, 0x10, 0x20, 0xB0, 0x11, 0x21, 0xC0, 0x05, 0xF6};	// running status made explicit
  CHECK(has(sx));
  CHECK(has(dump));
  // the bridge's short messages in order (the Sidecar's own whole messages may go between them)
  size_t at = 0;
  for (size_t q = 0; q < want_tail.size(); q += (want_tail[q] == 0xC0 ? 2 : want_tail[q] == 0xF6 ? 1 : 3)) {
    size_t len = want_tail[q] == 0xC0 ? 2 : want_tail[q] == 0xF6 ? 1 : 3;
    bool found = false;
    for (; at + len <= all.size(); at++)
      if (memcmp(&all[at], &want_tail[q], len) == 0) { found = true; at += len; break; }
    CHECK(found);
  }
  printf("merge: %d Polls, a bridge SysEx held the keys %d ms at most, %u bytes a Poll at most, no interleaving\n",
         polls, held_max, max_poll);

  // the pace over a second: at most kRate (+ the bucket)
  ToRytm* t2 = new ToRytm;
  Pacer p2;
  UsbMidiMerge* m2 = new UsbMidiMerge;
  FakeIo io2;
  io2.gadget = false;
  Bytes big = sysex(30000, 2);
  for (int i = 0; i < 8; i++) CHECK(t2->feed(big.data(), big.size()) == big.size());	// 240 KB queued
  uint64_t at900 = 0;
  for (int i = 0; i < 1000; i++) {
    m2->poll(io2, true, true, t2, &p2, 5000000 + i * 1000u);
    if (i == 899) at900 = t2->stats().sent;
  }
  CHECK(t2->stats().sent <= kRate + kPollBytes);			// a second: kRate and the bucket at most
  CHECK(at900 >= (uint64_t)kRate * 9 / 10 - 2000);			// and not much less while there is work
  printf("pace: %lu bytes in 1 s of Polls (rate %u)\n", (unsigned long)t2->stats().sent, kRate);

  // H1: a bridge send fails in the middle of a SysEx: the sending goes off (one stall), the rest of that SysEx and
  // the queue are dropped, nothing more is sent until a new umidi1 - and then the next message goes out whole
  CHECK(t2->feed(big.data(), big.size()) == big.size() && t2->feed(big.data(), big.size()) == big.size());
  m2->poll(io2, true, true, t2, &p2, 5500000);	// the first piece goes
  CHECK(t2->open());
  io2.fail_after = 0;
  io2.wire.clear();
  unsigned sends0 = io2.sends;
  m2->poll(io2, true, true, t2, &p2, 6000000);
  CHECK(m2->failed() && io2.warns == 1 && m2->bridge_failed() == 1);
  CHECK(!t2->pending() && !t2->open());
  unsigned sends1 = io2.sends;
  CHECK(sends1 == sends0 + 1);			// the failing one only
  io2.keys = {pk(0x09, 0x90, 1, 2)};
  io2.engine.push_back(sysex(13, 1));
  CHECK(t2->feed(big.data(), 100) == 100);	// part of a SysEx (no F7): not queued yet
  Bytes note = {0x90, 1, 2};
  for (int i = 0; i < 50; i++) m2->poll(io2, true, true, t2, &p2, 6001000 + i * 1000u);
  CHECK(io2.sends == sends1 && io2.wire.empty() && io2.warns == 1);	// off: no more stalls
  CHECK(io2.keys.empty() && io2.engine.empty());			// dropped, not piled up
  // R2-L1: no new umidi1 (the Rytm was only busy): tried again after kRetryUs by itself, backing off while it fails
  m2->poll(io2, true, true, t2, &p2, 6000000 + UsbMidiMerge::kRetryUs - 1000);
  CHECK(m2->failed() && io2.wire.empty());
  CHECK(t2->feed(note.data(), 3) == 3);
  m2->poll(io2, true, true, t2, &p2, 6000000 + UsbMidiMerge::kRetryUs);
  CHECK(!m2->failed() && m2->retries() == 1 && io2.wire.size() == 4 && io2.wire[1] == 0x90);
  io2.fail_after = 0;					// it fails again (after a success: 5 s again)
  CHECK(t2->feed(note.data(), 3) == 3);
  uint32_t f2 = 6000000 + UsbMidiMerge::kRetryUs + 1000;
  m2->poll(io2, true, true, t2, &p2, f2);
  CHECK(m2->failed());
  io2.fail_after = 0;					// and its retry fails too: off for twice as long
  CHECK(t2->feed(note.data(), 3) == 3);
  m2->poll(io2, true, true, t2, &p2, f2 + UsbMidiMerge::kRetryUs);
  CHECK(m2->failed() && m2->retries() == 2);
  uint32_t f3 = f2 + UsbMidiMerge::kRetryUs;
  m2->poll(io2, true, true, t2, &p2, f3 + UsbMidiMerge::kRetryUs);
  CHECK(m2->failed());
  m2->poll(io2, true, true, t2, &p2, f3 + 2 * UsbMidiMerge::kRetryUs);
  CHECK(!m2->failed() && m2->retries() == 3);
  io2.wire.clear();
  m2->rytm_new();
  t2->flush(false);
  CHECK(t2->feed(note.data(), 3) == 3);
  m2->poll(io2, true, true, t2, &p2, 7000000);
  CHECK(io2.wire.size() == 4 && io2.wire[1] == 0x90 && !m2->failed());
  // the engine's failure: the same rule
  io2.engine.push_back(sysex(13, 1));
  io2.fail_after = 0;
  io2.wire.clear();
  m2->poll(io2, true, true, t2, &p2, 7001000);
  CHECK(m2->failed() && m2->failures() == 1 && io2.wire.empty());
  // no Rytm at all: the bridge's queue is dropped, counted
  m2->rytm_new();
  CHECK(t2->feed(note.data(), 3) == 3);
  unsigned fl = t2->stats().flushed;
  m2->poll(io2, false, true, t2, &p2, 7002000);
  CHECK(!t2->pending() && t2->stats().flushed == fl + 1);
  // the computer's SysEx open: neither the keys nor the bridge go, the engine neither (the early return)
  m2->rytm_new();
  io2.gadget = true;
  io2.comp = {pk(0x04, 0xF0, 0x7D, 0x01)};
  m2->poll(io2, true, true, t2, &p2, 7002500);	// its SysEx starts
  CHECK(io2.computer_sx);
  io2.wire.clear();
  io2.keys = {pk(0x09, 0x90, 1, 2)};
  io2.engine.push_back(sysex(13, 1));
  CHECK(t2->feed(note.data(), 3) == 3);
  m2->poll(io2, true, true, t2, &p2, 7003000);
  CHECK(io2.wire.empty() && t2->pending() && io2.keys.size() == 1 && io2.engine.size() == 1);
  io2.comp = {pk(0x05, 0xF7, 0, 0)};
  m2->poll(io2, true, true, t2, &p2, 7004000);	// its F7: the engine and the bridge go after it
  CHECK(!io2.computer_sx && io2.engine.empty() && !t2->pending());
  m2->poll(io2, true, true, t2, &p2, 7005000);	// the keys (they were asked before the F7 went)
  CHECK(!t2->pending() && io2.keys.empty() && io2.engine.empty() && no_interleave(io2.wire));
  // M4: the largest SysEx the bridge takes holds a key back kMaxHoldMs at most
  m2->rytm_new();
  t2->flush(false);
  io2.wire.clear();
  Bytes maxsx = sysex(37457, 4);
  CHECK(t2->feed(maxsx.data(), maxsx.size()) == maxsx.size());
  Pacer p3;
  unsigned ms = 0;
  m2->poll(io2, true, true, t2, &p3, 9000000);
  io2.keys = {pk(0x09, 0x90, 9, 9)};
  for (; !io2.keys.empty() && ms < 1000; ms++) m2->poll(io2, true, true, t2, &p3, 9001000 + ms * 1000u);
  CHECK(ms <= kMaxHoldMs + 2 && no_interleave(io2.wire));
  // an engine SysEx piece whose next piece never comes: the others wait kEngineStall Polls, then go again
  Bytes piece = sysex(400, 1);
  piece.resize(300);
  io2.engine.push_back(piece);
  m2->poll(io2, true, true, t2, &p3, 9499000);
  CHECK(m2->engine_open());
  io2.keys = {pk(0x09, 0x90, 8, 8)};
  unsigned st = 0;
  for (; !io2.keys.empty() && st < 1000; st++) m2->poll(io2, true, true, t2, &p3, 9500000 + st * 1000u);
  CHECK(st >= UsbMidiMerge::kEngineStall && st <= UsbMidiMerge::kEngineStall + 2 && m2->engine_stalls() == 1);
  printf("hold: a %u-byte bridge SysEx held a key %u ms (cap %u ms)\n", (unsigned)maxsx.size(), ms, kMaxHoldMs);
  delete t2;
  delete m2;
}

// R2-M1: the computer's realtime (its clock) never waits behind a hold: one F8 a Poll, during a bridge SysEx of the
// largest size and during an engine song dump in pieces, each out in the Poll it arrived; its notes wait, in order
static void test_realtime_through_holds() {
  ToRytm* t = new ToRytm;
  UsbMidiMerge* m = new UsbMidiMerge;
  Pacer p;
  FakeIo io;
  Bytes maxsx = sysex(37457, 4);
  CHECK(t->feed(maxsx.data(), maxsx.size()) == maxsx.size());
  uint32_t now = 20000000;
  int held = 0, late = 0, polls = 0;
  Bytes dump = sysex(3069 * 3 + 50, 6);
  auto engine_pieces = [&]() {
    for (size_t o = 0; o < dump.size(); o += 3069)
      io.engine.push_back(Bytes(dump.begin() + o, dump.begin() + (o + 3069 < dump.size() ? o + 3069 : dump.size())));
  };
  for (; polls < 400; polls++, now += 1000) {
    if (polls == 200) engine_pieces();
    io.comp.push_back(pk(0x0F, 0xF8, 0, 0));
    if (polls % 7 == 0) io.comp.push_back(pk(0x09, 0x90, 0x30, 0x40));	// a note from the DAW: it may wait
    size_t w0 = io.wire.size();
    bool hold_before = t->open() || m->engine_open();
    // the engine's dump goes a piece a Poll: hold it back so it spans Polls
    std::vector<Bytes> later;
    if (io.engine.size() > 1) { later.assign(io.engine.begin() + 1, io.engine.end()); io.engine.resize(1); }
    m->poll(io, true, true, t, &p, now);
    io.engine.insert(io.engine.end(), later.begin(), later.end());
    bool f8 = false;
    for (size_t i = w0; i + 4 <= io.wire.size(); i += 4) if (io.wire[i] == 0x0F && io.wire[i + 1] == 0xF8) f8 = true;
    if (hold_before || t->open() || m->engine_open()) held++;
    if (!f8) late++;
  }
  CHECK(held > 150);					// the test did hold, both ways
  CHECK(late == 0);					// every clock went in the Poll it came: at most one Poll late
  CHECK(io.comp.empty() && m->deferred() == 0);
  // the notes came out in order, and never inside a SysEx
  std::vector<uint8_t> nonrt;
  for (size_t i = 0; i < io.wire.size(); i += 4) if ((io.wire[i] & 15) != 0x0F) nonrt.insert(nonrt.end(), &io.wire[i], &io.wire[i] + 4);
  CHECK(no_interleave(nonrt));
  CHECK(no_interleave(io.wire));
  Bytes all = unpack(io.wire.data(), io.wire.size());
  bool has_dump = false, has_sx = false;
  Bytes allx;
  for (uint8_t b : all) if (b != 0xF8) allx.push_back(b);
  for (size_t k = 0; k + dump.size() <= allx.size(); k++) if (memcmp(&allx[k], dump.data(), dump.size()) == 0) has_dump = true;
  for (size_t k = 0; k + maxsx.size() <= allx.size(); k++) if (memcmp(&allx[k], maxsx.data(), maxsx.size()) == 0) has_sx = true;
  CHECK(has_dump && has_sx);
  printf("realtime: %d Polls, %d of them held, every clock out in its own Poll\n", polls, held);
  delete t;
  delete m;
}

// R3-M1: a Rytm that is enumerated but not reading (every transfer fails after 3 s): each Poll makes one send at most -
// whatever is queued (keys, a lot of the computer's MIDI, the engine's, the bridge's) - including the retry Polls, so
// a Poll never stalls past the watchdog. What the computer had deferred is dropped and counted, not played late.
static void test_not_reading() {
  ToRytm* t = new ToRytm;
  UsbMidiMerge* m = new UsbMidiMerge;
  Pacer p;
  FakeIo io;
  io.fail_always = true;
  Bytes sx = sysex(3000, 1);
  uint32_t now = 30000000;
  auto load = [&]() {
    for (int k = 0; k < 200; k++) io.keys.push_back(pk(0x09, 0x90, 1, 2));
    for (int k = 0; k < 2000; k++) io.comp.push_back(pk(0x09, 0x90, 3, 4));
    for (int k = 0; k < 4; k++) io.engine.push_back(sysex(13, 1));
    t->feed(sx.data(), sx.size());
  };
  for (int round = 0; round < 3; round++) {		// the first failure, then two retries that fail too
    load();
    unsigned s0 = io.sends;
    m->poll(io, true, true, t, &p, now);
    CHECK(io.sends - s0 == 1 && m->failed());
    uint32_t until = now + (UsbMidiMerge::kRetryUs << round);
    for (uint32_t u = now + 1000; u < until; u += 250000) {	// off meanwhile: no send at all
      unsigned s1 = io.sends;
      m->poll(io, true, true, t, &p, u);
      CHECK(io.sends == s1);
    }
    now = until;
  }
  // the computer's failing: 2000 packets (eight 512-byte chunks) cost one send, and the engine's are not tried
  m->rytm_new();
  io.keys.clear();
  io.comp.clear();
  t->flush(false);
  for (int k = 0; k < 2000; k++) io.comp.push_back(pk(0x09, 0x90, 3, 4));
  io.engine = {sysex(13, 1)};
  unsigned sc = io.sends;
  m->poll(io, true, true, t, &p, now);
  CHECK(io.sends - sc == 1 && m->failed() && m->mac_failed() >= 1);
  // keys failing the same way: one send, the Poll ends there (the computer's never asked)
  m->rytm_new();
  io.comp = {pk(0x09, 0x90, 5, 6)};
  io.keys = {pk(0x09, 0x90, 1, 2)};
  unsigned s0 = io.sends;
  m->poll(io, true, true, t, &p, now);
  CHECK(io.sends - s0 == 1 && m->failed() && io.comp.size() == 1);
  // deferred packets at a failure: dropped and counted
  io.fail_always = false;
  m->rytm_new();
  io.comp.clear();
  t->flush(false);
  Bytes big = sysex(30000, 3);
  CHECK(t->feed(big.data(), big.size()) == big.size());
  m->poll(io, true, true, t, &p, now + 1000);		// the bridge's SysEx opens: a hold
  CHECK(t->open());
  for (int k = 0; k < 5; k++) io.comp.push_back(pk(0x09, 0x90, 7, 8));
  m->poll(io, true, true, t, &p, now + 2000);
  CHECK(m->deferred() == 5);
  unsigned d0 = m->deferred_dropped();
  io.fail_after = 0;
  m->poll(io, true, true, t, &p, now + 3000);
  CHECK(m->failed() && m->deferred() == 0 && m->deferred_dropped() == d0 + 5);
  // and at a new umidi1
  m->rytm_new();
  CHECK(t->feed(big.data(), big.size()) == big.size());
  m->poll(io, true, true, t, &p, now + 4000);
  for (int k = 0; k < 3; k++) io.comp.push_back(pk(0x09, 0x90, 7, 8));
  m->poll(io, true, true, t, &p, now + 5000);
  CHECK(m->deferred() == 3);
  m->rytm_new();
  CHECK(m->deferred() == 0 && m->deferred_dropped() == d0 + 8);
  delete t;
  delete m;
}

// R3-L1: during a hold, a Song Position Pointer (deferred) then Continue: Continue keeps its place behind it, while the
// clock still goes at once; with nothing deferred, Start goes at once
static void test_transport_order() {
  ToRytm* t = new ToRytm;
  UsbMidiMerge* m = new UsbMidiMerge;
  Pacer p;
  FakeIo io;
  Bytes big = sysex(20000, 2);
  CHECK(t->feed(big.data(), big.size()) == big.size());
  uint32_t now = 40000000;
  m->poll(io, true, true, t, &p, now);
  CHECK(t->open());
  io.comp = {pk(0x0F, 0xFA, 0, 0)};			// Start, nothing deferred: at once
  size_t w0 = io.wire.size();
  m->poll(io, true, true, t, &p, now += 1000);
  bool start = false;
  for (size_t i = w0; i < io.wire.size(); i += 4) start |= io.wire[i] == 0x0F && io.wire[i + 1] == 0xFA;
  CHECK(start && m->deferred() == 0);
  io.comp = {pk(0x03, 0xF2, 0x10, 0x00), pk(0x0F, 0xF8, 0, 0), pk(0x0F, 0xFB, 0, 0), pk(0x0F, 0xFE, 0, 0),
             pk(0x0F, 0xFC, 0, 0)};
  w0 = io.wire.size();
  m->poll(io, true, true, t, &p, now += 1000);
  bool f8 = false, fe = false, fb = false, fc = false;
  for (size_t i = w0; i < io.wire.size(); i += 4) {
    if (io.wire[i] != 0x0F) continue;
    f8 |= io.wire[i + 1] == 0xF8, fe |= io.wire[i + 1] == 0xFE, fb |= io.wire[i + 1] == 0xFB, fc |= io.wire[i + 1] == 0xFC;
  }
  CHECK(f8 && fe && !fb && !fc && m->deferred() == 3);	// SPP, Continue, Stop wait, in order
  for (int i = 0; i < 200 && t->pending(); i++) m->poll(io, true, true, t, &p, now += 1000);
  m->poll(io, true, true, t, &p, now += 1000);
  CHECK(m->deferred() == 0);
  // on the wire: the SPP before Continue before Stop
  long spp = -1, cont = -1, stop = -1;
  for (size_t i = w0; i < io.wire.size(); i += 4) {
    if (io.wire[i] == 0x03 && io.wire[i + 1] == 0xF2) spp = (long)i;
    if (io.wire[i] == 0x0F && io.wire[i + 1] == 0xFB) cont = (long)i;
    if (io.wire[i] == 0x0F && io.wire[i + 1] == 0xFC) stop = (long)i;
  }
  CHECK(spp >= 0 && spp < cont && cont < stop);
  CHECK(no_interleave(io.wire));
  delete t;
  delete m;
}

static void test_pieces() {
  // the CIN of every length's end, a SysEx piece a multiple of 3, a budget under 3 while open sends nothing
  for (unsigned n = 2; n < 40; n++) {
    ToRytm* t = new ToRytm;
    Bytes sx = sysex(n, 9);
    CHECK(t->feed(sx.data(), sx.size()) == n);
    std::vector<uint8_t> all;
    uint8_t pk[64];
    int zero = 0;
    while (t->pending()) {
      unsigned k = t->take(pk, sizeof pk, 7);		// 7 bytes a Poll: pieces of 6
      CHECK(k > 0 && k % 4 == 0);
      all.insert(all.end(), pk, pk + k);
      if (t->open()) {					// under 3 bytes: nothing, unless that is the whole rest
        unsigned k2 = t->take(pk, sizeof pk, 2);
        CHECK(k2 == 0 || (k2 == 4 && !t->open()));
        all.insert(all.end(), pk, pk + k2);
        zero += k2 == 0;
      }
    }
    CHECK(no_interleave(all));
    CHECK(unpack(all.data(), all.size()) == sx);
    uint8_t last = all[all.size() - 4] & 15;
    CHECK(last == 4 + ((n - 1) % 3 + 1));
    CHECK(n <= 9 || zero > 0);
    delete t;
  }
}

static void test_parser() {
  ToRytm* t = new ToRytm;
  // a status byte inside a SysEx: that SysEx dropped (junk), the status message kept; F9 / FD inside and outside
  Bytes bad = {0xF0, 0x01, 0xF9, 0x02, 0x90, 0x40, 0x7F, 0xF7, 0x05, 0xF4, 0xFD, 0xF0, 0x03, 0xFD, 0x04, 0xF7};
  CHECK(t->feed(bad.data(), bad.size()) == bad.size());
  CHECK(t->stats().msgs == 2 && t->stats().junk == 7);	// F9, the cut SysEx, the lone F7, 05, F4, FD, FD
  Bytes all = unpack(drain(*t).data(), 8);
  Bytes want = {0x90, 0x40, 0x7F, 0xF0, 0x03, 0x04};
  CHECK(Bytes(all.begin(), all.begin() + 6) == want);
  // a SysEx over kMaxMsg cut by a status byte: counted as too long, the status kept, the next SysEx fine
  Bytes big = sysex(kMaxMsg + 100, 1);
  big.back() = 0x90;
  big.push_back(0x41);
  big.push_back(0x42);
  Bytes small = sysex(20, 2);
  big.insert(big.end(), small.begin(), small.end());
  unsigned m0 = t->stats().msgs, tl = t->stats().too_long;
  CHECK(t->feed(big.data(), big.size()) == big.size());
  CHECK(t->stats().too_long == tl + 1 && t->stats().msgs == m0 + 2);
  all = unpack(drain(*t).data(), 4 + 28);
  CHECK(all[0] == 0x90 && all[1] == 0x41 && Bytes(all.begin() + 3, all.begin() + 23) == small);
  // an OS upgrade start split at every byte boundary: blocked every time, never a part of it through
  Bytes os = rpc(0x50, {0, 0, 0, 1, 's', 'y', 's', 'e', 'x', 0, 1});
  unsigned b0 = t->stats().blocked;
  for (size_t cut = 1; cut < os.size(); cut++) {
    CHECK(t->feed(os.data(), cut) == cut);
    CHECK(t->feed(os.data() + cut, os.size() - cut) == os.size() - cut);
  }
  CHECK(t->stats().blocked == b0 + os.size() - 1 && !t->pending());
  delete t;
}

static void test_backpressure() {
  ToRytm* t = new ToRytm;
  Bytes sx = sysex(30000, 5);
  unsigned fed = 0, full = 0;
  for (int i = 0; i < 20; i++) {
    unsigned k = t->feed(sx.data(), sx.size());
    fed += k;
    if (k < sx.size()) { full = k; break; }
  }
  CHECK(fed > 0 && t->stats().too_long == 0);		// the 256 KB queue is full: feed stopped, nothing dropped
  unsigned queued = t->stats().msgs;
  CHECK(queued == 8);
  drain(*t);
  CHECK(t->feed(sx.data() + full, sx.size() - full) == sx.size() - full);
  CHECK(t->stats().msgs == queued + 1);
  drain(*t);
  // a SysEx over kMaxMsg: dropped whole
  Bytes big = sysex(kMaxMsg + 10, 1);
  CHECK(t->feed(big.data(), big.size()) == big.size());
  CHECK(t->stats().too_long == 1 && t->stats().msgs == queued + 1);
  // the largest RPC message passes
  Bytes rpcmax = sysex(37457, 3);
  CHECK(t->feed(rpcmax.data(), rpcmax.size()) == rpcmax.size() && t->stats().msgs == queued + 2);
  drain(*t);
  // flush: a half-sent SysEx is finished, the rest dropped; flush(false) drops it too
  CHECK(t->feed(sx.data(), sx.size()) == sx.size());
  Bytes note = {0x90, 1, 2};
  CHECK(t->feed(note.data(), 3) == 3);
  uint8_t pk[4 * 400];
  t->take(pk, sizeof pk, 300);
  CHECK(t->open());
  t->flush();
  CHECK(t->stats().flushed == 1 && t->open());
  std::vector<uint8_t> all = drain(*t);
  CHECK(!t->open() && unpack(all.data(), all.size()).back() == 0xF7);
  CHECK(t->feed(sx.data(), sx.size()) == sx.size());
  t->take(pk, sizeof pk, 300);
  t->flush(false);
  CHECK(!t->pending() && !t->open());
  delete t;
}

static void test_os_block() {
  Bytes start = rpc(0x50, {0x00, 0x1C, 0xB0, 0x00, 's', 'y', 's', 'e', 'x', 0, 1});
  Bytes write = rpc(0x51, Bytes(40, 0x55));
  Bytes end = rpc(0x52, {0});
  Bytes ping = rpc(0x01), query = rpc(0x09, {'m', 'i', 'd', 'i', 0}), shot = rpc(0x04);
  Bytes hi = rpc(0xD0);			// a reply's id (the top bit in the packed group's head): refused all the same
  CHECK(classify(start.data(), start.size()) == kOsUpgrade && classify(write.data(), write.size()) == kOsUpgrade);
  CHECK(classify(end.data(), end.size()) == kOsUpgrade && classify(hi.data(), hi.size()) == kOsUpgrade);
  CHECK(classify(ping.data(), ping.size()) == kPlain && classify(query.data(), query.size()) == kPlain);
  CHECK(classify(shot.data(), shot.size()) == kPlain);
  // M5: the RPC decoded whatever the device byte
  for (uint8_t dev : {0x00, 0x0C, 0x08, 0x10, 0x7C}) {
    Bytes s = rpc_dev(dev, 0x50, {0, 0, 0, 1});
    CHECK(classify(s.data(), s.size()) == kOsUpgrade);
    Bytes c = rpc_dev(dev, 0x5C, {0, 0, 0, 1});
    CHECK(classify(c.data(), c.size()) == kDataWrite);
  }
  // the +Drive: writes refused, reads not (elkrpc.py's DATA_* and elektroid's sample file system numbering)
  for (uint8_t w : {0x11, 0x12, 0x20, 0x21, 0x40, 0x41, 0x42, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D}) {
    Bytes m = rpc(w, {0, 0, 0, 1, '/', 0});
    CHECK(classify(m.data(), m.size()) == kDataWrite);
  }
  for (uint8_t r : {0x01, 0x02, 0x04, 0x09, 0x10, 0x23, 0x30, 0x31, 0x32, 0x53, 0x54, 0x55, 0x56}) {
    Bytes m = rpc(r, {0, 0, 0, 1, '/', 0});
    CHECK(classify(m.data(), m.size()) == kPlain);
  }
  // Analog-Rytm_MKII_OS1.74.syx's first and second messages (the .syx image's own start and data), any device byte
  Bytes syx0 = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7F, 0x01, 0x07, 0x00, 0x01, 0x72, 0x00, 0x72, 0x68, 0xF7};
  Bytes syx1 = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7E, 0x00, 0x01, 0x72, 0x1D, 0x00, 0x16, 0x25, 0xF7};
  Bytes syxmk1 = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x7F, 0x02, 0x00, 0xF7};
  CHECK(is_os_upgrade(syx0.data(), syx0.size()) && is_os_upgrade(syx1.data(), syx1.size()));
  CHECK(is_os_upgrade(syxmk1.data(), syxmk1.size()));
  // not RPC: a Rytm kit dump / request whose 12th byte happens to be 0x50..0x5C, analoghealth's channel (7D), the mods'
  Bytes kit = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x52, 0x01, 0x01, 0x00, 0x00, 0x51, 0x05, 0xF7};
  Bytes kreq = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x62, 0x01, 0x01, 0x00, 0x00, 0x5C, 0x05, 0xF7};
  Bytes ah = rpc_dev(0x7D, 0x50, {0, 0, 0, 1});
  Bytes mod = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 0x01, 0x00, 0x00, 0xF7};
  CHECK(classify(kit.data(), kit.size()) == kPlain && classify(kreq.data(), kreq.size()) == kPlain);
  CHECK(classify(ah.data(), ah.size()) == kPlain && classify(mod.data(), mod.size()) == kPlain);

  ToRytm* t = new ToRytm;
  Bytes clr = rpc(0x5C, {0, 0, 0, 1, '/', 0});
  Bytes s;
  for (auto* m : {&start, &ping, &write, &syx0, &kit, &end, &clr, &syx1, &mod}) s.insert(s.end(), m->begin(), m->end());
  CHECK(t->feed(s.data(), s.size()) == s.size());
  CHECK(t->stats().blocked == 5 && t->stats().blocked_data == 1 && t->stats().msgs == 3);
  std::vector<uint8_t> all = drain(*t);
  Bytes want = ping;
  want.insert(want.end(), kit.begin(), kit.end());
  want.insert(want.end(), mod.begin(), mod.end());
  CHECK(unpack(all.data(), all.size()) == want);
  // the switches on: they pass
  t->allow(true, true);
  CHECK(t->feed(s.data(), s.size()) == s.size());
  CHECK(t->stats().blocked == 5 && t->stats().blocked_data == 1 && t->stats().msgs == 3 + 9);
  drain(*t);
  // L1: queued under a switch, the switch turned off before they went: dropped when they would start
  CHECK(t->feed(s.data(), s.size()) == s.size());
  t->allow(false, false);
  all = drain(*t);
  CHECK(unpack(all.data(), all.size()) == want);
  CHECK(t->stats().blocked == 10 && t->stats().blocked_data == 2);
  delete t;
}

// R2-M2: every RPC id through the classifier: the confirmed reads pass, 50-52 is the OS upgrade, everything else -
// elektroid's writes (sample FS, raw FS, RAM slots, data store) and every unknown id - is a write
static void test_rpc_table() {
  const uint8_t reads[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x10, 0x14, 0x22, 0x23, 0x26, 0x30, 0x31, 0x32, 0x33,
                           0x34, 0x35, 0x53, 0x54, 0x55, 0x56};
  const uint8_t writes[] = {0x11, 0x12, 0x15, 0x16, 0x18, 0x19, 0x20, 0x21, 0x24, 0x25, 0x40, 0x41, 0x42, 0x43, 0x44,
                            0x45, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D};
  for (uint8_t r : reads) { Bytes m = rpc(r, {0, 0, 0, 1}); CHECK(classify(m.data(), m.size()) == kPlain); }
  for (uint8_t w : writes) { Bytes m = rpc(w, {0, 0, 0, 1}); CHECK(classify(m.data(), m.size()) == kDataWrite); }
  unsigned plain = 0;
  for (unsigned c = 0; c < 0x80; c++) {
    Bytes m = rpc((uint8_t)c, {0, 0, 0, 1});
    Kind k = classify(m.data(), m.size());
    bool is_read = false;
    for (uint8_t r : reads) is_read |= r == c;
    CHECK(k == (c >= 0x50 && c <= 0x52 ? kOsUpgrade : is_read ? kPlain : kDataWrite));
    plain += k == kPlain;
    Bytes reply = rpc((uint8_t)(c | 0x80), {0});	// a reply's id: the same verdict
    CHECK(classify(reply.data(), reply.size()) == k);
  }
  CHECK(plain == sizeof reads);
}

static uint8_t g_rng = 1;
static void rng(uint8_t* p, unsigned n) { for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(g_rng = (uint8_t)(g_rng * 37 + 11)); }

static void test_hmac() {
  // RFC 4231 test case 2
  uint8_t mac[32];
  char hx[65];
  hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, mac);
  to_hex(mac, 32, hx);
  CHECK(strcmp(hx, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
  // a key over 64 bytes (RFC 4231 test case 6)
  uint8_t key[131];
  memset(key, 0xAA, sizeof key);
  const char* msg = "Test Using Larger Than Block-Size Key - Hash Key First";
  hmac_sha256(key, sizeof key, msg, strlen(msg), mac);
  to_hex(mac, 32, hx);
  CHECK(strcmp(hx, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0);
}

// the relay's answer (tools/rytm_bridge.py: Python's hmac) for a fixed token and nonce, as test_rytm_bridge.py computes it
static void test_answer_matches_python() {
  char line[kLineMax + 1];
  Gate::answer("0123456789abcdef0123456789abcdef", "00112233445566778899aabbccddeeff", line);
  CHECK(strcmp(line, "BRIDGE e2f18cb41fd0132236a07d04a640d4835d49aa14a1e3cadc7a6af79b13bd33bb") == 0);
}

static void test_hello_line() {
  // the hello and the client's first MIDI in one segment: the line, and the rest left for the caller
  const char seg[] = "BRIDGE abc\r\n\xF0\x7D\x01\xF7";
  HelloLine h;
  unsigned k = h.feed((const uint8_t*)seg, sizeof seg - 1);
  CHECK(h.done && !h.bad && strcmp(h.line, "BRIDGE abc") == 0 && k == 12 && sizeof seg - 1 - k == 4);
  // split over segments
  HelloLine h2;
  CHECK(h2.feed((const uint8_t*)"BRID", 4) == 4 && !h2.done);
  CHECK(h2.feed((const uint8_t*)"GE x\n\x90", 6) == 5 && h2.done && strcmp(h2.line, "BRIDGE x") == 0);
  // too long, or a NUL: bad
  HelloLine h3;
  Bytes longl(kLineMax + 5, 'a');
  h3.feed(longl.data(), longl.size());
  CHECK(h3.bad && !h3.done);
  HelloLine h4;
  h4.feed((const uint8_t*)"BR\0X\n", 5);
  CHECK(h4.bad);
  h.wipe();
  CHECK(h.line[0] == 0 && !h.done);
}

static void test_gate() {
  Gate g;
  const uint32_t A = 0x0A00000Au, B = 0x0A00000Bu;
  uint32_t now = 1000;
  CHECK(!g.on() && !g.os_allowed() && !g.data_allowed() && g.token()[0] == 0);	// off at boot, no token
  char nonce[kNonceHex + 1], line[kLineMax + 1];
  Gate::nonce(rng, nonce);
  CHECK(strlen(nonce) == kNonceHex);
  CHECK(g.hello("BRIDGE 00", nonce, A, now) == Gate::kOff);
  g.allow_os(true);
  CHECK(!g.os_allowed());				// not while off
  g.enable(rng);
  CHECK(g.on() && strlen(g.token()) == kTokenHex);
  char tok[kTokenHex + 1];
  strcpy(tok, g.token());
  Gate::answer(tok, nonce, line);
  CHECK(strlen(line) == 7 + kMacHex);
  CHECK(g.hello(line, nonce, A, now) == Gate::kOk);
  // M1: a sniffed answer is no use with the next connection's nonce
  char nonce2[kNonceHex + 1];
  Gate::nonce(rng, nonce2);
  CHECK(strcmp(nonce, nonce2) != 0);
  CHECK(g.hello(line, nonce2, A, now) == Gate::kBad);
  // the token itself as the answer, a prefix, upper case, extra bytes: wrong
  char bad[kLineMax + 1];
  snprintf(bad, sizeof bad, "BRIDGE %s", tok);
  CHECK(g.hello(bad, nonce, A, now) == Gate::kBad);
  Gate::answer(tok, nonce, bad);
  bad[7 + kMacHex - 1] = 0;
  CHECK(g.hello(bad, nonce, A, now) == Gate::kBad);
  Gate::answer(tok, nonce, bad);
  for (char* q = bad + 7; *q; q++) if (*q >= 'a' && *q <= 'f') *q = (char)(*q - 32);
  CHECK(g.hello(bad, nonce, A, now) == Gate::kBad);
  CHECK(g.hello(line, nonce, A, now) == Gate::kOk);		// a good one clears the count
  // not a bridge line (an HTTP request): refused, not counted
  for (int i = 0; i < 20; i++) CHECK(g.hello("POST / HTTP/1.1", nonce, A, now) == Gate::kNotBridge);
  CHECK(!g.locked(A, now));
  // M3: five wrong from A lock A out, not B, and the bridge stays on
  for (unsigned i = 0; i + 1 < kMaxFails; i++) CHECK(g.hello("BRIDGE x", nonce, A, now) == Gate::kBad);
  CHECK(g.hello("BRIDGE x", nonce, A, now) == Gate::kLocked);
  CHECK(g.on() && g.locked(A, now) && !g.locked(B, now) && g.locked_count(now) == 1);
  CHECK(g.hello(line, nonce, A, now) == Gate::kLocked);		// even the right answer, while locked
  CHECK(g.hello(line, nonce, B, now) == Gate::kOk);
  CHECK(g.locked(A, now + kLockMs - 1000) && !g.locked(A, now + kLockMs + 1));
  CHECK(g.hello(line, nonce, A, now + kLockMs + 1) == Gate::kOk);
  // many addresses: a lock is not pushed out by fresh ones
  for (unsigned i = 0; i < kMaxFails; i++) g.hello("BRIDGE x", nonce, A, now);
  for (uint32_t ip = 100; ip < 100 + 3 * kAddrs; ip++) g.hello("BRIDGE x", nonce, ip, now + 1);
  CHECK(g.locked(A, now + 2));
  // a new token at every enable; disable takes the switches and the token
  g.allow_os(true);
  g.allow_data(true);
  CHECK(g.os_allowed() && g.data_allowed());
  g.disable();
  CHECK(!g.on() && !g.os_allowed() && !g.data_allowed() && g.token()[0] == 0);
  CHECK(g.hello(line, nonce, B, now) == Gate::kOff);
  g.enable(rng);
  CHECK(strcmp(g.token(), tok) != 0 && g.hello(line, nonce, B, now) == Gate::kBad);
  // the pairing reset: off, the switches off, the lockouts forgotten
  g.allow_os(true);
  g.pairing_reset();
  CHECK(!g.on() && !g.os_allowed() && g.token()[0] == 0 && !g.locked(A, now + 2));
}

static void test_json() {
  Gate g;
  ToRytm* t = new ToRytm;
  Snapshot x = {"connected", "10.0.0.9", 3, 1, 2, 0, 4, 40, 0, 0, 1234};
  char out[1024];
  CHECK(status_json(out, sizeof out, g, *t, x, false) > 0);
  CHECK(strstr(out, "\"on\":false") && strstr(out, "\"state\":\"connected\"") && strstr(out, "\"fromRytm\":1234"));
  CHECK(!strstr(out, "10.0.0.9") && !strstr(out, "token") && !strstr(out, "client"));
  g.enable(rng);
  CHECK(status_json(out, sizeof out, g, *t, x, false) > 0);
  CHECK(!strstr(out, g.token()) && !strstr(out, "token") && !strstr(out, "10.0.0.9"));	// /status: never
  CHECK(status_json(out, sizeof out, g, *t, x, true) > 0);
  CHECK(strstr(out, g.token()) && strstr(out, "\"client\":\"10.0.0.9\""));		// GET /midibridge
  CHECK(out[0] == '{' && out[strlen(out) - 1] == '}');
  CHECK(status_json(out, 40, g, *t, x, true) == -1);	// does not fit: refused, not cut
  delete t;
}

// the Rytm's packets as Circle hands them over (the MIDI bytes of each event packet)
static void rytm_sysex(FromRytm<1024>& f, const Bytes& m, bool clock_inside = false) {
  for (size_t i = 0; i < m.size(); i += 3) {
    unsigned k = m.size() - i < 3 ? m.size() - i : 3;
    f.packet(&m[i], k);
    if (clock_inside && i == 3) { uint8_t c = 0xF8; f.packet(&c, 1); }
  }
}

static void test_from_rytm() {
  FromRytm<1024>& f = g_from;
  uint8_t note[3] = {0x90, 0x24, 0x64}, out[2048];
  f.packet(note, 3);					// no client: not copied
  CHECK(f.size() == 0 && f.drop_msgs() == 0);
  f.set_active(true);
  f.packet(note, 3);
  Bytes sx = sysex(20, 2);
  rytm_sysex(f, sx, true);
  // the clock inside the SysEx comes after it
  unsigned n = f.peek(out, sizeof out);
  Bytes got(out, out + n);
  Bytes want = {0x90, 0x24, 0x64};
  want.insert(want.end(), sx.begin(), sx.end());
  want.push_back(0xF8);
  CHECK(got == want);
  f.consume(n);
  CHECK(f.sent() == n && f.size() == 0);
  // a half-received SysEx is not visible to the client
  Bytes big = sysex(300, 4);
  for (size_t i = 0; i < 150; i += 3) f.packet(&big[i], 3);
  CHECK(f.size() == 0);
  for (size_t i = 150; i < big.size(); i += 3) f.packet(&big[i], big.size() - i < 3 ? big.size() - i : 3);
  CHECK(f.size() == 300);
  f.consume(300);
  // the client stops reading: whole messages are dropped and counted, never a part of one; the stream it gets is
  // still whole messages, in order
  unsigned sent = 0;
  for (int i = 0; i < 200; i++) {
    Bytes m = sysex(10 + (i % 50), (uint8_t)i);
    rytm_sysex(f, m);
    f.packet(note, 3);
    sent++;
  }
  CHECK(f.drop_msgs() > 0 && f.size() <= 1024);
  n = f.peek(out, sizeof out);
  // parse: every message whole (F0..F7 of the right length pattern, or 3-byte notes)
  unsigned msgs = 0;
  for (unsigned i = 0; i < n;) {
    if (out[i] == 0xF0) {
      unsigned j = i + 1;
      while (j < n && out[j] != 0xF7) { CHECK(!(out[j] & 0x80)); j++; }
      CHECK(j < n);
      i = j + 1;
    } else {
      CHECK(out[i] == 0x90 && i + 3 <= n);
      i += 3;
    }
    msgs++;
  }
  CHECK(msgs > 0 && msgs + f.drop_msgs() == 2 * sent);
  f.consume(n);
  // a SysEx larger than the whole queue: dropped, the stream after it fine
  unsigned d0 = f.drop_msgs();
  Bytes huge = sysex(3000, 1);
  rytm_sysex(f, huge);
  CHECK(f.drop_msgs() == d0 + 1 && f.size() == 0);
  f.packet(note, 3);
  CHECK(f.size() == 3);
  // a SysEx cut short by a status byte: dropped, the status message kept
  f.consume(3);
  uint8_t a[3] = {0xF0, 0x00, 0x20}, b[3] = {0xB0, 0x01, 0x02};
  f.packet(a, 3);
  f.packet(b, 3);
  n = f.peek(out, sizeof out);
  CHECK(n == 3 && out[0] == 0xB0);
  f.consume(n);
  // the client gone: a half-staged SysEx is not left over for the next one
  f.packet(a, 3);
  f.set_active(false);
  f.packet(note, 3);
  f.set_active(true);
  f.clear();
  uint8_t e[2] = {0x01, 0xF7};
  f.packet(e, 2);					// the old SysEx's end: a lone F7 now, nothing passed
  f.packet(note, 3);
  n = f.peek(out, sizeof out);
  CHECK(n == 3 && out[0] == 0x90);
  printf("from-rytm: %u messages dropped whole (%u bytes) while the client did not read\n", f.drop_msgs(), f.drop_bytes());
}


int main() {
  test_merge_poll();
  test_realtime_through_holds();
  test_not_reading();
  test_transport_order();
  test_pieces();
  test_parser();
  test_backpressure();
  test_os_block();
  test_rpc_table();
  test_hmac();
  test_answer_matches_python();
  test_hello_line();
  test_gate();
  test_json();
  test_from_rytm();
  printf("midibridge: PASS\n");
  return 0;
}
