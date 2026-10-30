// pi/engine/midibridge.h - the Rytm's USB MIDI over TCP, for hardware testing (docs/pi-engine.md "Rytm MIDI bridge",
// docs/HARDWARE-TESTING.md): the portable part. midibridge_pi.cpp is the Pi's side (the port, the tasks, the web page,
// /status, the console); usbmidi_merge.h is the Poll that merges it into the Sidecar's USB MIDI output;
// tests/test_midibridge.cpp runs all of it on the Mac. No Circle, no allocation, no printf. SHA-256: core/lib/sha256.h.
//
// WHAT IT CARRIES. A raw MIDI byte stream both ways, no framing, as analog-emu's --usb-midi-tcp does (Iced-Cream's
// analogrun / elkflash / elkbench take it as --port tcp:HOST:PORT, through tools/rytm_bridge.py, which does the
// handshake below): one client at a time, the newest authenticated one wins. Everything the Rytm sends is copied to
// the client (the Sidecar keeps getting all of it); the client's bytes go to the Rytm, merged into the Sidecar's own.
//
// THE MERGE (ToRytm, usbmidi_merge.h). The client's bytes are parsed into whole messages and queued only once complete:
// a SysEx waits for its F7. Each Poll sends the Sidecar's own messages first, then the bridge's, paced by a token bucket
// at kRate (250 KB/s, below the Rytm's ~300 KB/s, so a Poll does not sit in a blocking bulk transfer) and at most
// kPollBytes a Poll. While a bridge SysEx is half sent (open()) the Sidecar's own messages wait - USB MIDI has one
// cable - and a SysEx is at most kMaxMsg, so that wait is at most kMaxHoldMs. A full queue stops the reading (TCP's flow
// control): nothing from the client is dropped for lack of room.
//
// THE COPY (FromRytm). Staged in the USB MIDI interrupt and published to the client's task as whole messages only, so a
// client that stops reading loses whole messages (counted). Realtime bytes inside a SysEx are held until its F7.
//
// THE GATE. Off at every boot, and a new token at every enable. A client gets "SIDECAR-MIDIBRIDGE 1 <nonce>\n" (a fresh
// 128-bit nonce) and must answer "BRIDGE <HMAC-SHA256(token, nonce) in hex>\n" within kHelloMs, so a sniffed exchange
// cannot be replayed. Wrong answers are counted per address: kMaxFails lock that address out for kLockMs; nothing on
// the network can turn the bridge off. A line that is not a BRIDGE line (an HTTP request from a web page) is refused and
// not counted, so a page in the tester's own browser cannot lock the tester's address out.
//
// WHAT IS BLOCKED (classify()). By default: the OS upgrade - the MIDI RPC's OsUpgradeStart / Write / End (command
// 0x50..0x52 in the 8-to-7 packed message: elkflash, elektroid) and the OS .syx image's own messages (F0 00 20 3C <dev>
// 00 7E|7F, as Analog-Rytm_MKII_OS1.74.syx) - and every other RPC command but the confirmed reads (rpc_read(): an
// allowlist from elektroid's src/connectors/elektron.c, so an unknown id fails closed): the +Drive's writes. Each has
// its own switch, off at every boot and with the bridge. The RPC command is decoded whatever the device byte (m[4]),
// except the Rytm's own dumps and requests (device 07, id 0x50..0x6F at m[6]) and analoghealth's channel (device 7D),
// whose bytes are not RPC.
// (The stock OS drops device 7D, which suggests its RPC parser checks the device byte; decoding all others is defence
// in depth.) Anything else - kit and pattern dumps, the mods' own SysEx - passes.
#pragma once
#include <stdint.h>
#include <string.h>

#include "lib/sha256.h"

namespace midibridge {

constexpr uint16_t kPort = 7330;
constexpr unsigned kMaxMsg = 38912;		// one SysEx at most: the largest RPC message (32 KB decoded) framed, 37457
constexpr unsigned kRate = 250000;		// the bridge's bytes a second to the Rytm at most
constexpr unsigned kPollBytes = 512;		// a Poll at most (the token bucket's size)
constexpr unsigned kMaxHoldMs = (unsigned)((uint64_t)kMaxMsg * 1000 / kRate) + 1;	// the longest the Sidecar waits
static_assert(kMaxHoldMs <= 160, "a bridge SysEx would hold the Sidecar's own messages back too long");
constexpr unsigned kTokenHex = 32;
constexpr unsigned kNonceHex = 32;
constexpr unsigned kMacHex = 64;
constexpr unsigned kLineMax = 96;		// a hello line at most (without its end)
constexpr unsigned kHelloMs = 1000;		// the answer within this
constexpr unsigned kMaxFails = 5;		// wrong answers from one address before it is locked out
constexpr uint32_t kLockMs = 10u * 60u * 1000u;
constexpr unsigned kAddrs = 16;			// addresses tracked

// ---- what a whole SysEx is (see above). m: its first bytes (at least min(n, 12)), n: its length.
enum Kind { kPlain, kOsUpgrade, kDataWrite };
// The MIDI RPC commands that only read, as elektroid's src/connectors/elektron.c names them: ping 01, software version
// 02, device UID 03, screenshot 04, storage info 05, query 09; the sample file system's read dir 10, file info 22 23,
// reader 30 31 32; the raw file system's read dir 14, file info 26, reader 33 34 35; the data store's list 53 and
// read open / partial / close 54 55 56 (elkrpc.py's DATA_LIST / DATA_READ_*). Every other id - the writes (sample FS
// 11 12 20 21 40-42, raw FS 15 16 24 25 43-45, RAM slots 18 19, data store 57-5D) and anything unknown - is a write
// and needs the "+Drive writes" switch; 50-52 is the OS upgrade.
inline bool rpc_read(uint8_t cmd) {
  switch (cmd) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x09:
    case 0x10: case 0x22: case 0x23: case 0x30: case 0x31: case 0x32:
    case 0x14: case 0x26: case 0x33: case 0x34: case 0x35:
    case 0x53: case 0x54: case 0x55: case 0x56:
      return true;
    default:
      return false;
  }
}
inline Kind classify(const uint8_t* m, unsigned n) {
  if (n < 8 || m[0] != 0xF0 || m[1] != 0x00 || m[2] != 0x20 || m[3] != 0x3C || m[5] != 0x00) return kPlain;
  if (m[6] == 0x7E || m[6] == 0x7F) return kOsUpgrade;			// the .syx image's messages
  if (m[4] == 0x7D) return kPlain;					// analoghealth's channel
  if (m[4] == 0x07 && m[6] >= 0x50 && m[6] <= 0x6F) return kPlain;	// the Rytm's dumps and dump requests
  // 8-to-7: m[6] holds the first group's top bits, m[7..13] its bytes: seq hi, seq lo, resp hi, resp lo, command
  if (n < 13) return kPlain;
  uint8_t cmd = (uint8_t)((m[11] | ((m[6] & (0x40 >> 4)) ? 0x80 : 0)) & 0x7F);	// a reply's bit: refused all the same
  if (cmd >= 0x50 && cmd <= 0x52) return kOsUpgrade;
  return rpc_read(cmd) ? kPlain : kDataWrite;		// an allowlist: an unknown id fails closed
}
inline bool is_os_upgrade(const uint8_t* m, unsigned n) { return classify(m, n) == kOsUpgrade; }

// no early exit, no branch on the data (as paircode::ct_equal)
inline bool ct_equal(const void* a, const void* b, unsigned n) {
  const volatile uint8_t* x = (const volatile uint8_t*)a;
  const volatile uint8_t* y = (const volatile uint8_t*)b;
  uint8_t d = 0;
  for (unsigned i = 0; i < n; i++) d |= x[i] ^ y[i];
  return d == 0;
}

inline void to_hex(const uint8_t* d, unsigned n, char* out) {
  static const char hx[] = "0123456789abcdef";
  for (unsigned i = 0; i < n; i++) out[2 * i] = hx[d[i] >> 4], out[2 * i + 1] = hx[d[i] & 15];
  out[2 * n] = 0;
}

// HMAC-SHA256 (RFC 2104) of msg under key, as Python's hmac.new(key, msg, hashlib.sha256)
inline void hmac_sha256(const void* key, unsigned nkey, const void* msg, unsigned nmsg, uint8_t out[32]) {
  uint8_t k[64] = {0}, pad[64], inner[32];
  if (nkey > 64) core::lib::sha256(key, nkey, k);
  else memcpy(k, key, nkey);
  core::lib::Sha256 h;
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  h.init();
  h.update(pad, 64);
  h.update(msg, nmsg);
  h.final(inner);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
  h.init();
  h.update(pad, 64);
  h.update(inner, 32);
  h.final(out);
  memset(k, 0, sizeof k);
  memset(pad, 0, sizeof pad);
  memset(inner, 0, sizeof inner);
}

// ---- one hello line out of a connection's first bytes: feed() takes bytes up to and including the '\n' (a "\r\n"
// end too) and returns how many it took; the rest of that segment is the client's first MIDI, kept by the caller.
struct HelloLine {
  char line[kLineMax + 1] = {0};
  unsigned n = 0;
  bool done = false, bad = false;			// bad: over kLineMax, or a NUL in it
  unsigned feed(const uint8_t* p, unsigned len) {
    unsigned i = 0;
    while (i < len && !done && !bad) {
      uint8_t c = p[i++];
      if (c == '\n') {
        if (n > 0 && line[n - 1] == '\r') n--;
        line[n] = 0;
        done = true;
      } else if (c == 0 || n >= kLineMax) {
        bad = true;
      } else {
        line[n++] = (char)c;
      }
    }
    return i;
  }
  void wipe() { memset(line, 0, sizeof line); n = 0; done = bad = false; }
};

// ---- the gate: on/off, the switches, the token, the per-address lockout. Core 0 only (the web and console tasks,
// the listener task: Circle's cooperative tasks on one core).
class Gate {
 public:
  enum Hello { kOk, kOff, kNotBridge, kBad, kLocked };
  using Random = void (*)(uint8_t*, unsigned);
  void enable(Random random) {				// a new token at every enable
    if (on_) return;
    new_token(random);
    on_ = true;
  }
  void disable() {					// the switches go with it, and the token
    on_ = os_ = data_ = false;
    memset(token_, 0, sizeof token_);
  }
  void allow_os(bool b) { os_ = on_ && b; }
  void allow_data(bool b) { data_ = on_ && b; }
  void pairing_reset() {				// every device unpaired: off, and the lockouts forgotten
    disable();
    memset(addr_, 0, sizeof addr_);
  }
  bool on() const { return on_; }
  bool os_allowed() const { return os_; }
  bool data_allowed() const { return data_; }
  const char* token() const { return token_; }		// THE WEB PAGE ONLY (paired, same-origin): never a log line, never /status
  unsigned refused() const { return refused_; }

  // A connection's nonce: kNonceHex hex digits + NUL into out
  static void nonce(Random random, char* out) {
    uint8_t r[kNonceHex / 2];
    random(r, sizeof r);
    to_hex(r, sizeof r, out);
    memset(r, 0, sizeof r);
  }
  // the answer to nonce: "BRIDGE <hex>" into out (kLineMax + 1): what the relay sends (and the tests)
  static void answer(const char* token, const char* nonce, char* out) {
    uint8_t mac[32];
    hmac_sha256(token, (unsigned)strlen(token), nonce, (unsigned)strlen(nonce), mac);
    memcpy(out, "BRIDGE ", 7);
    to_hex(mac, 32, out + 7);
    memset(mac, 0, sizeof mac);
  }
  // an address locked out now (it is refused before any hello)
  bool locked(uint32_t ip, uint32_t now_ms) const {
    const Addr* a = find(ip);
    return a != nullptr && is_locked(*a, now_ms);
  }
  unsigned locked_count(uint32_t now_ms) const {
    unsigned k = 0;
    for (const Addr& a : addr_) if (is_locked(a, now_ms)) k++;
    return k;
  }
  // One answer line (without its end) to the nonce this connection was given, from address ip
  Hello hello(const char* line, const char* nonce, uint32_t ip, uint32_t now_ms) {
    if (!on_) { refused_++; return kOff; }
    if (locked(ip, now_ms)) { refused_++; return kLocked; }
    if (strncmp(line, "BRIDGE ", 7) != 0) { refused_++; return kNotBridge; }	// not counted: not a guess
    char want[kLineMax + 1];
    answer(token_, nonce, want);
    unsigned n = 0;
    while (n <= 7 + kMacHex && line[n]) n++;
    bool ok = n == 7 + kMacHex && ct_equal(line, want, 7 + kMacHex);
    memset(want, 0, sizeof want);
    Addr* a = slot(ip, now_ms);
    if (ok) {
      a->fails = 0;
      return kOk;
    }
    refused_++;
    if (a->fails >= kMaxFails) a->fails = 0;		// an old lock that ran out: counting again
    if (++a->fails >= kMaxFails) {
      a->until = now_ms + kLockMs;
      return kLocked;
    }
    return kBad;
  }

 private:
  struct Addr {
    uint32_t ip, until, seen;
    unsigned fails;
  };
  const Addr* find(uint32_t ip) const {
    for (const Addr& a : addr_) if (a.ip == ip && ip != 0) return &a;
    return nullptr;
  }
  bool is_locked(const Addr& a, uint32_t now_ms) const {
    return a.ip != 0 && a.fails >= kMaxFails && (int32_t)(a.until - now_ms) > 0;
  }
  // this address's entry; a new one takes a free slot, else the least recently seen that is not locked, else the
  // least recently seen
  Addr* slot(uint32_t ip, uint32_t now_ms) {
    for (Addr& a : addr_) if (a.ip == ip) { a.seen = now_ms; return &a; }
    Addr* best = nullptr;
    for (Addr& a : addr_) if (a.ip == 0) { best = &a; break; }
    for (int pass = 0; pass < 2 && best == nullptr; pass++)
      for (Addr& a : addr_)
        if ((pass == 1 || !is_locked(a, now_ms)) && (best == nullptr || (int32_t)(a.seen - best->seen) < 0)) best = &a;
    *best = Addr{ip, 0, now_ms, 0};
    return best;
  }
  void new_token(Random random) {
    uint8_t r[kTokenHex / 2];
    random(r, sizeof r);
    to_hex(r, sizeof r, token_);
    memset(r, 0, sizeof r);
  }
  bool on_ = false, os_ = false, data_ = false;
  char token_[kTokenHex + 1] = {0};
  unsigned refused_ = 0;
  Addr addr_[kAddrs] = {};
};

// ---- the pace towards the Rytm: a token bucket of kPollBytes, kRate bytes a second (now: microseconds, wrapping)
class Pacer {
 public:
  unsigned avail(uint32_t now_us) {
    if (!started_) { started_ = true; last_ = now_us; tokens_ = kPollBytes; }
    uint32_t dt = now_us - last_;
    last_ = now_us;
    uint64_t add = (uint64_t)dt * kRate + frac_;
    tokens_ += (unsigned)(add / 1000000u > kPollBytes ? kPollBytes : add / 1000000u);
    frac_ = (unsigned)(add % 1000000u);
    if (tokens_ > kPollBytes) tokens_ = kPollBytes, frac_ = 0;
    return tokens_;
  }
  void spend(unsigned n) { tokens_ = n > tokens_ ? 0 : tokens_ - n; }

 private:
  bool started_ = false;
  uint32_t last_ = 0;
  unsigned tokens_ = 0, frac_ = 0;
};

// ---- the client's bytes to the Rytm: whole messages, then USB MIDI 1.0 event packets on cable 0 in pieces.
// feed() runs in the bridge's task, take() in core 0's loop (usbmidi_merge.h): Circle's tasks on one core that never
// preempt each other, so no lock (the interrupt never touches this).
class ToRytm {
 public:
  static constexpr unsigned kRing = 1u << 18;	// queued messages (4-byte length + bytes): 256 KB

  struct Stats {
    uint64_t in;		// client bytes taken
    uint64_t sent;		// MIDI bytes handed to the Rytm (as packets)
    unsigned msgs;		// whole messages queued
    unsigned blocked;		// OS upgrade messages refused (the switch was off, at queueing or at sending)
    unsigned blocked_data;	// +Drive writes refused (the same)
    unsigned too_long;		// SysEx over kMaxMsg, dropped
    unsigned junk;		// stray bytes: data with no status, a lone F7, undefined status, a SysEx cut by a status
    unsigned flushed;		// queued messages dropped: the bridge closed, or no Rytm to send them to
  };

  // the switches as they are now (the gate's): checked when a message is queued and again when it starts going out,
  // so a switch turned off drops what was queued under it
  void allow(bool os, bool data) { os_ = os, data_ = data; }

  // up to n bytes; returns how many were taken. Fewer than n: the queue is full, call again with the rest later.
  unsigned feed(const uint8_t* p, unsigned n) {
    unsigned i = 0;
    for (; i < n; i++) {
      uint8_t b = p[i];
      if (b >= 0xF8) {						// realtime: a message of its own, even inside a SysEx
        if (b == 0xF9 || b == 0xFD) { st_.junk++; continue; }	// undefined
        if (!room(1)) break;
        commit(&b, 1);
        continue;
      }
      if (insx_) {
        if (b == 0xF7) {
          if (over_) { st_.too_long++; insx_ = over_ = false; sxn_ = 0; continue; }
          if (!room(sxn_ + 1)) break;
          sx_[sxn_++] = 0xF7;
          if (!refused(sx_, sxn_)) commit(sx_, sxn_);
          insx_ = false, sxn_ = 0;
          continue;
        }
        if (!(b & 0x80)) {
          if (over_) continue;
          if (sxn_ + 1 >= kMaxMsg) { over_ = true; continue; }	// room for the F7 kept
          sx_[sxn_++] = b;
          continue;
        }
        if (over_) st_.too_long++;
        else st_.junk++;					// a status inside: that SysEx is lost
        insx_ = false, sxn_ = 0, over_ = false;
        // and the status itself is taken below
      }
      if (b == 0xF0) {
        insx_ = true, sxn_ = 0, over_ = false;
        sx_[sxn_++] = b;
        run_ = 0, have_ = 0;
        continue;
      }
      if (b & 0x80) {
        need_ = data_len(b);
        if (need_ < 0) { st_.junk++; run_ = 0; have_ = 0; continue; }	// F4, F5, a lone F7
        msg_[0] = b, have_ = 1;
        run_ = b < 0xF0 ? b : 0;				// running status: channel messages only
        if (need_ == 0) {					// F6
          if (!room(1)) { have_ = 0; break; }
          commit(msg_, 1);
          have_ = 0;
        }
        continue;
      }
      // a data byte
      if (have_ == 0) {
        if (run_ == 0) { st_.junk++; continue; }
        msg_[0] = run_, have_ = 1, need_ = data_len(run_);
      }
      if (have_ == need_ && !room(have_ + 1)) break;		// the last byte: only once it can be queued
      msg_[have_++] = b;
      if (have_ == need_ + 1) {
        commit(msg_, have_);
        have_ = 0;
      }
    }
    st_.in += i;
    return i;
  }

  // The next USB MIDI packets for the Rytm: at most cap bytes of packets (a multiple of 4) and budget MIDI bytes. A
  // SysEx that does not fit goes in pieces of whole packets (open() until its last one). Returns the packets' bytes.
  unsigned take(uint8_t* out, unsigned cap, unsigned budget) {
    unsigned o = 0;
    while (rd_ != wr_ && o + 4 <= cap && budget > 0) {
      unsigned len = head_len(), base = rd_ + 4;
      uint8_t s = at(base);
      if (s != 0xF0) {						// a short message: one packet, whole
        if (len > budget) break;
        uint8_t m[3] = {s, len > 1 ? at(base + 1) : (uint8_t)0, len > 2 ? at(base + 2) : (uint8_t)0};
        out[o] = cin(m[0], len), out[o + 1] = m[0], out[o + 2] = m[1], out[o + 3] = m[2];
        o += 4, budget -= len;
        st_.sent += len;
        pop_head();
        continue;
      }
      if (off_ == 0) {						// a SysEx about to start: the switches again
        uint8_t h[16];
        for (unsigned k = 0; k < 16 && k < len; k++) h[k] = at(base + k);
        if (refused(h, len)) { pop_head(); continue; }
      }
      unsigned left = len - off_, k = left;
      unsigned fit = (cap - o) / 4 * 3;
      if (k > budget) k = budget;
      if (k > fit) k = fit;
      if (k < left) k -= k % 3;					// a piece: whole CIN 4 packets
      if (k == 0) break;
      for (unsigned j = 0; j < k; j += 3) {
        unsigned g = k - j < 3 ? k - j : 3;
        bool end = off_ + j + g == len;
        out[o] = (uint8_t)(end ? 4 + g : 4);
        for (unsigned q = 0; q < 3; q++) out[o + 1 + q] = q < g ? at(base + off_ + j + q) : 0;
        o += 4;
      }
      off_ += k, budget -= k;
      st_.sent += k;
      if (off_ == len) pop_head();
      else break;						// the rest next time (open())
    }
    return o;
  }

  bool open() const { return off_ != 0; }			// a SysEx is half sent: nothing else may go to the Rytm
  bool pending() const { return rd_ != wr_; }
  unsigned queued_bytes() const { return wr_ - rd_; }
  // Drops every queued message but a half-sent one (that one is finished, so the Rytm's parser is not left inside
  // a SysEx) and whatever was being parsed. keep_open false: the half-sent one too (no Rytm, or a failed send).
  void flush(bool keep_open = true) {
    unsigned end = rd_;
    if (keep_open && off_ != 0) end = rd_ + 4 + head_len();
    else off_ = 0;
    for (unsigned i = end; i != wr_; i += 4 + len_at(i)) st_.flushed++;
    wr_ = end;
    insx_ = false, sxn_ = 0, over_ = false, have_ = 0, run_ = 0;
  }
  const Stats& stats() const { return st_; }

 private:
  bool refused(const uint8_t* m, unsigned n) {
    Kind k = classify(m, n);
    if (k == kOsUpgrade && !os_) { st_.blocked++; return true; }
    if (k == kDataWrite && !data_) { st_.blocked_data++; return true; }
    return false;
  }
  static int data_len(uint8_t s) {
    if (s < 0xF0) return (s & 0xE0) == 0xC0 ? 1 : 2;		// Cn, Dn: 1; the rest: 2
    switch (s) {
      case 0xF1: case 0xF3: return 1;
      case 0xF2: return 2;
      case 0xF6: return 0;
      default: return -1;
    }
  }
  static uint8_t cin(uint8_t s, unsigned len) {
    if (s < 0xF0) return (uint8_t)(s >> 4);
    if (s >= 0xF8) return 0x0F;
    return (uint8_t)(len == 1 ? 0x05 : len == 2 ? 0x02 : 0x03);	// F6 / F1, F3 / F2
  }
  bool room(unsigned len) const { return kRing - (wr_ - rd_) >= 4 + len; }
  uint8_t at(unsigned i) const { return ring_[i & (kRing - 1)]; }
  unsigned len_at(unsigned i) const {
    return (unsigned)at(i) | (unsigned)at(i + 1) << 8 | (unsigned)at(i + 2) << 16 | (unsigned)at(i + 3) << 24;
  }
  unsigned head_len() const { return len_at(rd_); }
  void pop_head() {
    rd_ += 4 + head_len();
    off_ = 0;
  }
  void commit(const uint8_t* m, unsigned len) {
    for (unsigned k = 0; k < 4; k++) ring_[(wr_ + k) & (kRing - 1)] = (uint8_t)(len >> (8 * k));
    for (unsigned k = 0; k < len; k++) ring_[(wr_ + 4 + k) & (kRing - 1)] = m[k];
    wr_ += 4 + len;
    st_.msgs++;
  }
  uint8_t ring_[kRing];
  unsigned rd_ = 0, wr_ = 0, off_ = 0;
  uint8_t sx_[kMaxMsg];					// the SysEx being received
  bool insx_ = false;
  unsigned sxn_ = 0;
  bool over_ = false, os_ = false, data_ = false;
  uint8_t msg_[3] = {0}, run_ = 0;
  int need_ = 0;
  int have_ = 0;
  Stats st_ = {};
};


// ---- the Rytm's bytes to the client: the USB MIDI interrupt pushes (packet()), the bridge's task pops (peek /
// consume). Single producer, single consumer, acquire/release on the published index (as spsc.h).
template <unsigned N>
class FromRytm {
  static_assert((N & (N - 1)) == 0, "N must be a power of 2");

 public:
  // The producer, while a client is connected (set_active by the task; packets before that are not copied)
  void set_active(bool b) { __atomic_store_n(&active_, b, __ATOMIC_RELEASE); }
  bool active() const { return __atomic_load_n(&active_, __ATOMIC_ACQUIRE); }

  // one USB MIDI event packet's MIDI bytes (1..3, Circle's MIDIPacketHandler)
  void packet(const uint8_t* m, unsigned n) {
    if (!active()) {
      if (sx_) abort_staged(false);
      return;
    }
    for (unsigned i = 0; i < n; i++) {
      uint8_t b = m[i];
      if (b >= 0xF8) {
        if (sx_) {						// held until the SysEx ends
          if (npend_ < sizeof pend_) pend_[npend_++] = b;
          else drop_msgs_++, drop_bytes_++;
        } else {
          put(b);
          publish();
        }
        continue;
      }
      if (b == 0xF0) {
        if (sx_) abort_staged(true);
        sx_ = true;
        put(b);
        continue;
      }
      if (b == 0xF7) {
        if (!sx_) { drop_bytes_++; continue; }		// a lone F7
        put(b);
        sx_ = false;
        status_ = false;
        publish();
        continue;
      }
      if (b & 0x80) {
        if (sx_) abort_staged(true);				// a SysEx cut short by the Rytm: not passed on
        put(b);
        status_ = true;
        continue;
      }
      if (!sx_ && !status_) { drop_bytes_++; continue; }	// data with no status (after a cut SysEx)
      put(b);
    }
    if (!sx_) publish();					// channel and system common messages: one packet each
  }

  // The consumer: up to max published bytes (not removed until consume)
  unsigned peek(uint8_t* out, unsigned max) const {
    unsigned r = __atomic_load_n(&rd_, __ATOMIC_RELAXED), w = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE);
    unsigned n = w - r < max ? w - r : max;
    for (unsigned k = 0; k < n; k++) out[k] = buf_[(r + k) & (N - 1)];
    return n;
  }
  void consume(unsigned n) {
    unsigned r = __atomic_load_n(&rd_, __ATOMIC_RELAXED);
    __atomic_store_n(&rd_, r + n, __ATOMIC_RELEASE);
    sent_ += n;
  }
  void clear() {							// the consumer, at a new client: no backlog
    __atomic_store_n(&rd_, __atomic_load_n(&wr_, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  }
  unsigned size() const {
    return __atomic_load_n(&wr_, __ATOMIC_ACQUIRE) - __atomic_load_n(&rd_, __ATOMIC_ACQUIRE);
  }
  uint64_t sent() const { return sent_; }			// bytes the client was given (consumed)
  unsigned drop_msgs() const { return __atomic_load_n(&drop_msgs_, __ATOMIC_RELAXED); }
  unsigned drop_bytes() const { return __atomic_load_n(&drop_bytes_, __ATOMIC_RELAXED); }

 private:
  void put(uint8_t b) {
    if (over_) return;
    unsigned r = __atomic_load_n(&rd_, __ATOMIC_ACQUIRE);
    if (stage_ - r >= N) { over_ = true; return; }
    buf_[stage_ & (N - 1)] = b;
    stage_++;
  }
  // the staged message to the client, or dropped whole when it did not fit
  void publish() {
    unsigned w = __atomic_load_n(&wr_, __ATOMIC_RELAXED);
    if (stage_ != w || over_) {
      if (over_) {
        drop_msgs_++;
        drop_bytes_ += stage_ - w;
        stage_ = w;
        over_ = false;
      } else {
        __atomic_store_n(&wr_, stage_, __ATOMIC_RELEASE);
      }
    }
    if (npend_ && !sx_) {						// the realtime bytes held during a SysEx
      unsigned np = npend_;
      npend_ = 0;
      for (unsigned k = 0; k < np; k++) {
        put(pend_[k]);
        publish();
      }
    }
  }
  void abort_staged(bool count) {
    unsigned w = __atomic_load_n(&wr_, __ATOMIC_RELAXED);
    if (count) drop_msgs_++, drop_bytes_ += stage_ - w;
    stage_ = w;
    over_ = false;
    sx_ = false;
    status_ = false;
  }
  uint8_t buf_[N];
  alignas(64) unsigned wr_ = 0;		// published (the consumer reads it)
  unsigned stage_ = 0;			// the producer's: staged up to here
  bool sx_ = false, over_ = false, status_ = false;
  uint8_t pend_[16];
  unsigned npend_ = 0;
  unsigned drop_msgs_ = 0, drop_bytes_ = 0;
  bool active_ = false;
  alignas(64) unsigned rd_ = 0;
  uint64_t sent_ = 0;
};


// ---- the state as JSON: /status's (no address, no token: /status is open to the network) and GET /midibridge's
// (paired, same-origin: the client's address, and the token while the bridge is on). No printf on the Pi.
struct Snapshot {
  const char* state;		// "off", "waiting", "connected"
  const char* client;		// the client's address ("" none)
  unsigned connects, replaced, refused, locked, drops, drop_bytes, rytm_fail, queued;
  uint64_t from_rytm;
};
class Json {
 public:
  Json(char* out, unsigned cap) : p_(out), cap_(cap) { if (cap_) p_[0] = 0; }
  void s(const char* t) { while (*t) c(*t++); }
  void u(uint64_t v) {
    char b[24];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) c(b[--n]);
  }
  void kv(const char* k, uint64_t v) { s(",\""); s(k); s("\":"); u(v); }
  void kb(const char* k, bool v) { s(",\""); s(k); s(v ? "\":true" : "\":false"); }
  void ks(const char* k, const char* v) { s(",\""); s(k); s("\":\""); s(v); s("\""); }	// v: no quotes in it
  int done() { if (over_ || n_ >= cap_) return -1; p_[n_] = 0; return (int)n_; }

 private:
  void c(char ch) { if (n_ + 1 < cap_) p_[n_++] = ch; else over_ = true; }
  char* p_;
  unsigned cap_, n_ = 0;
  bool over_ = false;
};
// with_private: the client's address and the token (GET /midibridge only)
inline int status_json(char* out, unsigned cap, const Gate& g, const ToRytm& t, const Snapshot& x, bool with_private) {
  Json j(out, cap);
  const ToRytm::Stats& st = t.stats();
  j.s("{\"on\":");
  j.s(g.on() ? "true" : "false");
  j.kb("osUpgrade", g.os_allowed());
  j.kb("dataWrites", g.data_allowed());
  j.ks("state", x.state);
  if (with_private) j.ks("client", x.client);
  j.kv("port", kPort);
  j.kv("connects", x.connects);
  j.kv("replaced", x.replaced);
  j.kv("refused", x.refused);
  j.kv("lockedAddrs", x.locked);
  j.kv("toRytm", st.sent);
  j.kv("toRytmMsgs", st.msgs);
  j.kv("fromRytm", x.from_rytm);
  j.kv("drops", x.drops);
  j.kv("dropBytes", x.drop_bytes);
  j.kv("blocked", st.blocked);
  j.kv("blockedData", st.blocked_data);
  j.kv("tooLong", st.too_long);
  j.kv("junk", st.junk);
  j.kv("flushed", st.flushed);
  j.kv("queued", x.queued);
  j.kv("rytmFail", x.rytm_fail);
  j.kv("rateBps", kRate);
  if (with_private && g.on()) j.ks("token", g.token());
  j.s("}");
  return j.done();
}

}  // namespace midibridge
