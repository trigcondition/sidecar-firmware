// core/tests/pm_fake.h - a fake Rytm for the host's param-modulation tests (core/mod/rytm_param.h): the Rytm side of
// the contract (notes/design rytm-param-mod.md section 1, 2) on a 13 x 64 word model, fed the bytes the host sends.
// The same arithmetic as sim/pm_model.py (the reference the mod's emulator tests compare against):
//   base  the smoother's state (the stored value or the Rytm's own p-lock), 8.8, 0..0x7fff
//   rmod  the Rytm's own modulation this block (its LFO, velocity routes, PERF): eff - base before S
//   eff   = clamp(base + mix x (ovr - base) + rmod + off, 0, 0x7fff); an override on a scene-locked word is skipped
// Ramps per block (32 frames at 48 kHz); OFFSET v x 4 in 8.8, OVERRIDE v x 2, mix /127; the 250 ms timeout, the kit
// check, CLEAR, the 128 slots, the refusals (2D 01 10 op 1 / 2), 07's answer. Time: advance(us) runs the blocks.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace pmfake {

struct Ent {
  bool used = false;
  double off = 0, off_to = 0, off_step = 0;
  int off_n = 0;
  double ovr = 0, ovr_to = 0, ovr_step = 0;
  int ovr_n = 0;
  double mix = 0, mix_to = 0, mix_step = 0;
  int mix_n = 0;
};

struct Rytm {
  int kit = 0;
  double base[13][64] = {};
  double rmod[13][64] = {};
  bool scene[13][64] = {};
  Ent e[13][64];
  uint64_t us = 0, last_op_us = 0;
  bool timing_out = false;
  int slots_full = 0, kit_drops = 0, timeouts = 0, msgs = 0, done = 0;
  std::vector<std::vector<uint8_t>> answers;   // what the Rytm sent back (2D 01 10 / 11)
  std::vector<std::vector<uint8_t>> rx;        // every param message taken (ops 04..07)
  static constexpr double kBlockUs = 32.0 * 1e6 / 48000.0;
  double block_acc = 0;

  Rytm() {
    for (auto& r : base)
      for (double& v : r) v = 64.0 * 256.0;   // every word at 64
  }
  static bool word_ok(int t, int w) {
    if (t < 0 || t > 12) return false;
    if (w == 0 || w == 4) return false;
    if (t == 12 && (w == 29 || (w >= 38 && w <= 41))) return false;
    if (w >= 1 && w <= 41) return true;
    return w >= 48 && w <= 56;
  }
  int used() const {
    int n = 0;
    for (auto& r : e)
      for (auto& x : r) n += x.used;
    return n;
  }
  void refuse(int op, int st) { answers.push_back({0x2D, 1, 0x10, (uint8_t)op, (uint8_t)st}); }
  Ent* slot(int t, int w, int op) {
    Ent& x = e[t][w];
    if (x.used) return &x;
    if (used() >= 128) {
      ++slots_full;
      refuse(op, 2);
      return nullptr;
    }
    x = Ent();
    x.used = true;
    return &x;
  }
  static void ramp(double& cur, double& to, double& step, int& n, double target, int r) {
    to = target;
    if (r <= 0) cur = target, n = 0;
    else step = (target - cur) / r, n = r;
  }
  // a whole host message F0 7D 52 54 5A 01 <op> ... F7; anything else is ignored (true: a param op)
  bool take(const uint8_t* m, int n) {
    if (n < 8 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 0x52 || m[3] != 0x54 || m[4] != 0x5A || m[5] != 1 || m[n - 1] != 0xF7)
      return false;
    int op = m[6];
    if (op < 4 || op > 7) return false;
    rx.emplace_back(m, m + n);
    for (int i = 6; i < n - 1; ++i)
      if (m[i] > 0x7F) return refuse(op, 1), true;
    const uint8_t* b = m + 7;
    int len = n - 8;
    if (op == 7) {
      if (len) return refuse(op, 1), true;
      int kd = kit_drops > 16383 ? 16383 : kit_drops;   // (the contract: the counters saturate)
      answers.push_back({0x2D, 1, 0x11, (uint8_t)(used() > 127 ? 127 : used()), 0, 0, (uint8_t)(kd >> 7), (uint8_t)(kd & 0x7F),
                         (uint8_t)(timeouts > 127 ? 127 : timeouts), 2});
      return true;
    }
    if (len < 2) return refuse(op, 1), true;
    if (op == 6) {
      if (len != 4) return refuse(op, 1), true;
      if (b[0] != kit) return ++kit_drops, true;
      last_op_us = us, timing_out = false;
      int mask = b[2] | b[3] << 7;
      for (int t = 0; t < 13; ++t)
        if (mask >> t & 1)
          for (Ent& x : e[t])
            if (x.used) {
              ramp(x.off, x.off_to, x.off_step, x.off_n, 0, b[1]);
              ramp(x.mix, x.mix_to, x.mix_step, x.mix_n, 0, b[1]);
              if (!b[1]) x = Ent();
            }
      ++done;
      return true;
    }
    int cnt = b[2], rec = op == 4 ? 4 : 5;
    if (cnt < 1 || cnt > (op == 4 ? 28 : 22) || len != 3 + cnt * rec) return refuse(op, 1), true;
    for (int i = 0; i < cnt; ++i) {   // validate all first (a bad record refuses the message)
      const uint8_t* r = b + 3 + rec * i;
      int w = r[1] & 0x3F;
      if (!word_ok(r[0], w) || (op == 4 && (r[1] & 0x40)) || (op == 5 && w >= 48 && w <= 55)) return refuse(op, 1), true;
    }
    if (b[0] != kit) return ++kit_drops, true;
    last_op_us = us, timing_out = false;
    int R = b[1];
    for (int i = 0; i < cnt; ++i) {
      const uint8_t* r = b + 3 + rec * i;
      int t = r[0], w = r[1] & 0x3F;
      int v = r[2] << 7 | r[3];
      Ent* x = slot(t, w, op);
      if (!x) continue;
      if (op == 4) {
        if (v & 0x2000) v -= 0x4000;
        ramp(x->off, x->off_to, x->off_step, x->off_n, v * 4.0, R);
      } else {
        bool jump = r[1] & 0x40;
        double mx = r[4] / 127.0;
        if (x->mix == 0 && x->mix_n == 0) x->ovr = v * 2.0;   // a fresh override starts at its value
        ramp(x->ovr, x->ovr_to, x->ovr_step, x->ovr_n, v * 2.0, jump ? 0 : R);
        ramp(x->mix, x->mix_to, x->mix_step, x->mix_n, mx, jump ? 0 : R);
      }
    }
    ++done;
    return true;
  }
  void block() {
    if (!timing_out && us - last_op_us >= 250000 && used()) {   // the timeout: everything to 0 over 32 blocks
      timing_out = true, ++timeouts;
      for (auto& r : e)
        for (Ent& x : r)
          if (x.used) ramp(x.off, x.off_to, x.off_step, x.off_n, 0, 32), ramp(x.mix, x.mix_to, x.mix_step, x.mix_n, 0, 32);
    }
    for (auto& r : e)
      for (Ent& x : r) {
        if (!x.used) continue;
        auto adv = [](double& c, double to, double st, int& n) {
          if (n > 0 && --n == 0) c = to;
          else if (n > 0) c += st;
        };
        adv(x.off, x.off_to, x.off_step, x.off_n);
        adv(x.ovr, x.ovr_to, x.ovr_step, x.ovr_n);
        adv(x.mix, x.mix_to, x.mix_step, x.mix_n);
        if (x.off == 0 && x.mix == 0 && !x.off_n && !x.mix_n) x = Ent();   // done: freed
      }
  }
  void advance(uint64_t to_us) {
    if (to_us <= us) return;
    block_acc += (double)(to_us - us);
    us = to_us;
    while (block_acc >= kBlockUs) block_acc -= kBlockUs, block();
  }
  // the effective value of (t, w) in steps (velocity words: the entry's offset in steps)
  double eff(int t, int w) const {
    const Ent& x = e[t][w];
    double b = base[t][w], v = b;
    if (x.used && !scene[t][w]) v = b + x.mix * (x.ovr - b);
    v += rmod[t][w] + (x.used ? x.off : 0);
    if (w <= 41) v = v < 0 ? 0 : v > 0x7fff ? 0x7fff : v;
    return v / 256.0;
  }
  double off(int t, int w) const { return e[t][w].used ? e[t][w].off / 256.0 : 0; }
  bool has(int t, int w) const { return e[t][w].used; }
};

}  // namespace pmfake
