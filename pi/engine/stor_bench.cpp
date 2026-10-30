// pi/engine/stor_bench.cpp - see stor_bench.h (portable: no libc beyond the language)
#include "stor_bench.h"
#include "stor_out.h"
#include "stor_vol.h"

namespace storbench {

const uint32_t kPiece[kSizes] = {64u << 10, 256u << 10, 1u << 20};

using stor::Out;

namespace {

enum Phase : int { pWrite, pRead, pRand4, pRand64, pRawSeq, pRawRand, pBusyRead, pBusyWrite, pClean };
const char *const kPhase[] = {"sequential write", "sequential read", "random 4 KB reads", "random 64 KB reads",
			      "raw sequential reads", "raw 4 KB reads", "reads while busy", "reads while busy + writes",
			      "cleaning up"};

const Io *s_pIo = 0;
uint8_t *s_pW = 0, *s_pR = 0;
uint32_t s_Lat[kMaxN];

struct Run
{
	State state = kIdle;
	Phase ph = pWrite;
	int si = 0;			// the piece size's index (phases 1)
	int n = 0, want = 0;		// operations done / wanted in this phase
	uint64_t pos = 0, raw_len = 0;
	uint64_t busy_us = 0;
	uint32_t t0 = 0, t_phase = 0, u0 = 0, rnd = 1, last = 0;
	uint64_t elapsed = 0;
	bool open[2] = {false, false}, s0 = false, wrote = false;	// wrote: the busy phase's last operation was a write
	char why[80] = "";
};
Run s_Run;
Results s_Res;
bool s_bHave = false;
Args s_Args;

uint32_t now (void) { return s_pIo->now_us (); }
uint32_t rnd (void)
{
	s_Run.rnd = s_Run.rnd * 1664525u + 1013904223u;
	return s_Run.rnd >> 3;
}
uint64_t rnd_block (uint64_t nBlocks)	// 0 .. nBlocks-1 (64-bit: a 4 KB index over a large device)
{
	if (nBlocks <= 1) return 0;
	uint64_t r = (uint64_t) rnd () << 29 ^ rnd ();
	return r % nBlocks;
}

bool same (const uint8_t *a, const uint8_t *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
	return true;
}

void close_all (void)
{
	for (int f = 0; f < 2; f++)
		if (s_Run.open[f]) s_pIo->close (f), s_Run.open[f] = false;
}
void clean (void)
{
	close_all ();
	s_pIo->remove (s_Res.vol, 0);
	s_pIo->remove (s_Res.vol, 1);
	stor::set_busy (s_Res.vol, false);
}
void copy (char *d, int cap, const char *t)
{
	int i = 0;
	for (; t && t[i] && i < cap - 1; i++) d[i] = t[i];
	d[i] = 0;
}
bool fail (const char *pWhy)
{
	clean ();
	s_Run.state = kFailed;
	char b[120];
	Out o (b, sizeof b);
	o.s (pWhy).s (" (").s (kPhase[s_Run.ph]).s (")");
	copy (s_Run.why, sizeof s_Run.why, b);
	if (s_pIo->log)
	{
		char l[140];
		Out m (l, sizeof l);
		m.s ("stor: bench ").s (stor::name (s_Res.vol)).s (" failed: ").s (b);
		s_pIo->log (l);
	}
	return false;
}

// one piece of n bytes from the write buffer: one call, or calls of <= 64 KB not crossing 64 KB (split)
bool put (int f, uint32_t n, uint32_t *pCalls)
{
	if (!s_Res.split)
	{
		++*pCalls;
		return s_pIo->write (f, s_pW, n) == (int) n;
	}
	for (uint32_t d = 0; d < n;)
	{
		uint32_t k = stor::piece ((uintptr_t) (s_pW + d), n - d);
		++*pCalls;
		if (s_pIo->write (f, s_pW + d, k) != (int) k) return false;
		d += k;
	}
	return true;
}
bool get (int f, uint64_t off, uint32_t n)
{
	if (!s_Res.split) return s_pIo->read_at (f, off, s_pR, n) == (int) n;
	for (uint32_t d = 0; d < n;)
	{
		uint32_t k = stor::piece ((uintptr_t) (s_pR + d), n - d);
		if (s_pIo->read_at (f, off + d, s_pR + d, k) != (int) k) return false;
		d += k;
	}
	return true;
}
// the pattern at file offset off: the last sequential pass wrote kPiece[si] bytes of the write buffer each time
void check (uint64_t off, uint32_t n, uint32_t nPeriod)
{
	if (!s_Res.verify_ok) return;
	uint32_t k = n < 64 ? n : 64;
	uint32_t a = (uint32_t) (off % nPeriod);
	if (!same (s_pR, s_pW + a, k) || !same (s_pR + n - k, s_pW + a + n - k, k)) s_Res.verify_ok = false, s_Res.verify_off = off;
}

void enter (Phase p)
{
	s_Run.ph = p, s_Run.n = 0, s_Run.pos = 0, s_Run.busy_us = 0, s_Run.t_phase = now ();
	copy (s_Run.why, sizeof s_Run.why, kPhase[p]);
}

void next_after_rand (void)
{
	if (s_pIo->raw_read && s_pIo->raw_size)
	{
		uint64_t sz = s_pIo->raw_size (s_Res.vol);
		s_Run.raw_len = (sz < s_Res.size ? sz : s_Res.size) & ~(uint64_t) 0xFFFF;
		if (s_Run.raw_len >= 65536)
		{
			s_Res.raw = true;
			enter (pRawSeq);
			return;
		}
	}
	enter (pBusyRead);
}

void busy_enter (Phase p)
{
	enter (p);
	s_pIo->loop_reset ();
	s_Run.u0 = s_pIo->underruns ();
	s_Run.s0 = s_pIo->streaming ();
}
void busy_leave (Busy &b)
{
	b.ran = true;
	b.lat = lat_of (s_Lat, s_Run.n, s_Run.busy_us);
	s_pIo->loop_read (&b.loop_max_us, &b.loop_avg_us);
	b.underruns = s_pIo->underruns () - s_Run.u0;
	b.streaming = s_Run.s0 && s_pIo->streaming ();
}

}	// namespace

bool parse (const char *a, Args *p, const char **pErr)
{
	Args r;
	const char *e = "stor=bench[:SD|USB|USB2|USB3|NVME][:MB][:n=N][!]";
	if (!a) a = "";
	int len = 0;
	while (a[len]) len++;
	if (len && a[len - 1] == '!') r.force = true, len--;
	int i = 0;
	if (i < len && a[i] != ':') { if (pErr) *pErr = e; return false; }
	while (i < len)
	{
		i++;	// the ':'
		int j = i;
		while (j < len && a[j] != ':') j++;
		if (j == i) { if (pErr) *pErr = e; return false; }
		if (a[i] >= '0' && a[i] <= '9')
		{
			uint32_t v = 0;
			int k = i;
			for (; k < j && a[k] >= '0' && a[k] <= '9' && v < 100000; k++) v = v * 10 + (a[k] - '0');
			if (k != j || v < kMinMB || v > kMaxMB) { if (pErr) *pErr = "MB: 8..8192"; return false; }
			r.mb = v;
		}
		else if (j - i > 2 && (a[i] == 'n' || a[i] == 'N') && a[i + 1] == '=')
		{
			int v = 0, k = i + 2;
			for (; k < j && a[k] >= '0' && a[k] <= '9' && v < 100000; k++) v = v * 10 + (a[k] - '0');
			if (k != j || v < kMinN || v > kMaxN) { if (pErr) *pErr = "n=: 20..4000 reads"; return false; }
			r.n = v;
		}
		else
		{
			char t[8];
			int k = 0;
			for (; k < j - i && k < 7; k++) t[k] = a[i + k];
			t[k] = 0;
			const char *end;
			int v = j - i < 7 ? stor::parse_vol (t, &end) : -1;
			if (v < 0 || *end) { if (pErr) *pErr = e; return false; }
			r.vol = v;
		}
		i = j;
	}
	*p = r;
	return true;
}

const char *refuse (const Args &a, const Cond &c)
{
	static char b[128];
	Out o (b, sizeof b);
	if (c.running) return "a bench is running (stor=cancel stops it)";
	if (c.sdtest) return "the SD card test is running (sdtest=cancel)";
	if (!c.mounted) { o.s (stor::name (a.vol)).s (" is not mounted (stor lists the volumes)"); return b; }
	if ((c.playing || c.recording) && !a.force)
	{
		o.s (c.recording ? "a recording runs" : "the Rytm plays").s (": stor=bench:").s (stor::name (a.vol))
		 .s ("! runs it anyway (audio may drop)");
		return b;
	}
	return 0;
}

uint32_t pct (const uint32_t *v, int n, int q)
{
	if (n <= 0) return 0;
	int k = (n * q + 99) / 100;		// ceil (n q / 100): the nearest rank
	if (k < 1) k = 1;
	if (k > n) k = n;
	return v[k - 1];
}

Lat lat_of (uint32_t *v, int n, uint64_t nBusyUs)
{
	static const int kGap[] = {701, 301, 132, 57, 23, 10, 4, 1};	// Ciura's shell sort gaps
	for (int g : kGap)
		for (int i = g; i < n; i++)
		{
			uint32_t t = v[i];
			int j = i;
			for (; j >= g && v[j - g] > t; j -= g) v[j] = v[j - g];
			v[j] = t;
		}
	Lat l;
	if (n <= 0) return l;
	uint64_t s = 0;
	for (int i = 0; i < n; i++) s += v[i];
	l.n = (uint32_t) n;
	l.mean = (uint32_t) (s / n);
	l.p50 = pct (v, n, 50), l.p99 = pct (v, n, 99), l.max = v[n - 1];
	l.iops = nBusyUs ? (uint32_t) ((uint64_t) n * 1000000 / nBusyUs) : 0;
	return l;
}

uint32_t mbps_x100 (uint64_t nBytes, uint64_t nUs)
{
	if (!nUs) return 0;
	return (uint32_t) (nBytes * 100 * 1000000 / 1048576 / nUs);	// (fits: 2^33 bytes x 10^8 < 2^64)
}

void set_io (const Io *pIo) { s_pIo = pIo; }
void set_buffers (uint8_t *pWrite, uint8_t *pRead)
{
	s_pW = pWrite, s_pR = pRead;
	uint32_t r = 0x2545F491u;
	for (uint32_t i = 0; s_pW && i < kBufBytes; i++) r = r * 1664525u + 1013904223u, s_pW[i] = (uint8_t) (r >> 24);
}

const char *start (const Args &a, bool bSplit, uint64_t nFree, const char *pLink, const char *pFs)
{
	static char b[96];
	if (s_Run.state == kRunning) return "a bench is running (stor=cancel stops it)";
	if (!s_pIo || !s_pW || !s_pR) return "no buffers (2 MB)";
	uint64_t size = (uint64_t) a.mb << 20;
	if (nFree && nFree < size + kSpare)
	{
		uint64_t fit = nFree > kSpare ? (nFree - kSpare) & ~(uint64_t) (kBufBytes - 1) : 0;
		if (fit < ((uint64_t) kMinMB << 20))
		{
			Out o (b, sizeof b);
			o.s ("only ").u (nFree >> 20).s (" MB free on ").s (stor::name (a.vol)).s (" (").u (kMinMB + (kSpare >> 20)).s (" MB needed)");
			return b;
		}
		size = fit;
	}
	size &= ~(uint64_t) (kBufBytes - 1);
	s_Args = a;
	s_Run = Run ();
	s_Res = Results ();
	s_Res.vol = a.vol, s_Res.split = bSplit, s_Res.size = size;
	copy (s_Res.link, sizeof s_Res.link, pLink);
	copy (s_Res.fs, sizeof s_Res.fs, pFs);
	for (int k = 0; k < kSizes; k++) s_Res.seq[k].piece = kPiece[k];
	s_Run.rnd = now () | 1;
	s_bHave = false;		// (the old result is gone: s_Res was reset above)
	s_pIo->remove (a.vol, 0);
	s_pIo->remove (a.vol, 1);
	if (!s_pIo->open (a.vol, 0, true)) return "cannot create the test file";
	s_Run.open[0] = true;
	s_Run.state = kRunning;
	s_Run.t0 = s_Run.last = now ();
	stor::set_busy (a.vol, true);
	enter (pWrite);
	return 0;
}

void cancel (void)
{
	if (s_Run.state != kRunning) return;
	clean ();
	s_Run.state = kCancelled;
	copy (s_Run.why, sizeof s_Run.why, "cancelled");
}

bool step (void)
{
	if (s_Run.state != kRunning) return false;
	Run &r = s_Run;
	const uint32_t t_step = now ();
	const int n = s_Args.n;
	auto budget = [&] { return now () - t_step < kStepUs && !(s_pIo->hold && s_pIo->hold ()); };
	r.elapsed += now () - r.last, r.last = now ();	// (u64: a long bench outlasts the u32 microsecond clock)
	switch (r.ph)
	{
	case pWrite:
		{
			Seq &q = s_Res.seq[r.si];
			do
			{
				uint32_t calls = 0, t = now ();
				if (!put (0, q.piece, &calls)) return fail ("a write failed (full?)");
				uint32_t dt = now () - t;
				q.calls = calls, q.wus += dt, q.wbytes += q.piece;
				if (dt > q.wmax) q.wmax = dt;
				r.pos += q.piece;
			}
			while (r.pos < s_Res.size && budget ());
			if (r.pos < s_Res.size) return true;
			uint32_t t = now ();
			bool ok = s_pIo->sync (0);
			s_pIo->close (0);
			r.open[0] = false;
			q.sync = now () - t;
			q.wus += q.sync;
			if (!ok) return fail ("the sync failed");
			if (!s_pIo->open (s_Res.vol, 0, false)) return fail ("cannot open the test file again");
			r.open[0] = true;
			enter (pRead);
			return true;
		}
	case pRead:
		{
			Seq &q = s_Res.seq[r.si];
			do
			{
				uint32_t t = now ();
				if (!get (0, r.pos, q.piece)) return fail ("a read failed");
				uint32_t dt = now () - t;
				q.rus += dt, q.rbytes += q.piece;
				if (dt > q.rmax) q.rmax = dt;
				check (r.pos, q.piece, q.piece);
				r.pos += q.piece;
			}
			while (r.pos < s_Res.size && budget ());
			if (r.pos < s_Res.size) return true;
			if (++r.si < kSizes)
			{
				s_pIo->close (0);
				r.open[0] = false;
				if (!s_pIo->open (s_Res.vol, 0, true)) return fail ("cannot create the test file again");
				r.open[0] = true;
				enter (pWrite);
			}
			else enter (pRand4);
			return true;
		}
	case pRand4:
	case pRand64:
	case pRawRand:
		{
			const bool raw = r.ph == pRawRand;
			const uint32_t c = r.ph == pRand64 ? 65536 : 4096;
			const uint64_t span = raw ? s_pIo->raw_size (s_Res.vol) : s_Res.size;
			do
			{
				uint64_t off = rnd_block (span / c - 1) * c;
				uint32_t t = now ();
				bool ok = raw ? s_pIo->raw_read (s_Res.vol, off, s_pR, c) == (int) c : get (0, off, c);
				uint32_t dt = now () - t;
				if (!ok) return fail (raw ? "a raw read failed" : "a random read failed");
				if (!raw) check (off, c, kPiece[kSizes - 1]);
				r.busy_us += dt;
				s_Lat[r.n++] = dt;
			}
			while (r.n < n && budget ());
			if (r.n < n) return true;
			Lat l = lat_of (s_Lat, r.n, r.busy_us);
			if (r.ph == pRand4) s_Res.rnd4 = l, enter (pRand64);
			else if (r.ph == pRand64) s_Res.rnd64 = l, next_after_rand ();
			else s_Res.raw4 = l, busy_enter (pBusyRead);
			return true;
		}
	case pRawSeq:
		{
			do
			{
				uint32_t t = now ();
				if (s_pIo->raw_read (s_Res.vol, r.pos, s_pR, 65536) != 65536) return fail ("a raw read failed");
				s_Res.raw_us += now () - t;
				s_Res.raw_bytes += 65536;
				r.pos += 65536;
			}
			while (r.pos < r.raw_len && budget ());
			if (r.pos < r.raw_len) return true;
			enter (pRawRand);
			return true;
		}
	case pBusyRead:
	case pBusyWrite:
		{
			Busy &b = s_Res.busy[r.ph == pBusyWrite];
			if (r.ph == pBusyWrite)	// the recorder's pace: a 64 KB piece whenever the writes fall behind it
			{
				uint64_t due = (uint64_t) (now () - r.t_phase) * kRecBytesPerSec / 1000000;
				if (b.wbytes < due && !r.wrote && b.wbytes < kBusyWriteMax)	// (alternating at worst: a drive slower than the pace still gets its reads)
				{
					r.wrote = true;
					uint32_t calls = 0, t = now ();
					if (!put (1, 65536, &calls)) return fail ("a write failed (full?)");
					b.wus += now () - t;
					b.wbytes += 65536;
					return true;		// one operation a pass
				}
			}
			r.wrote = false;
			uint64_t off = rnd_block (s_Res.size / 65536 - 1) * 65536;
			uint32_t t = now ();
			if (!get (0, off, 65536)) return fail ("a read failed");
			uint32_t dt = now () - t;
			check (off, 65536, kPiece[kSizes - 1]);
			r.busy_us += dt;
			s_Lat[r.n++] = dt;
			if (r.n < n) return true;
			busy_leave (b);
			if (r.ph == pBusyRead)
			{
				if (!s_pIo->open (s_Res.vol, 1, true)) return fail ("cannot create the second file");
				r.open[1] = true;
				busy_enter (pBusyWrite);
			}
			else enter (pClean);
			return true;
		}
	case pClean:
		{
			clean ();
			s_Res.total_ms = (uint32_t) (r.elapsed / 1000);
			r.state = kDone;
			s_bHave = true;
			copy (r.why, sizeof r.why, "done");
			if (s_pIo->log)
			{
				static char t[1600];
				table (s_Res, t, sizeof t);
				for (char *l = t; *l;)
				{
					char *e = l;
					while (*e && *e != '\n') e++;
					char c = *e;
					*e = 0;
					s_pIo->log (l);
					if (!c) break;
					l = e + 1;
				}
			}
			return false;
		}
	}
	return false;
}

State state (void) { return s_Run.state; }
int vol (void) { return s_Res.vol; }
bool have (void) { return s_bHave; }
const Results &results (void) { return s_Res; }
const char *phase (void) { return s_Run.why; }

int progress (void)
{
	const Run &r = s_Run;
	if (r.state == kDone) return 100;
	if (r.state != kRunning) return 0;
	uint64_t sz = s_Res.size ? s_Res.size : 1;
	int nn = s_Args.n ? s_Args.n : 1;
	switch (r.ph)
	{
	case pWrite: return r.si * 24 + (int) (12 * r.pos / sz);
	case pRead: return r.si * 24 + 12 + (int) (12 * r.pos / sz);
	case pRand4: return 72 + 5 * r.n / nn;
	case pRand64: return 77 + 5 * r.n / nn;
	case pRawSeq: return 82 + (int) (3 * r.pos / (r.raw_len ? r.raw_len : 1));
	case pRawRand: return 85 + 3 * r.n / nn;
	case pBusyRead: return 88 + 5 * r.n / nn;
	case pBusyWrite: return 93 + 6 * r.n / nn;
	default: return 99;
	}
}

namespace {
// microseconds as milliseconds with three decimals, right-aligned in w
void ms (Out &o, uint32_t us, int w)
{
	char b[24];
	Out t (b, sizeof b);
	t.fix (us, 3);
	o.pad (b, w);
}
void mbs (Out &o, uint32_t x100, int w)
{
	char b[24];
	Out t (b, sizeof b);
	t.fix (x100, 2);
	o.pad (b, w);
}
void num (Out &o, uint64_t v, int w, const char *pre = "")
{
	char b[24];
	Out t (b, sizeof b);
	t.s (pre).u (v);
	o.pad (b, w);
}
void cap (Out &o, const char *t)	// a row's caption: 8 columns
{
	int k = 0;
	for (; t[k]; k++) o.c (t[k]);
	for (; k < 8; k++) o.c (' ');
}
void lat_cols (Out &o, const Lat &l) { ms (o, l.p50, 8), ms (o, l.p99, 8), ms (o, l.max, 8); }
void lat_head (Out &o, const char *t) { cap (o, t), o.pad ("p50 ms", 8).pad ("p99 ms", 8).pad ("max ms", 8); }
}	// namespace

// widths: caption 8 | seq: write 11, read 10, worst 14, sync 9, calls 6 | QD1: 3 x 8, IOPS 7 | busy: 3 x 8, loop 16,
// underruns 6, OB 4, writes 8 - every line under 72 characters
int table (const Results &r, char *pOut, int nCap)
{
	Out o (pOut, nCap);
	o.s ("stor bench ").s (stor::name (r.vol)).s (": ").u (r.size >> 20).s (" MB file");
	if (r.fs[0]) o.s (", ").s (r.fs);
	if (r.link[0]) o.s (", ").s (r.link);
	o.s (r.split ? ", calls <= 64 KB" : ", one call a piece").c ('\n');
	cap (o, "seq"), o.pad ("write MB/s", 11).pad ("read MB/s", 10).pad ("worst w/r ms", 14).pad ("sync ms", 9).pad ("calls", 6).c ('\n');
	for (int k = 0; k < kSizes; k++)
	{
		const Seq &q = r.seq[k];
		cap (o, k == 0 ? "64 KB" : k == 1 ? "256 KB" : "1 MB");
		mbs (o, mbps_x100 (q.wbytes, q.wus), 11), mbs (o, mbps_x100 (q.rbytes, q.rus), 10);
		char b[32];
		Out t (b, sizeof b);
		t.fix (q.wmax / 100, 1).c ('/').fix (q.rmax / 100, 1);
		o.pad (b, 14);
		ms (o, q.sync, 9), num (o, q.calls, 6);
		o.c ('\n');
	}
	lat_head (o, "QD1"), o.pad ("IOPS", 7).c ('\n');
	cap (o, "rnd 4K"), lat_cols (o, r.rnd4), num (o, r.rnd4.iops, 7), o.c ('\n');
	cap (o, "rnd 64K"), lat_cols (o, r.rnd64), num (o, r.rnd64.iops, 7), o.c ('\n');
	if (r.raw)
	{
		cap (o, "raw 4K"), lat_cols (o, r.raw4), num (o, r.raw4.iops, 7), o.c ('\n');
		o.s ("raw seq 64K ");
		mbs (o, mbps_x100 (r.raw_bytes, r.raw_us), 0);
		o.s (" MB/s over ").u (r.raw_bytes >> 20).s (" MB (no FatFs)\n");
	}
	else o.s ("raw: not available here\n");
	lat_head (o, "busy 64K"), o.pad ("loop max/avg us", 16).pad ("under", 6).pad ("OB", 4).pad ("w MB/s", 8).c ('\n');
	for (int k = 0; k < 2; k++)
	{
		const Busy &b = r.busy[k];
		if (!b.ran) continue;
		cap (o, k ? "+writes" : "reads"), lat_cols (o, b.lat);
		char t[32];
		Out x (t, sizeof t);
		x.u (b.loop_max_us).c ('/').u (b.loop_avg_us);
		o.pad (t, 16);
		num (o, b.underruns, 6, "+");
		o.pad (b.streaming ? "on" : "off", 4);
		if (k) mbs (o, mbps_x100 (b.wbytes, b.wus), 8);
		else o.pad ("-", 8);
		o.c ('\n');
	}
	o.s ("verify ");
	if (r.verify_ok) o.s ("ok");
	else o.s ("MISMATCH at ").u (r.verify_off);
	o.s (", ").u (r.total_ms / 1000).s (" s\n");
	return o.len;
}

int json (char *pOut, int nCap)
{
	static const char *const kState[] = {"idle", "running", "done", "failed", "cancelled"};
	static char t[1600];
	Out o (pOut, nCap);
	const Results &r = s_Res;
	o.s ("\"bench\":{\"state\":\"").s (kState[s_Run.state]).s ("\",\"progress\":").i (progress ()).s (",\"phase\":\"")
	 .s (s_Run.why).s ("\",\"vol\":\"").s (stor::name (r.vol)).s ("\",\"have\":").b (s_bHave);
	if (s_bHave)
	{
		o.s (",\"sizeMB\":").u (r.size >> 20).s (",\"seq\":[");
		for (int k = 0; k < kSizes; k++)
		{
			const Seq &q = r.seq[k];
			if (k) o.c (',');
			o.s ("{\"pieceKB\":").u (q.piece >> 10).s (",\"writeMBps\":").fix (mbps_x100 (q.wbytes, q.wus), 2)
			 .s (",\"readMBps\":").fix (mbps_x100 (q.rbytes, q.rus), 2).s ("}");
		}
		o.s ("],\"rand4kP99Ms\":").fix (r.rnd4.p99, 3).s (",\"rand64kP99Ms\":").fix (r.rnd64.p99, 3)
		 .s (",\"busyLoopMaxUs\":").u (r.busy[1].ran ? r.busy[1].loop_max_us : r.busy[0].loop_max_us)
		 .s (",\"verify\":").b (r.verify_ok).s (",\"table\":\"");
		table (r, t, sizeof t);
		for (const char *c = t; *c; c++)
		{
			if (*c == '\n') o.s ("\\n");
			else if (*c == '"' || *c == '\\') o.c ('\\').c (*c);
			else o.c (*c);
		}
		o.c ('"');
	}
	o.c ('}');
	return o.len;
}

}	// namespace storbench
