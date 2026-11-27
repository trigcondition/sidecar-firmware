#!/usr/bin/env python3
"""Two-box worst-case CPU load test (design item 8): sets the load up from the Pi over POST /cmd and reads the CPU.

Both boxes may be on stock OS: every voice and setting comes from the Pi (pi/engine/cores.cpp slot=S:, docs/pi-engine.md).
Needs a kernel with the load test (sidecar-firmware b73c0c1 or later) and the pairing gate off (paircode=0).

  lt1_load.py --base http://HOST[:PORT] setup [--worst NAME] [--config A|B] [--enforce]   load on (loadtest=on first: nothing saved)
  lt1_load.py --base ... config B                                             AMP on the first box's voices 1-6 (INSERT A)
  lt1_load.py --base ... read --secs 30 --tag NAME                            stats lines for SECS, /status, mfx / sendfx
  lt1_load.py --base ... stop                                                 drones off, admit=enforce
  lt1_load.py --base ... baseline --secs 30 --tag NAME                        read only, no command sent

Writes only into --out (default ./lt1-out): commands.log (each command and the log bytes after it), status-*.json,
read-*.txt, summary-*.json. Exit 1 when a command is refused or unknown (the log says so), 2 on no answer.
"""
import argparse, json, os, re, sys, time, urllib.parse, urllib.request

DRONE_OK = ['plaits', 'juno', 'fm', 'daisy', 'dsps', 'wt', 'gr', 'md']   # the bench machines the note adapter plays as benched
BAD = re.compile(r'unknown command|no type|not taken|at most|refused|no such slot|not understood|no machine|is for the|'
                 r'command too long|queue full', re.I)
NOTBAD = re.compile(r'requests, \d+ refused|reuses \d+ refused \d+|no box refused|web: command :')   # (the web stats line, the amp readback's counters, admit=report's answer, the echo)
STATS = re.compile(r'engine: fifo min \d+ max \d+ target \d+ under (\d+).*?\| cpu c1 (\d+)\.(\d) c2 (\d+)\.(\d) c3 (\d+)\.(\d) %')
BENCH = re.compile(r'bench (\w+): (\d+)\.(\d+) % of a core per voice at (\d+) MHz')
ADMIT = re.compile(r'admit: at \d+ MHz a voice .*')
# stats with chains (945da9a: c2 only; 64fdd7b: c1 and c2) and core 3 without its render wait; the idle line's asleep %
CHAINS = re.compile(r'\((?:with chains c1 (\d+\.\d) c2 (\d+\.\d)|c2 with chains (\d+\.\d)), c3 own (\d+\.\d)(?:, c3 mfx (\d+\.\d))?\)')
ASLEEP = re.compile(r'idle: asleep (\d+) (\d+) (\d+) (\d+) %')
MUTE = re.compile(r'engine: muted|guard: voice \d.*(crash|over budget|muted)', re.I)


class Pi:
    def __init__(self, base, out):
        self.base, self.out, self.since = base.rstrip('/'), out, None
        os.makedirs(out, exist_ok=True)
        self.clog = open(os.path.join(out, 'commands.log'), 'a', encoding='utf-8')

    def get(self, path, timeout=10):
        with urllib.request.urlopen(self.base + path, timeout=timeout) as r:
            return r.read().decode('utf-8', 'replace')

    def status(self):
        return json.loads(self.get('/status'))

    def log(self, since=None):
        """New console bytes since the last call (GET /log?since=N: first line 'START END')."""
        s = self.since if since is None else since
        t = self.get('/log?since=%d' % (s or 0))
        head, _, body = t.partition('\n')
        try:
            start, end = (int(x) for x in head.split()[:2])
        except ValueError:
            return ''
        if self.since is None and since is None:
            body = ''   # the first call only finds the end
        self.since = end
        return body

    def cmd(self, c, wait=0.3, budget=120.0):
        """One command; a "command queue full" answer (the control core behind, console.cpp) is sent again after a pause."""
        t0, k = time.time(), 0
        while True:
            data = urllib.parse.urlencode({'c': c}).encode()
            req = urllib.request.Request(self.base + '/cmd', data=data, method='POST')
            with urllib.request.urlopen(req, timeout=10) as r:
                r.read()
            time.sleep(wait)
            got = self.log()
            self.clog.write('> %s%s\n%s\n' % (c, ' (again %d)' % k if k else '', got.rstrip()))
            self.clog.flush()
            if 'queue full' not in got or time.time() - t0 > budget:
                break
            k += 1
            time.sleep(min(3.0, 0.5 * k))   # (QEMU's control core drains slowly; a Pi's at once)
        bad = [l for l in got.splitlines() if BAD.search(l) and not NOTBAD.search(l)]
        return got, bad


def worst_machine(pi):
    """The costliest bench machine the drone plays as benched, from the console log (the boot's lines, if still held)."""
    rows = {}
    for m in BENCH.finditer(pi.log(since=0)):
        rows[m.group(1)] = int(m.group(2)) * 100 + int(m.group(3))
    pi.log()
    play = {k: v for k, v in rows.items() if k in DRONE_OK}
    if not play:
        return None, rows
    return max(play, key=play.get), rows


def setup_cmds(worst, slots_voices, enforce=False):
    # --enforce (LT4, rebalance-core2): admission stays on, so each INSERT type and AMP is admitted as it is set (or refused,
    # NO CPU: /status admit insRefused / ampRefused) and the voices are the admitted counts; else admit=report (all of it)
    c = ['loadtest=on', 'admit=enforce' if enforce else 'admit=report', 'amp=synthetic:on']
    for s, n in slots_voices:
        for v in range(n):
            c.append('slot=%d:notemach=%d:%s' % (s, v, worst))
        for v in range(1, n + 1):
            c += ['slot=%d:ins=V%d:A:type:sat' % (s, v), 'slot=%d:ins=V%d:B:type:sat' % (s, v)]
            c += ['slot=%d:ins=V%d:eq:%s:%d' % (s, v, k, x) for k, x in (('hpf', 40), ('lpf', 90), ('low', 90), ('high', 90))]
            c += ['slot=%d:ins=V%d:send:%d:80' % (s, v, b) for b in (1, 2, 3)]
        c += ['slot=%d:ins=B%d:A:type:cloud' % (s, b) for b in (1, 2, 3)]
        c += ['slot=%d:ins=B%d:B:type:sat' % (s, b) for b in (1, 2, 3)]
        c += ['slot=%d:mfx=1:type:cloud' % s, 'slot=%d:mfx=2:type:amp' % s, 'slot=%d:mfx=3:type:amp' % s,
              'slot=%d:mfx=4:type:sat' % s, 'slot=%d:mfx=eq:hpf:40' % s, 'slot=%d:mfx=eq:low:90' % s,
              'slot=%d:sendfx=1:cloudseed' % s, 'slot=%d:sendfx=2:cloudseed' % s,
              'slot=%d:sendlvl=1:all:100' % s, 'slot=%d:sendlvl=2:all:100' % s]
        c.append('slot=%d:drone=all' % s)
    return c


def light_cmds(worst, slots_voices, enforce=False):
    """--profile qemu: every kind of command once per slot, on one voice (QEMU can't carry the full load: its engine stalls)."""
    c = ['loadtest=on', 'admit=enforce' if enforce else 'admit=report', 'amp=synthetic:on']
    for s, n in slots_voices:
        c += ['slot=%d:notemach=0:%s' % (s, worst), 'slot=%d:ins=V1:A:type:sat' % s, 'slot=%d:ins=V1:B:type:sat' % s,
              'slot=%d:ins=V1:eq:hpf:40' % s, 'slot=%d:ins=V1:send:3:80' % s, 'slot=%d:ins=B3:A:type:cloud' % s,
              'slot=%d:ins=B3:B:type:sat' % s, 'slot=%d:mfx=4:type:sat' % s, 'slot=%d:sendfx=1:chorus' % s,
              'slot=%d:sendlvl=1:all:100' % s, 'slot=%d:drone=0' % s]
    return c


def readback_cmds(slots):
    c = ['amp']
    for s in slots:
        c += ['slot=%d:ins' % s, 'slot=%d:mfx' % s, 'slot=%d:sendfx' % s]
    return c


def held_slots(st):
    out = []
    for k, sl in enumerate(st.get('slots', [])):
        if sl.get('box') and not sl.get('presumed'):   # (kernel /status slots: slot box presumed ... voices asked short)
            out.append((int(sl.get('slot', k)), int(sl.get('asked') or sl.get('voices') or 0), sl))
    return out


def read(pi, secs, tag):
    pi.log()
    t0, lines = time.time(), []
    while time.time() - t0 < secs:
        time.sleep(2)
        lines += pi.log().splitlines()
    st = pi.status()
    json.dump(st, open(os.path.join(pi.out, 'status-%s.json' % tag), 'w'), indent=1)
    extra = []
    for c in ['mfx=stats', 'slot=1:mfx=stats', 'sendfx', 'slot=1:sendfx', 'amp']:
        got, _ = pi.cmd(c, wait=0.5)
        extra += got.splitlines()
    open(os.path.join(pi.out, 'read-%s.txt' % tag), 'w', encoding='utf-8').write('\n'.join(lines + ['---'] + extra) + '\n')
    cpu = [(int(m.group(1)), float(m.group(2) + '.' + m.group(3)), float(m.group(4) + '.' + m.group(5)),
            float(m.group(6) + '.' + m.group(7))) for m in map(STATS.search, lines) if m]
    mutes = [l for l in lines if MUTE.search(l)]
    ch = [m for m in map(CHAINS.search, lines) if m]
    sl = [[int(m.group(k)) for k in range(1, 5)] for m in map(ASLEEP.search, lines) if m]
    adm = st.get('admit', {})
    summ = {'tag': tag, 'secs': secs, 'samples': len(cpu),
            'c1': [c[1] for c in cpu], 'c2': [c[2] for c in cpu], 'c3': [c[3] for c in cpu],
            'under_last': cpu[-1][0] if cpu else None, 'under_first': cpu[0][0] if cpu else None,
            'admit': adm, 'slots': [{k: s.get(k) for k in ('slot', 'box', 'base', 'voices', 'asked', 'short')}
                                    for s in st.get('slots', [])],
            'c1_chains': [float(m.group(1)) for m in ch if m.group(1)],
            'c2_chains': [float(m.group(2) or m.group(3)) for m in ch],
            'c3_own': [float(m.group(4)) for m in ch],
            'c3_mfx': [float(m.group(5)) for m in ch if m.group(5)],
            'asleep': sl,
            'mutes': mutes[:20], 'uptime': st.get('uptime'), 'commit': st.get('commit') or st.get('version')}
    if cpu:
        for k in ('c1', 'c2', 'c3'):
            v = summ[k]
            summ[k + '_mean'] = round(sum(v) / len(v), 1)
            summ[k + '_max'] = max(v)
        for k in ('c1_chains', 'c2_chains', 'c3_own', 'c3_mfx'):
            v = summ[k]
            if v:
                summ[k + '_mean'] = round(sum(v) / len(v), 1)
                summ[k + '_max'] = max(v)
        summ['c1_estimate'] = adm.get('core1Bp', 0) / 100.0
        summ['c2_estimate'] = adm.get('core2Bp', 0) / 100.0
    json.dump(summ, open(os.path.join(pi.out, 'summary-%s.json' % tag), 'w'), indent=1)
    print(json.dumps({k: summ.get(k) for k in ('tag', 'samples', 'c1_mean', 'c1_max', 'c1_chains_mean', 'c1_chains_max',
                                               'c1_estimate', 'c2_mean', 'c2_max', 'c2_chains_mean', 'c2_chains_max',
                                               'c2_estimate', 'c3_mean', 'c3_own_mean', 'c3_mfx_mean', 'under_first', 'under_last', 'slots', 'mutes')},
                     indent=1))
    return summ


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--out', default='lt1-out')
    ap.add_argument('what', choices=['setup', 'config', 'read', 'stop', 'baseline', 'cmds'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--worst')
    ap.add_argument('--secs', type=int, default=30)
    ap.add_argument('--tag', default='read')
    ap.add_argument('--profile', choices=['unit', 'qemu'], default='unit')
    ap.add_argument('--enforce', action='store_true', help='setup with admission on (admit=enforce): types and AMPs admitted as set')
    a = ap.parse_args()
    pi = Pi(a.base, a.out)
    pi.log()
    if a.what in ('read', 'baseline'):
        read(pi, a.secs, a.tag)
        return 0
    if a.what == 'stop':
        for c in ['slot=0:drone=off', 'slot=1:drone=off', 'admit=enforce']:
            pi.cmd(c, wait=0.5)
        st = pi.status()
        json.dump(st, open(os.path.join(pi.out, 'status-stop.json'), 'w'), indent=1)
        print(json.dumps({'admit': st.get('admit'), 'slots': [{k: s.get(k) for k in ('slot', 'box', 'base', 'voices', 'asked', 'short')}
                                                              for s in st.get('slots', [])]}, indent=1))
        return 0
    if a.what == 'config':
        if a.arg != 'B':
            print('config: only B (AMP on the first box\'s voices 1-6, INSERT A)')
            return 2
        st = pi.status()
        hs = held_slots(st)
        base = [h for h in hs if h[2].get('base')]   # AMP renders on the base box's chains only (core/fx/amp.cpp ctl)
        first = base[0][0] if base else (hs[0][0] if hs else 0)
        bad = []
        nv = 1 if a.profile == 'qemu' else 6
        for c in ['slot=%d:ins=V%d:A:type:amp' % (first, v) for v in range(1, nv + 1)]:
            bad += pi.cmd(c)[1]
        for c in readback_cmds([h[0] for h in hs]):
            pi.cmd(c, wait=0.5)
        print('config B on slot %d; refused: %s' % (first, bad or 'none'))
        return 1 if bad else 0
    worst = a.worst
    if not worst:
        worst, rows = worst_machine(pi)
        print('bench rows in the log: %s; worst the drone plays: %s' % (rows, worst))
        if not worst:
            print('no bench rows left in /log: give --worst NAME')
            return 2
    st = pi.status()
    hs = held_slots(st)
    if len(hs) != 2:
        print('not two boxes held: %s' % [(h[0], h[1]) for h in hs])
        return 2
    cmds = (light_cmds if a.profile == 'qemu' else setup_cmds)(worst, [(h[0], min(8, h[1])) for h in hs], a.enforce)
    if a.what == 'cmds':
        print('\n'.join(cmds))
        return 0
    bad = []
    for c in cmds:
        got, b = pi.cmd(c)
        bad += ['%s -> %s' % (c, x) for x in b]
    for c in readback_cmds([h[0] for h in hs]):
        pi.cmd(c, wait=0.5)
    st = pi.status()
    json.dump(st, open(os.path.join(pi.out, 'status-setup.json'), 'w'), indent=1)
    print('setup: %d commands, worst machine %s, refused/unknown: %d' % (len(cmds), worst, len(bad)))
    for b in bad:
        print('  ' + b)
    print(json.dumps({'admit': st.get('admit'), 'slots': [{k: s.get(k) for k in ('slot', 'box', 'base', 'voices', 'asked', 'short')}
                                                          for s in st.get('slots', [])]}, indent=1))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
