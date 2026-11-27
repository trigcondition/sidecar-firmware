"""The Rytm side of the param-modulation contract (rytm-param-mod sections 1 and 2; docs/RYTM-PARAMS.md) as a model:
the reference the Rytm mod's emulator tests compare against, and the same arithmetic as the host tests' fake
(core/tests/pm_fake.h). Fed the host's whole messages (protocol.pm_offset / pm_override / pm_clear / pm_status), it keeps
per (track 0..12, word 0..63) an entry (offset, override, mix; each ramped per Rytm block of 32 frames) and gives the
effective value:

    eff = clamp(base + mix x (ovr - base) + rmod + off, 0, 0x7fff)      (8.8, 256 = one step)

base the smoother's state (the stored value or the Rytm's own p-lock), rmod the Rytm's own modulation this block; an
override on a word a Rytm scene locks is skipped (owner decision O3: the scene lock wins, offsets still add). OFFSET v
x 4 in 8.8, OVERRIDE v x 2, mix / 127. A message for another kit is dropped (counted); 250 ms without any param op: every
entry to 0 over 32 blocks (counted); 128 slots, the 129th refused (2D 01 10 op 2); a bad message refused whole
(2D 01 10 op 1); 07 answered 2D 01 11.

0.5.0 v2 P3, the Rytm-side morph (protocol.pm_morph_set / pm_fader; rytm-mods 0026 "pm morph"): 09 gives an entry a
part (a, b x 2 in 8.8; flags 1 has A, 2 has B; flags 0 removes it: its value stays, the mix to 0 over 8 blocks), 0A the
fader f / 16383 (ramped; kept across kits). Each block a part is the entry's override: both a + (b - a) f, mix 1; A only
a, mix 1 - f; B only b, mix f. A CLEAR or the timeout ends the part as it ends the entry; 07 has the parts in use last.
"""
from protocol import HOST_ACK, HOST_CMD, HOST_PM_FADER, HOST_PM_MORPH_SET, PM_STATUS, PROTO_VER

BLOCK_US = 32 * 1e6 / 48000
SLOTS = 128
TIMEOUT_US = 250000


def word_ok(t, w):
    if not 0 <= t <= 12 or w in (0, 4):
        return False
    if t == 12 and (w == 29 or 38 <= w <= 41):
        return False
    return 1 <= w <= 41 or 48 <= w <= 56


class _Ramp:
    __slots__ = ("cur", "to", "step", "n")

    def __init__(self):
        self.cur = self.to = self.step = 0.0
        self.n = 0

    def set(self, target, r):
        self.to = target
        if r <= 0:
            self.cur, self.n = target, 0
        else:
            self.step, self.n = (target - self.cur) / r, r

    def adv(self):
        if self.n > 0:
            self.n -= 1
            self.cur = self.to if self.n == 0 else self.cur + self.step


class _Ent:
    __slots__ = ("off", "ovr", "mix", "mfl", "ma", "mb")

    def __init__(self):
        self.off, self.ovr, self.mix = _Ramp(), _Ramp(), _Ramp()
        self.mfl, self.ma, self.mb = 0, 0.0, 0.0

    def end_part(self, ramp):
        self.mfl = 0
        self.ovr.n = 0
        self.mix.set(0, ramp)

    def done(self):
        return self.off.cur == 0 and self.mix.cur == 0 and not self.off.n and not self.mix.n and not self.mfl


class Rytm:
    def __init__(self, kit=0):
        self.kit = kit
        self.base = [[64 * 256.0] * 64 for _ in range(13)]
        self.rmod = [[0.0] * 64 for _ in range(13)]
        self.scene = [[False] * 64 for _ in range(13)]
        self.e = {}                      # (t, w) -> _Ent
        self.us = self.last_op_us = 0.0
        self.timing_out = False
        self.refused = self.kit_drops = self.timeouts = 0
        self.answers = []                # what the Rytm sends back: 2D 01 10 / 11 bodies (after F0 7D 52 54)
        self._acc = 0.0
        self.full_us = None
        self.fad = _Ramp()               # P3: the fader 0..1

    def _refuse(self, op, st):
        self.refused += 1
        self.answers.append(bytes([0x2D, PROTO_VER, HOST_ACK, op, st]))

    def _slot(self, t, w, op):
        x = self.e.get((t, w))
        if x:
            return x
        if len(self.e) >= SLOTS:
            if self.full_us is None or self.us - self.full_us >= 1e6:   # "full": at most once a second
                self.full_us = self.us
                self._refuse(op, 2)
            return None
        x = self.e[(t, w)] = _Ent()
        return x

    def take(self, m):
        """A whole host message; True when it was a param op (04..07)."""
        m = bytes(m)
        if len(m) < 8 or m[:4] != b"\xf0\x7d\x52\x54" or m[4] != HOST_CMD or m[5] != PROTO_VER or m[-1] != 0xF7:
            return False
        op, b = m[6], m[7:-1]
        if not 4 <= op <= 7 and op not in (HOST_PM_MORPH_SET, HOST_PM_FADER):
            return False
        if any(x > 0x7F for x in m[6:-1]):
            return self._refuse(op, 1) or True
        if op == 7:
            if b:
                return self._refuse(op, 1) or True
            kd = min(self.kit_drops, 16383)   # (the contract: the counters saturate; slots and timeouts at 127)
            parts = sum(1 for x in self.e.values() if x.mfl)
            self.answers.append(bytes([0x2D, PROTO_VER, PM_STATUS, min(len(self.e), 127), 0, 0, kd >> 7, kd & 0x7F,
                                       min(self.timeouts, 127), 2, min(parts, 127)]))
            return True
        if op == HOST_PM_FADER:
            if len(b) != 4:
                return self._refuse(op, 1) or True
            if b[0] != self.kit:
                self.kit_drops += 1
                return True
            self.last_op_us, self.timing_out = self.us, False
            self.fad.set((b[1] << 7 | b[2]) / 16383.0, b[3])
            return True
        if op == HOST_PM_MORPH_SET:
            n = b[1] if len(b) > 1 else 0
            if not 1 <= n <= 20 or len(b) != 2 + 7 * n:
                return self._refuse(op, 1) or True
            recs = [b[2 + 7 * i:9 + 7 * i] for i in range(n)]
            if any(not word_ok(r[0], r[1]) or 48 <= r[1] <= 55 or r[2] > 3 for r in recs):
                return self._refuse(op, 1) or True
            if b[0] != self.kit:
                self.kit_drops += 1
                return True
            self.last_op_us, self.timing_out = self.us, False
            for r in recs:
                if not r[2]:
                    x = self.e.get((r[0], r[1]))
                    if x and x.mfl:
                        x.end_part(8)
                    continue
                x = self._slot(r[0], r[1], op)
                if x:
                    x.mfl, x.ma, x.mb = r[2], (r[3] << 7 | r[4]) * 2.0, (r[5] << 7 | r[6]) * 2.0
            return True
        if len(b) < 2:
            return self._refuse(op, 1) or True
        if op == 6:
            if len(b) != 4:
                return self._refuse(op, 1) or True
            if b[0] != self.kit:
                self.kit_drops += 1
                return True
            self.last_op_us, self.timing_out = self.us, False
            mask = b[2] | b[3] << 7
            for (t, w), x in list(self.e.items()):
                if mask >> t & 1:
                    x.off.set(0, b[1])
                    x.end_part(b[1])
                    if not b[1]:
                        del self.e[(t, w)]
            return True
        n, rec = (b[2] if len(b) > 2 else 0), (4 if op == 4 else 5)
        if not 1 <= n <= (28 if op == 4 else 22) or len(b) != 3 + n * rec:
            return self._refuse(op, 1) or True
        recs = [b[3 + rec * i:3 + rec * (i + 1)] for i in range(n)]
        for r in recs:
            w = r[1] & 0x3F
            if not word_ok(r[0], w) or (op == 4 and r[1] & 0x40) or (op == 5 and 48 <= w <= 55):
                return self._refuse(op, 1) or True
        if b[0] != self.kit:
            self.kit_drops += 1
            return True
        self.last_op_us, self.timing_out = self.us, False
        ramp = b[1]
        for r in recs:
            t, w, v = r[0], r[1] & 0x3F, r[2] << 7 | r[3]
            x = self._slot(t, w, op)
            if not x:
                continue
            if op == 4:
                if v & 0x2000:
                    v -= 0x4000
                x.off.set(v * 4.0, ramp)
            else:
                jump = bool(r[1] & 0x40)
                if x.mix.cur == 0 and x.mix.n == 0:
                    x.ovr.cur = v * 2.0        # a fresh override starts at its value
                x.ovr.set(v * 2.0, 0 if jump else ramp)
                x.mix.set(r[4] / 127.0, 0 if jump else ramp)
        return True

    def block(self):
        if not self.timing_out and self.us - self.last_op_us >= TIMEOUT_US and self.e:
            self.timing_out = True
            self.timeouts += 1
            for x in self.e.values():
                x.off.set(0, 32)
                x.end_part(32)
        self.fad.adv()
        f = self.fad.cur
        for k, x in list(self.e.items()):
            if x.mfl:   # P3: the part is the override this block
                x.ovr.cur, x.mix.cur = ((x.ma + (x.mb - x.ma) * f, 1.0) if x.mfl == 3 else
                                        (x.ma, 1.0 - f) if x.mfl == 1 else (x.mb, f))
                x.ovr.n = x.mix.n = 0
            x.off.adv()
            x.ovr.adv()
            x.mix.adv()
            if x.done():
                del self.e[k]

    def free_all(self):
        """A kit change: every entry and part freed (the fader stays)."""
        self.e.clear()

    def advance(self, to_us):
        if to_us <= self.us:
            return
        self._acc += to_us - self.us
        self.us = to_us
        while self._acc >= BLOCK_US:
            self._acc -= BLOCK_US
            self.block()

    def eff(self, t, w):
        """(t, w)'s effective value in steps (a velocity word: its offset)."""
        x = self.e.get((t, w))
        b = self.base[t][w]
        v = b
        if x and not self.scene[t][w]:
            v = b + x.mix.cur * (x.ovr.cur - b)
        v += self.rmod[t][w] + (x.off.cur if x else 0)
        if w <= 41:
            v = min(max(v, 0), 0x7FFF)
        return v / 256.0
