// pi/engine/tests/test_stor_bench.cpp - stor_bench.cpp: the command's parser, the refusal rules, the percentiles and
// MB/s, the phases end to end on (1) a simulated drive with its own clock (exact figures in the table; every call
// <= 64 KB inside one 64 KB window when split; a corrupted read reported; a drive slower than the busy phase's pace
// still finishes) and (2) a temp folder through stdio (the files removed after, a cancel cleans up).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "stor_bench.h"
#include "stor_vol.h"

using namespace storbench;

namespace {
// ---- (1) the simulated drive: files in memory, time advanced by each call
struct Sim
{
	std::vector<uint8_t> file[2];
	uint64_t pos[2] = {0, 0};
	bool open[2] = {false, false};
	uint64_t clock = 1000;
	uint64_t write_bps = 50ull << 20, read_bps = 100ull << 20;	// MB/s as 2^20
	uint32_t call_us = 0, sync_us = 3000;
	uint32_t max_call = 0, calls = 0, crossings = 0;
	int corrupt_at = -1;		// the n-th read gets a flipped byte
	int reads = 0, removes = 0;
	bool streaming = true;
	uint32_t underruns = 5, loop_max = 0;
	std::string log;
} S;
void tick (uint64_t us) { S.clock += us; }
void note_call (const void *p, uint32_t n)
{
	S.calls++;
	if (n > S.max_call) S.max_call = n;
	uintptr_t a = (uintptr_t) p;
	if ((a >> 16) != ((a + n - 1) >> 16)) S.crossings++;
}
bool s_open (int, int f, bool w)
{
	if (w) S.file[f].clear ();
	S.pos[f] = w ? 0 : 0, S.open[f] = true;
	return true;
}
int s_write (int f, const void *p, uint32_t n)
{
	note_call (p, n);
	const uint8_t *b = (const uint8_t *) p;
	S.file[f].insert (S.file[f].end (), b, b + n);
	tick (S.call_us + (uint64_t) n * 1000000 / S.write_bps);
	S.loop_max = 1000 + (uint32_t) ((uint64_t) n * 1000000 / S.write_bps);	// the loop waits as long as the call
	return (int) n;
}
int s_read (int f, uint64_t off, void *p, uint32_t n)
{
	note_call (p, n);
	if (off + n > S.file[f].size ()) return -1;
	memcpy (p, &S.file[f][off], n);
	if (S.reads++ == S.corrupt_at) ((uint8_t *) p)[n - 1] ^= 1;
	tick (S.call_us + (uint64_t) n * 1000000 / S.read_bps);
	return (int) n;
}
bool s_sync (int) { tick (S.sync_us); return true; }
void s_close (int f) { S.open[f] = false; }
void s_remove (int, int f) { S.file[f].clear (); S.removes++; }
int s_raw (int, uint64_t, void *p, uint32_t n) { note_call (p, n); tick ((uint64_t) n * 1000000 / (200ull << 20)); return (int) n; }
uint64_t s_raw_size (int) { return 2ull << 30; }
uint32_t s_now (void) { return (uint32_t) S.clock; }
bool s_streaming (void) { return S.streaming; }
uint32_t s_under (void) { return S.underruns; }
void s_loop_reset (void) { S.loop_max = 0; }
void s_loop_read (uint32_t *m, uint32_t *a) { *m = S.loop_max, *a = 1010; }
void s_log (const char *l) { S.log += l; S.log += "\n"; }
const Io kSimIo = {s_open, s_write, s_read, s_sync, s_close, s_remove, s_raw, s_raw_size, s_now, s_streaming, s_under,
		   s_loop_reset, s_loop_read, s_log, 0};
const Io kSimNoRaw = {s_open, s_write, s_read, s_sync, s_close, s_remove, 0, 0, s_now, s_streaming, s_under,
		      s_loop_reset, s_loop_read, s_log, 0};

int run (int nMaxSteps = 10000000)
{
	int steps = 0;
	while (step ()) if (++steps > nMaxSteps) return -1;
	return steps;
}

// ---- (2) a temp folder through stdio
std::string g_Dir;
FILE *g_F[2];
std::string path (int f) { return g_Dir + (f ? "/bench2.bin" : "/bench.bin"); }
bool f_open (int, int f, bool w) { g_F[f] = fopen (path (f).c_str (), w ? "w+b" : "rb"); return g_F[f] != 0; }
int f_write (int f, const void *p, uint32_t n) { return fwrite (p, 1, n, g_F[f]) == n ? (int) n : -1; }
int f_read (int f, uint64_t off, void *p, uint32_t n)
{
	if (fseeko (g_F[f], (off_t) off, SEEK_SET)) return -1;
	return fread (p, 1, n, g_F[f]) == n ? (int) n : -1;
}
bool f_sync (int f) { return fflush (g_F[f]) == 0; }
void f_close (int f) { if (g_F[f]) fclose (g_F[f]); g_F[f] = 0; }
void f_remove (int, int f) { unlink (path (f).c_str ()); }
uint32_t f_now (void) { static uint32_t t; return t += 7; }
const Io kFileIo = {f_open, f_write, f_read, f_sync, f_close, f_remove, 0, 0, f_now, s_streaming, s_under,
		    s_loop_reset, s_loop_read, s_log, 0};
bool exists (int f) { struct stat st; return stat (path (f).c_str (), &st) == 0; }
}	// namespace

int main ()
{
	// ---- the command
	Args a;
	const char *err = 0;
	CHECK (parse ("", &a, &err) && a.vol == stor::kSD && a.mb == kDefaultMB && a.n == kDefaultN && !a.force);
	CHECK (parse (":USB", &a, &err) && a.vol == stor::kUSB && a.mb == 256);
	CHECK (parse (":nvme:1024:n=500!", &a, &err) && a.vol == stor::kNVME && a.mb == 1024 && a.n == 500 && a.force);
	CHECK (parse (":64:USB2", &a, &err) && a.vol == stor::kUSB2 && a.mb == 64);
	CHECK (parse ("!", &a, &err) && a.force && a.vol == stor::kSD);
	CHECK (!parse ("USB", &a, &err) && strstr (err, "stor=bench["));
	CHECK (!parse (":FOO", &a, &err) && !parse (":USB:", &a, &err) && !parse ("::", &a, &err));
	CHECK (!parse (":4", &a, &err) && strstr (err, "MB"));
	CHECK (!parse (":9000", &a, &err) && !parse (":64x", &a, &err));
	CHECK (!parse (":n=5", &a, &err) && strstr (err, "n=") && !parse (":n=", &a, &err) && !parse (":n=99999", &a, &err));

	// ---- the refusals
	Cond c;
	a = Args ();
	a.vol = stor::kUSB;
	CHECK (refuse (a, c) && strstr (refuse (a, c), "USB is not mounted"));
	c.mounted = true;
	CHECK (refuse (a, c) == 0);
	c.playing = true;
	CHECK (refuse (a, c) && strstr (refuse (a, c), "the Rytm plays: stor=bench:USB! runs it anyway"));
	c.playing = false, c.recording = true;
	CHECK (refuse (a, c) && strstr (refuse (a, c), "a recording runs"));
	a.force = true;
	CHECK (refuse (a, c) == 0);				// forced: may run
	c.running = true;
	CHECK (refuse (a, c) && strstr (refuse (a, c), "a bench is running"));	// never two, forced or not
	c.running = false, c.sdtest = true;
	CHECK (refuse (a, c) && strstr (refuse (a, c), "SD card test"));
	c.sdtest = false, c.mounted = false;
	CHECK (refuse (a, c) && strstr (refuse (a, c), "not mounted"));	// forced or not

	// ---- the math
	uint32_t v[100];
	for (int i = 0; i < 100; i++) v[i] = 100 - i;		// 100..1
	Lat l = lat_of (v, 100, 2000000);
	CHECK (v[0] == 1 && v[99] == 100);				// sorted
	CHECK (l.p50 == 50 && l.p99 == 99 && l.max == 100 && l.mean == 50 && l.iops == 50 && l.n == 100);
	uint32_t w[3] = {7, 3, 5};
	l = lat_of (w, 3, 0);
	CHECK (l.p50 == 5 && l.p99 == 7 && l.max == 7 && l.iops == 0);
	CHECK (pct (w, 1, 50) == 3 && pct (w, 0, 50) == 0);
	uint32_t one[1] = {42};
	l = lat_of (one, 1, 42);
	CHECK (l.p50 == 42 && l.p99 == 42 && l.max == 42 && l.iops == 23809);
	CHECK (mbps_x100 (1ull << 20, 1000000) == 100 && mbps_x100 (256ull << 20, 2560000) == 10000 && mbps_x100 (5, 0) == 0);
	CHECK (mbps_x100 (8ull << 30, 1) > 0);			// no overflow at 8 GB

	// ---- (1) the simulated drive, split (USB): exact figures
	uint8_t *wb = (uint8_t *) aligned_alloc (65536, kBufBytes), *rb = (uint8_t *) aligned_alloc (65536, kBufBytes);
	CHECK (wb && rb);
	set_io (&kSimIo);
	a = Args ();
	CHECK (start (a, true, 0, "x", "y") && strstr (start (a, true, 0, "", ""), "no buffers"));
	set_buffers (wb, rb);
	stor::reset ();
	a.vol = stor::kUSB, a.mb = 8, a.n = 100;
	CHECK (start (a, true, 1ull << 30, "USB 3 SuperSpeed", "exFAT") == 0);
	CHECK (state () == kRunning && stor::status (stor::kUSB).busy);	// no eject meanwhile
	CHECK (start (a, true, 1ull << 30, "", "") && strstr (start (a, true, 1ull << 30, "", ""), "running"));
	int steps = run ();
	CHECK (steps > 0 && state () == kDone && progress () == 100 && have ());
	CHECK (!stor::status (stor::kUSB).busy && S.file[0].empty () && S.file[1].empty () && !S.open[0] && !S.open[1]);
	CHECK (S.max_call <= 65536 && S.crossings == 0);		// every call <= 64 KB, inside one 64 KB window
	const Results &r = results ();
	CHECK (r.size == 8ull << 20 && r.split && r.verify_ok);
	for (int k = 0; k < kSizes; k++)
	{
		CHECK (r.seq[k].wbytes == 8ull << 20 && r.seq[k].rbytes == 8ull << 20 && r.seq[k].calls == kPiece[k] / 65536);
		CHECK (mbps_x100 (r.seq[k].rbytes, r.seq[k].rus) == 10000);	// 100 MB/s, every piece size
		CHECK (r.seq[k].sync == 3000);
		uint32_t wm = mbps_x100 (r.seq[k].wbytes, r.seq[k].wus);	// 50 MB/s, the sync's 3 ms in it
		CHECK (wm > 4800 && wm < 5000);
	}
	CHECK (r.rnd4.n == 100 && r.rnd4.p50 == 39 && r.rnd4.max == 39);	// 4096 B at 100 MB/s = 39 us
	CHECK (r.rnd64.p50 == 625 && r.rnd64.p99 == 625 && r.rnd64.iops == 1600);
	CHECK (r.raw && r.raw_bytes == 8ull << 20 && mbps_x100 (r.raw_bytes, r.raw_us) / 100 == 200 && r.raw4.n == 100);	// (312.5 us a call, whole us)
	CHECK (r.busy[0].ran && r.busy[1].ran && r.busy[0].streaming && r.busy[0].underruns == 0 && r.busy[0].lat.n == 100);
	CHECK (r.busy[0].loop_max_us == 0 && r.busy[1].loop_max_us > 1000);	// the writes in the loop's way
	CHECK (r.busy[1].wbytes >= 65536 && r.busy[1].wbytes % 65536 == 0);
	CHECK (S.log.find ("stor bench USB: 8 MB file, exFAT, USB 3 SuperSpeed, calls <= 64 KB\n") == 0);
	CHECK (S.log.find ("64 KB         49.07    100.00       1.2/0.6    3.000     1\n") != std::string::npos);
	CHECK (S.log.find ("    100.00") != std::string::npos);
	CHECK (S.log.find ("rnd 64K    0.625   0.625   0.625   1600\n") != std::string::npos);
	CHECK (S.log.find ("raw seq 64K 200.32 MB/s over 8 MB (no FatFs)\n") != std::string::npos);
	CHECK (S.log.find ("verify ok") != std::string::npos);
	printf ("%s", S.log.c_str ());
	char t[2048];
	int tn = table (r, t, sizeof t);
	CHECK (tn == (int) strlen (t) && S.log.find (t) == 0);
	for (char *p = t, *e; *p; p = e + 1)			// a compact table: no line over 72 characters
	{
		e = strchr (p, '\n');
		CHECK (e && e - p <= 72);
	}
	char j[4096];
	int jn = json (j, sizeof j);
	CHECK (jn == (int) strlen (j) && strstr (j, "\"bench\":{\"state\":\"done\",\"progress\":100,\"phase\":\"done\",\"vol\":\"USB\",\"have\":true,\"sizeMB\":8"));
	CHECK (strstr (j, "{\"pieceKB\":64,\"writeMBps\":49.") && strstr (j, "\"readMBps\":100.00}") && strstr (j, "\\nverify ok"));
	CHECK (!strchr (j, '\n'));

	// one call a piece (SD / NVMe): 1 MB calls; no raw access: said so
	S = Sim ();
	set_io (&kSimNoRaw);
	a.vol = stor::kNVME;
	CHECK (start (a, false, 0, "PCIe gen 2 x1", "FAT32") == 0);
	CHECK (run () > 0 && state () == kDone);
	CHECK (S.max_call == (1u << 20) && results ().seq[2].calls == 1 && !results ().raw);
	CHECK (S.log.find ("raw: not available here") != std::string::npos);

	// a corrupted read is reported (the 30th read: in the first sequential pass)
	S = Sim ();
	S.corrupt_at = 30;
	set_io (&kSimIo);
	CHECK (start (a, true, 0, "", "") == 0 && run () > 0 && state () == kDone);
	CHECK (!results ().verify_ok && results ().verify_off == 30ull * 65536);
	CHECK (S.log.find ("verify MISMATCH at 1966080") != std::string::npos);

	// a drive slower than the recorder's pace: the busy phase alternates and finishes
	S = Sim ();
	S.write_bps = 1ull << 20;					// 1 MB/s: every write is behind the 2 MB/s pace
	a.n = 40;
	CHECK (start (a, true, 0, "", "") == 0 && run () > 0 && state () == kDone);
	CHECK (results ().busy[1].lat.n == 40 && results ().busy[1].wbytes >= 39ull * 65536);

	// the busy phase's second file stays under kBusyWriteMax however slow the drive; a hold (the SD's streams) makes
	// each step one operation, and the bench still ends
	S = Sim ();
	S.write_bps = 1ull << 20;
	a.n = 400;
	{
		static const Io kHold = {s_open, s_write, s_read, s_sync, s_close, s_remove, s_raw, s_raw_size, s_now, s_streaming,
					 s_under, s_loop_reset, s_loop_read, s_log, [] () { return true; }};
		set_io (&kHold);
		CHECK (start (a, true, 0, "", "") == 0 && run () > 0 && state () == kDone);
		CHECK (results ().busy[1].wbytes == kBusyWriteMax && results ().busy[1].lat.n == 400);
		set_io (&kSimIo);
	}
	// a start that cannot create its file: no result shown (the old one was reset)
	{
		static const Io kNoOpen = {[] (int, int, bool) { return false; }, s_write, s_read, s_sync, s_close, s_remove, 0, 0,
					   s_now, s_streaming, s_under, s_loop_reset, s_loop_read, s_log, 0};
		CHECK (have ());
		set_io (&kNoOpen);
		CHECK (start (a, true, 0, "", "") && !have ());
		set_io (&kSimIo);
	}
	// too little free space: shrunk to fit; far too little: refused
	S = Sim ();
	a.mb = 256, a.n = 20;
	CHECK (start (a, true, 100ull << 20, "", "") == 0 && results ().size == 76ull << 20);
	cancel ();
	CHECK (state () == kCancelled && S.file[0].empty () && S.removes >= 4 && !S.open[0]);
	CHECK (start (a, true, 30ull << 20, "", "") && strstr (start (a, true, 30ull << 20, "", ""), "only 30 MB free on NVME"));
	// a failing write: failed, cleaned up, the volume free again
	S = Sim ();
	a.mb = 8;
	stor::set_busy (stor::kNVME, false);
	S.write_bps = 0;						// (never: the write callback below fails instead)
	{
		static const Io kBad = {s_open, [] (int, const void *, uint32_t) { return -1; }, s_read, s_sync, s_close, s_remove,
					0, 0, s_now, s_streaming, s_under, s_loop_reset, s_loop_read, s_log, 0};
		set_io (&kBad);
		CHECK (start (a, true, 0, "", "") == 0);
		CHECK (run () == 0 && state () == kFailed && strstr (phase (), "a write failed") && !stor::status (stor::kNVME).busy);
		CHECK (S.log.find ("stor: bench NVME failed: a write failed (full?) (sequential write)") != std::string::npos);
	}

	// ---- (2) a temp folder: real files, removed after; a cancel removes them too
	char dir[] = "/tmp/stor_benchXXXXXX";
	CHECK (mkdtemp (dir));
	g_Dir = dir;
	S = Sim ();
	set_io (&kFileIo);
	a = Args ();
	a.vol = stor::kUSB, a.mb = 8, a.n = 50;
	CHECK (start (a, true, 0, "", "FAT32") == 0 && exists (0));
	CHECK (run () > 0 && state () == kDone && results ().verify_ok);
	CHECK (!exists (0) && !exists (1));
	CHECK (start (a, true, 0, "", "") == 0);
	for (int i = 0; i < 5; i++) step ();
	CHECK (exists (0));
	cancel ();
	CHECK (state () == kCancelled && !exists (0) && !exists (1) && !strcmp (phase (), "cancelled"));
	rmdir (dir);

	free (wb), free (rb);
	printf ("test_stor_bench: ok\n");
	return 0;
}
