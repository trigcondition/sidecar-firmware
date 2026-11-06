// pi/engine/tests/test_midi_filter.cpp - midi_filter.h: what passes, what is dropped, order kept.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "check.h"
#include "midi_filter.h"
#include "usbmidi_pack.h"

using Bytes = std::vector<uint8_t>;

static Bytes Pack(const Bytes& m) {     // one message as event packets on cable 0
  Bytes out;
  if (m[0] == 0xF0) {
    out.resize(usbmidi_sysex_bytes((int)m.size()));
    out.resize(usbmidi_pack_sysex(m.data(), (int)m.size(), out.data()));
    return out;
  }
  uint8_t cin = m[0] >= 0xF8 ? 0xF : m[0] >> 4;
  out = {cin, m[0], m.size() > 1 ? m[1] : (uint8_t)0, m.size() > 2 ? m[2] : (uint8_t)0};
  return out;
}

static Bytes Run(MidiFilter& f, const Bytes& packets) {
  Bytes out;
  for (size_t i = 0; i < packets.size(); i += 4)
    f.Feed(&packets[i], [&](const uint8_t* p) { out.insert(out.end(), p, p + 4); });
  return out;
}

int main() {
  const Bytes note = {0x90, 60, 100}, cc = {0xB3, 7, 99}, clock = {0xF8};
  const Bytes kit = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x52, 1, 2, 3, 4, 5, 6, 7, 0xF7};   // an Elektron kit dump
  const Bytes os = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7E, 0, 1, 2, 3, 0xF7};            // an OS .syx message
  const Bytes os_start = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7F, 0x01, 0xF7};
  const Bytes link = {0xF0, 0x7D, 0x52, 0x54, 0x04, 0x00, 48, 100, 0xF7};
  const Bytes ci = {0xF0, 0x7E, 0x7F, 0x0D, 0x70, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 0xF7};
  const Bytes tiny = {0xF0, 0xF7}, short3 = {0xF0, 0x01, 0xF7};
  const Bytes uni = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};   // identity request: universal, not CI
  // OS-update look-alikes on other device bytes: a test build's message (7D) and an RPC (10) whose payload starts 7E|7F
  const Bytes tb = {0xF0, 0x00, 0x20, 0x3C, 0x7D, 0x00, 0x7E, 1, 2, 3, 0xF7};
  const Bytes rpc = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00, 0x7F, 1, 2, 0xF7};

  // to the Rytm: the kit, notes, CCs, clock, tiny SysEx and non-CI universal pass; OS update, host link, CI do not
  {
    MidiFilter f(true);
    Bytes in, want;
    for (const Bytes* m : {&note, &kit, &os, &cc, &link, &ci, &clock, &os_start, &tiny, &short3, &uni, &tb, &rpc, &note}) {
      Bytes p = Pack(*m);
      in.insert(in.end(), p.begin(), p.end());
      if (m != &os && m != &link && m != &ci && m != &os_start && m != &tiny && m != &tb && m != &rpc)
        want.insert(want.end(), p.begin(), p.end());
    }
    Bytes out = Run(f, in);
    CHECK(out == want);
    CHECK(f.DroppedMessages() == 7);   // the look-alikes too, as before usbmidi=os: (a known limit)
    CHECK(!f.InSysEx());
  }
  // to the Rytm with SetPassOsUpdate(true) (usbmidi=os:on|emu): the OS update (device byte 0C) passes whole, the
  // look-alikes on other device bytes stay dropped, the rest as before
  {
    MidiFilter f(true);
    f.SetPassOsUpdate(true);
    Bytes in, want;
    for (const Bytes* m : {&note, &os_start, &os, &link, &os, &tb, &ci, &rpc, &os_start, &tiny, &cc}) {
      Bytes p = Pack(*m);
      in.insert(in.end(), p.begin(), p.end());
      if (m != &link && m != &ci && m != &tiny && m != &tb && m != &rpc) want.insert(want.end(), p.begin(), p.end());
    }
    Bytes out = Run(f, in);
    CHECK(out == want);
    CHECK(f.DroppedMessages() == 5);
    CHECK(!f.InSysEx());
    // the last one dropped, for the log: the empty SysEx's two bytes
    uint8_t last[MidiFilter::kHead];
    CHECK(f.LastDropped(last) == 2 && last[0] == 0xF0 && last[1] == 0xF7);
  }
  {  // the head kept is the dropped message's, at most kHead bytes; none before the first drop
    MidiFilter f(true);
    f.SetPassOsUpdate(true);
    uint8_t last[MidiFilter::kHead];
    CHECK(f.LastDropped(last) == 0);
    Run(f, Pack(kit));
    CHECK(f.LastDropped(last) == 0);
    Run(f, Pack(rpc));
    CHECK(f.LastDropped(last) == (int)rpc.size() && memcmp(last, rpc.data(), rpc.size()) == 0);
    Run(f, Pack(ci));
    CHECK(f.LastDropped(last) == MidiFilter::kHead && memcmp(last, ci.data(), MidiFilter::kHead) == 0);
  }
  // to the computer: the OS-update shape passes (the Rytm never sends one; only the Rytm-bound side blocks it)
  {
    MidiFilter f(false);
    Bytes in = Pack(os), l = Pack(link), c = Pack(ci), n = Pack(note);
    in.insert(in.end(), l.begin(), l.end());
    in.insert(in.end(), c.begin(), c.end());
    in.insert(in.end(), n.begin(), n.end());
    Bytes want = Pack(os);
    want.insert(want.end(), n.begin(), n.end());
    CHECK(Run(f, in) == want);
    CHECK(f.DroppedMessages() == 2);
  }
  // real-time bytes inside a held, a passing and a dropped SysEx go through at once; the SysEx stays whole
  {
    MidiFilter f(true);
    Bytes k = Pack(kit), c = Pack(clock), o = Pack(os), in, want;
    in.insert(in.end(), k.begin(), k.begin() + 4);      // kit packet 1 (held)
    in.insert(in.end(), c.begin(), c.end());            // clock: out first
    in.insert(in.end(), k.begin() + 4, k.end());        // the kit's rest: decides at packet 3
    in.insert(in.end(), o.begin(), o.begin() + 12);     // OS: 3 packets decide (dropped)
    in.insert(in.end(), c.begin(), c.end());
    in.insert(in.end(), o.begin() + 12, o.end());
    want = c;
    want.insert(want.end(), k.begin(), k.end());
    want.insert(want.end(), c.begin(), c.end());
    CHECK(Run(f, in) == want);
  }
  // a long SysEx that passes: every packet, in order
  {
    MidiFilter f(true);
    Bytes big = {0xF0, 0x00, 0x20, 0x3C};
    for (int i = 0; i < 3000; i++) big.push_back((uint8_t)(i & 0x7F));
    big.push_back(0xF7);
    Bytes p = Pack(big);
    CHECK(Run(f, p) == p);
  }
  // the pass toggled in the middle of a message: a message already passing goes on to its end (the decision is the
  // head's), so turning the pass off alone is not enough - Cut () drops the rest up to its end, and the next message is
  // decided afresh; a message still held when the pass goes off is decided with the new pass
  {
    Bytes big = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7E};
    for (int i = 0; i < 60; i++) big.push_back((uint8_t)i);
    big.push_back(0xF7);
    Bytes p = Pack(big), half(p.begin(), p.begin() + 40), rest(p.begin() + 40, p.end());
    {
      MidiFilter f(true);
      f.SetPassOsUpdate(true);
      Bytes out = Run(f, half);
      CHECK(out == half);
      f.SetPassOsUpdate(false);
      out = Run(f, rest);
      CHECK(out == rest);                // the pass alone does not stop it
    }
    {
      MidiFilter f(true);
      f.SetPassOsUpdate(true);
      CHECK(Run(f, half) == half);
      f.SetPassOsUpdate(false);
      f.Cut();
      CHECK(f.InSysEx());
      CHECK(Run(f, rest).empty());       // dropped up to its end
      CHECK(!f.InSysEx());
      CHECK(Run(f, p).empty());          // the next one: decided with the pass off
      Bytes n = Pack(note);
      CHECK(Run(f, n) == n);
    }
    {
      MidiFilter f(true);                // held (2 packets of the head) when the pass goes off: dropped whole
      f.SetPassOsUpdate(true);
      Bytes head(p.begin(), p.begin() + 8);
      CHECK(Run(f, head).empty());
      f.SetPassOsUpdate(false);
      CHECK(Run(f, Bytes(p.begin() + 8, p.end())).empty());
      CHECK(f.OsMessages() == 1);
    }
    {
      MidiFilter f(true);                // Cut while idle changes nothing
      f.Cut();
      Bytes k = Pack(kit);
      CHECK(Run(f, k) == k);
    }
  }
  // waiting for a start after an aborted upgrade (AwaitStart): the image's end (7F 02) and data stay dropped, its
  // start (7F 01, as the stock .syx files' first message) ends the wait; the RPC's 50 ends it too
  {
    const Bytes s0 = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7F, 0x01, 0x07, 0x00, 0x01, 0x72, 0x00, 0x72, 0x68, 0xF7};
    Bytes e0 = s0;
    e0[7] = 0x02;
    CHECK(os_upgrade::start(s0.data(), (int)s0.size()) && !os_upgrade::start(e0.data(), (int)e0.size()));
    CHECK(os_upgrade::pass(e0.data(), (int)e0.size()));     // an end passes when no wait is on
    MidiFilter f(true);
    f.SetPassOsUpdate(true);
    f.AwaitStart(true);
    CHECK(Run(f, Pack(e0)).empty() && Run(f, Pack(os)).empty() && f.AwaitingStart());
    CHECK(Run(f, Pack(s0)) == Pack(s0) && !f.AwaitingStart());
    CHECK(Run(f, Pack(os)) == Pack(os) && Run(f, Pack(e0)) == Pack(e0));
  }
  // usbmidi=os:off never drops less than dev (before usbmidi=os:) toward the Rytm: dev's drop rule, copied here, against
  // the filter with the pass off, over every device byte, the image commands 7E 7F and neighbours, h[5] 00 and 01,
  // messages cut short, and RPC frames on every command. Off differs from dev only where the shared rule adds the
  // bridge's RPC OsUpgradeStart / Write / End (os_upgrade::rpc): dropped now, passed by dev. With the pass on, only
  // os_upgrade::pass goes through. (The bridge's side of the same rule: test_midibridge test_os_rule.)
  {
    auto dev_drop = [](const uint8_t* h, int n) {   // origin/dev ccab290's MidiFilter::Drop for to_rytm = true
      bool host_link = n >= 4 && h[1] == 0x7D && h[2] == 0x52 && h[3] == 0x54;
      bool midi_ci = n >= 4 && (h[1] == 0x7E || h[1] == 0x7F) && h[3] == 0x0D;
      bool os_update = n >= 7 && h[1] == 0x00 && h[2] == 0x20 && h[3] == 0x3C && h[5] == 0x00 && (h[6] == 0x7E || h[6] == 0x7F);
      bool empty = n == 2 && h[1] == 0xF7;
      return host_link || midi_ci || os_update || empty;
    };
    const int kDevHead = 9;                         // dev's head
    std::vector<Bytes> cases = {os, os_start, tb, rpc, kit, link, ci, tiny, short3, uni};
    for (int dev = 0; dev < 0x80; dev++) {
      for (uint8_t cmd : {0x7E, 0x7F, 0x7D, 0x10, 0x52, 0x00})
        for (uint8_t h5 : {0x00, 0x01}) {
          cases.push_back({0xF0, 0x00, 0x20, 0x3C, (uint8_t)dev, h5, cmd, 0x01, 0x02, 0x03, 0xF7});
          cases.push_back({0xF0, 0x00, 0x20, 0x3C, (uint8_t)dev, h5, cmd, 0xF7});      // a head of 8 bytes
          cases.push_back({0xF0, 0x00, 0x20, 0x3C, (uint8_t)dev, h5, 0xF7});           // cut before the command
        }
      // an RPC frame (8-to-7: m[6] the top bits, m[7..11] seq hi lo, resp hi lo, command), every command
      for (int c = 0; c < 0x80; c++)
        for (uint8_t top : {0x00, 0x04})            // 0x04: the command's top bit (a reply): the same verdict
          cases.push_back({0xF0, 0x00, 0x20, 0x3C, (uint8_t)dev, 0x00, top, 0x00, 0x05, 0x00, 0x00, (uint8_t)c, 0x01, 0xF7});
    }
    unsigned n_dev = 0, n_more = 0, n_passed_on = 0;
    for (const Bytes& m : cases) {
      int n = (int)m.size();
      bool was = dev_drop(m.data(), n < kDevHead ? n : kDevHead);
      MidiFilter off(true), on(true);
      on.SetPassOsUpdate(true);
      Bytes p = Pack(m);
      bool off_drop = Run(off, p).empty(), on_drop = Run(on, p).empty();
      CHECK(!was || off_drop);                      // never less than dev
      CHECK(off_drop == (was || os_upgrade::rpc(m.data(), n)));   // more only by the bridge's RPC rule
      CHECK(off.DroppedMessages() == (off_drop ? 1u : 0u));
      CHECK(off.OsMessages() == (os_upgrade::any(m.data(), n) ? 1u : 0u));
      CHECK(on_drop == (off_drop && !os_upgrade::pass(m.data(), n)));
      n_dev += was;
      n_more += off_drop && !was;
      n_passed_on += off_drop && !on_drop;
    }
    CHECK(n_dev > 2 * 128);             // every device byte's 7E and 7F
    // RPC 50..52 (and the replies) on every device byte but analoghealth's 7D
    CHECK(n_more == 127 * 3 * 2);
    // os, os_start, device 0C's h5 00 with 7E or 7F in both lengths, and every RPC 50..52
    CHECK(n_passed_on == 6 + n_more);
  }
  printf("test_midi_filter: PASS\n");
  return 0;
}
