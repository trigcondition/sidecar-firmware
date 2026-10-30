// pi/engine/tests/test_paircode.cpp - the pairing code's portable part (paircode.cpp): the code's shape, the constant-
// time compare, the rate limit and its lockout, sessions and their expiry, the remembered devices' file and its atomic
// save on a temp folder (a failing write or rename leaves it as it was), a device surviving a reboot, revocation, the
// cmdline dev bypass, the 7310 PAIR / AUTH lines, and that no answer or file ever holds the code except the card-root
// recourse file. Only made-up codes and secrets.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "check.h"
#include "paircode.h"

using namespace paircode;

// ---- the card (as test_wifi_conf.cpp's)
static std::string g_root;
static int g_failRename = 0, g_failWrite = 0;	// the next one fails
static std::string mac (const char *p) { return g_root + p; }
static bool f_write (const char *p, const char *d, unsigned n)
{
	if (g_failWrite && --g_failWrite == 0) return false;
	FILE *f = fopen (mac (p).c_str (), "wb");
	if (!f) return false;
	bool ok = fwrite (d, 1, n, f) == n;
	return fclose (f) == 0 && ok;
}
static bool f_rename (const char *a, const char *b)
{
	if (g_failRename && --g_failRename == 0) return false;
	struct stat st;
	if (stat (mac (b).c_str (), &st) == 0) return false;	// as FatFs: the target must not exist
	return ::rename (mac (a).c_str (), mac (b).c_str ()) == 0;
}
static bool f_remove (const char *p) { return unlink (mac (p).c_str ()) == 0; }
static bool f_exists (const char *p) { struct stat st; return stat (mac (p).c_str (), &st) == 0; }
static bool f_read (const char *p, char *out, unsigned size)
{
	FILE *f = fopen (mac (p).c_str (), "rb");
	if (!f) return false;
	size_t n = fread (out, 1, size - 1, f);
	fclose (f);
	out[n] = 0;
	return true;
}
static const wificonf::Fs kFs = {f_write, f_rename, f_remove, f_exists, f_read};
static std::string slurp (const char *p)
{
	char b[4096];
	return f_read (p, b, sizeof b) ? std::string (b) : std::string ("<none>");
}

// ---- the device's entropy and clock, made predictable
static uint32_t g_ms = 0;
static uint32_t g_seed = 12345;
static bool g_stuckRandom = false;
static void fake_random (uint8_t *out, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
	{
		g_seed = g_seed * 1103515245u + 12345u;
		out[i] = g_stuckRandom ? 0x41 : (uint8_t) (g_seed >> 16);
	}
}
static uint32_t fake_ms (void) { return g_ms; }
static const Deps kDeps = {fake_random, fake_ms};

static Gate make (Mode m = kOn, const char *fixed = 0)
{
	Gate g;
	g.init (m, fixed, kDeps);
	return g;
}

// Before init() there is no clock and no entropy: nothing is paired and nothing can be (a request cannot reach it that
// early on the Pi, but it must never fall open if one did)
static void test_uninit ()
{
	Gate g;
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	CHECK (!g.check ("") && !g.check ("0123456789abcdef0123456789abcdef"));
	CHECK (g.pair ("12345678", 0, false, id, sec, tok) != kOk);
	CHECK (g.resume ("00000000", "00000000000000000000000000000000", tok) != kOk);
	CHECK (g.sessions () == 0 && g.clients () == 0 && g.lock_left_s () == 0);
}

static void test_code ()
{
	Gate g = make ();
	CHECK (g.gated () && g.mode () == kOn);
	CHECK (strlen (g.code ()) == kDigits);
	for (unsigned i = 0; i < kDigits; i++) CHECK (g.code ()[i] >= '0' && g.code ()[i] <= '9');
	char b[16];
	g.grouped (b, sizeof b);
	CHECK (strlen (b) == 9 && b[4] == ' ' && strncmp (b, g.code (), 4) == 0 && strncmp (b + 5, g.code () + 4, 4) == 0);
	g.grouped (b, 4);
	CHECK (b[0] == 0);			// too small: nothing written
	// two boots do not give the same code (the real RNG; here the counter keeps running)
	Gate h = make ();
	CHECK (strcmp (g.code (), h.code ()) != 0);
	// every digit can come out (a byte at 250 and above is drawn again: no modulo bias)
	bool seen[10] = {};
	for (int i = 0; i < 200; i++) { Gate k = make (); for (unsigned d = 0; d < kDigits; d++) seen[k.code ()[d] - '0'] = true; }
	for (int d = 0; d < 10; d++) CHECK (seen[d]);
	// the compare has no early exit and still answers
	CHECK (ct_equal ("12345678", "12345678", 8));
	CHECK (!ct_equal ("12345678", "12345679", 8));
	CHECK (!ct_equal ("12345678", "02345678", 8));
	CHECK (ct_equal ("", "", 0));
}

static void test_cmdline ()
{
	char f[kDigits + 1];
	CHECK (mode_from_cmdline (0, f) == kOn && f[0] == 0);
	CHECK (mode_from_cmdline ("", f) == kOn && f[0] == 0);
	CHECK (mode_from_cmdline ("1", f) == kOn && f[0] == 0);			// junk: the gate stays on
	CHECK (mode_from_cmdline ("off", f) == kOn && f[0] == 0);
	CHECK (mode_from_cmdline ("1234567", f) == kOn && f[0] == 0);		// too short: on
	CHECK (mode_from_cmdline ("123456789", f) == kOn && f[0] == 0);
	CHECK (mode_from_cmdline ("0", f) == kOff && f[0] == 0);
	CHECK (mode_from_cmdline ("00000000", f) == kFixed && strcmp (f, "00000000") == 0);
	CHECK (mode_from_cmdline ("13572468", f) == kFixed && strcmp (f, "13572468") == 0);
	// paircode=0: nothing is gated and nothing needs a token
	Gate off = make (kOff);
	CHECK (!off.gated () && off.check (0) && off.check ("nonsense"));
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	CHECK (off.pair (off.code (), "x", true, id, sec, tok) == kOffGate);
	CHECK (off.resume ("00000000", "00000000000000000000000000000000", tok) == kOffGate);
	// a fixed code (a dev card that still asks for one)
	Gate fix = make (kFixed, "13572468");
	CHECK (fix.gated () && strcmp (fix.code (), "13572468") == 0);
	CHECK (fix.pair ("13572468", "dev", false, id, sec, tok) == kOk && fix.check (tok));
}

static void test_pair ()
{
	g_ms = 0;
	Gate g = make ();
	char code[kDigits + 1];
	strcpy (code, g.code ());
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	// nothing is paired to start with
	CHECK (!g.check ("") && !g.check ("0") && !g.check (0) && g.sessions () == 0 && g.clients () == 0);
	// a wrong code, then the right one
	CHECK (g.pair ("00000000", "x", false, id, sec, tok) == kWrongCode || strcmp (code, "00000000") == 0);
	g_ms += 1000;
	CHECK (g.pair (code, "  Studio iMac  ", true, id, sec, tok) == kOk);
	CHECK (strlen (tok) == kTokenHex && strlen (id) == kIdHex && strlen (sec) == kSecretHex);
	CHECK (g.check (tok) && g.sessions () == 1 && g.clients () == 1 && g.dirty ());
	// the token is not the code and the card's file holds neither the code nor the secret
	char text[2048];
	CHECK (g.format (text, sizeof text) > 0);
	CHECK (strstr (text, code) == 0 && strstr (text, sec) == 0 && strstr (text, tok) == 0);
	CHECK (strstr (text, id) != 0 && strstr (text, "Studio iMac") != 0);	// the name was trimmed
	// a second client: its own token and secret
	char id2[kIdHex + 1], sec2[kSecretHex + 1], tok2[kTokenHex + 1];
	g_ms += 1000;
	CHECK (g.pair (code, "phone", true, id2, sec2, tok2) == kOk);
	CHECK (strcmp (tok, tok2) != 0 && strcmp (sec, sec2) != 0 && strcmp (id, id2) != 0);
	CHECK (g.clients () == 2 && g.sessions () == 2);
	// one of them asks not to be remembered
	g_ms += 1000;
	char id3[kIdHex + 1], sec3[kSecretHex + 1], tok3[kTokenHex + 1];
	CHECK (g.pair (code, "curl", false, id3, sec3, tok3) == kOk);
	CHECK (id3[0] == 0 && sec3[0] == 0 && g.clients () == 2 && g.check (tok3));
	// a bad code's shape is refused without quoting it
	g_ms += 1000;
	CHECK (g.pair ("12", "x", false, id3, sec3, tok3) == kBadInput);
	CHECK (strstr (err_text (kBadInput), "12") == 0);
	g_ms += 1000;
	CHECK (g.pair ("1234567a", "x", false, id3, sec3, tok3) == kBadInput);
	for (Result r : {kWrongCode, kLocked, kBadInput, kNoDevice, kOffGate})
		CHECK (strstr (err_text (r), code) == 0 && strstr (err_text (r), sec) == 0);
}

static void test_limit ()
{
	g_ms = 100000;
	Gate g = make ();
	char code[kDigits + 1];
	strcpy (code, g.code ());
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	const char *wrong = strcmp (code, "11111111") == 0 ? "22222222" : "11111111";
	// one attempt a second
	CHECK (g.pair (wrong, 0, false, id, sec, tok) == kWrongCode);
	CHECK (g.pair (wrong, 0, false, id, sec, tok) == kLocked && g.lock_left_s () == 1);
	CHECK (g.pair (code, 0, false, id, sec, tok) == kLocked);	// even the right one waits its second
	// five wrong ones lock it for a minute
	for (unsigned i = 1; i < kMaxFails; i++) { g_ms += 1000; CHECK (g.pair (wrong, 0, false, id, sec, tok) == kWrongCode); }
	CHECK (g.lock_left_s () >= 59 && g.lock_left_s () <= 60);
	g_ms += 30000;
	CHECK (g.pair (code, 0, false, id, sec, tok) == kLocked && g.lock_left_s () >= 29);
	g_ms += 31000;
	CHECK (g.lock_left_s () == 0);
	// the next lockout is twice as long
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; CHECK (g.pair (wrong, 0, false, id, sec, tok) == kWrongCode); }
	CHECK (g.lock_left_s () > 110 && g.lock_left_s () <= 120);
	g_ms += 121000;
	// the right code clears the count and the doubling
	CHECK (g.pair (code, 0, false, id, sec, tok) == kOk && g.lock_left_s () == 0);
	g_ms += 1000;
	for (unsigned i = 0; i < kMaxFails; i++) { CHECK (g.pair (wrong, 0, false, id, sec, tok) == kWrongCode); g_ms += 1000; }
	CHECK (g.lock_left_s () >= 59 && g.lock_left_s () <= 60);	// a minute again, not four
	// a wrong secret counts the same way
	g_ms += 61000;
	Gate h = make ();
	CHECK (h.resume ("00000000", "ffffffffffffffffffffffffffffffff", tok) == kNoDevice);
	g_ms += 1000;
	CHECK (h.resume ("zz", "nope", tok) == kNoDevice);
	// a locked gate still serves the devices that are already paired
	g_ms += 1000;
	char ptok[kTokenHex + 1];
	CHECK (h.pair (h.code (), "paired", true, id, sec, ptok) == kOk);
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; h.pair ("00000000", 0, false, id, sec, tok); }
	CHECK (h.lock_left_s () > 0 && h.check (ptok));
}

static void test_sessions ()
{
	g_ms = 0;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	CHECK (g.pair (g.code (), "a", false, id, sec, tok) == kOk);
	// an hour without a gated request ends it
	g_ms += kIdleMs - 1000;
	CHECK (g.check (tok));				// and that use pushes it on
	g_ms += kIdleMs - 1000;
	CHECK (g.check (tok));
	g_ms += kIdleMs;
	CHECK (!g.check (tok) && g.sessions () == 0);
	// a day ends it even when it is used all the time
	g_ms = 0;
	Gate h = make ();
	CHECK (h.pair (h.code (), "a", false, id, sec, tok) == kOk);
	for (unsigned i = 0; i < 24; i++) { g_ms += 55u * 60u * 1000u; CHECK (h.check (tok)); }	// 22 h of use
	g_ms += kLifeMs;
	CHECK (!h.check (tok));
	// nine sessions: the least recently used goes, the others stay
	g_ms = 0;
	Gate k = make ();
	char t[kSessions + 1][kTokenHex + 1];
	for (unsigned i = 0; i <= kSessions; i++)
	{
		g_ms += 1000;
		CHECK (k.pair (k.code (), 0, false, id, sec, t[i]) == kOk);
	}
	CHECK (k.sessions () == kSessions);
	CHECK (!k.check (t[0]));			// the oldest one went
	CHECK (k.check (t[1]) && k.check (t[kSessions]));
}

static void test_card ()
{
	char tmpl[] = "/tmp/paircode-XXXXXX";
	CHECK (mkdtemp (tmpl));
	g_root = tmpl;
	CHECK (mkdir (mac ("/.rytm").c_str (), 0755) == 0);
	g_ms = 0;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	char id2[kIdHex + 1], sec2[kSecretHex + 1], tok2[kTokenHex + 1];
	CHECK (g.pair (g.code (), "iMac", true, id, sec, tok) == kOk);
	g_ms += 1000;
	CHECK (g.pair (g.code (), "iPad", true, id2, sec2, tok2) == kOk);
	CHECK (g.save (kFs, kPath));
	g.saved ();
	CHECK (!g.dirty () && !f_exists ("/.rytm/pair.txt.part"));
	std::string on_card = slurp (kPath);
	CHECK (on_card.find (g.code ()) == std::string::npos);
	CHECK (on_card.find (sec) == std::string::npos && on_card.find (sec2) == std::string::npos);
	CHECK (on_card.find (tok) == std::string::npos);
	CHECK (on_card.find ("iMac") != std::string::npos && on_card.find ("v=1") != std::string::npos);

	// ---- a reboot: a new code, the file read back, the remembered devices resume without it
	g_ms += 5000;
	Gate b = make ();
	CHECK (strcmp (b.code (), g.code ()) != 0);
	char text[kFileMax + 64];
	CHECK (f_read (kPath, text, sizeof text));
	b.load (text);
	CHECK (b.clients () == 2 && !b.dirty ());
	CHECK (!b.check (tok));				// the old session did not survive
	char ntok[kTokenHex + 1];
	CHECK (b.resume (id, sec, ntok) == kOk && b.check (ntok) && strcmp (ntok, tok) != 0);
	g_ms += 1000;
	CHECK (b.resume (id2, sec2, ntok) == kOk);
	// a wrong secret, and the right secret under the wrong id
	g_ms += 1000;
	CHECK (b.resume (id, "00000000000000000000000000000000", ntok) == kNoDevice);
	g_ms += 1000;
	CHECK (b.resume (id2, sec, ntok) == kNoDevice);
	// the file's junk is ignored, its order and names kept
	b.load ("v=1\nc=bad\n# a comment\nc=zzzzzzzz 00 x\nnonsense\n");
	CHECK (b.clients () == 0);
	b.load (text);
	CHECK (b.clients () == 2);

	// ---- revoking
	g_ms += 1000;
	CHECK (b.resume (id, sec, ntok) == kOk);
	b.forget (ntok);				// this device only
	CHECK (b.clients () == 1 && b.dirty () && !b.check (ntok));
	g_ms += 1000;
	CHECK (b.resume (id, sec, ntok) == kNoDevice);	// it is gone
	g_ms += 1000;
	CHECK (b.resume (id2, sec2, ntok) == kOk);
	b.forget ("not a token");			// nothing happens
	CHECK (b.clients () == 1);
	b.revoke_all ();
	CHECK (b.clients () == 0 && b.sessions () == 0 && !b.check (ntok));
	CHECK (b.save (kFs, kPath));
	CHECK (slurp (kPath).find ("c=") == std::string::npos);

	// ---- nine devices: the oldest goes
	g_ms += 1000;
	Gate n = make ();
	char ids[kClients + 1][kIdHex + 1], secs[kClients + 1][kSecretHex + 1];
	for (unsigned i = 0; i <= kClients; i++)
	{
		g_ms += 1000;
		CHECK (n.pair (n.code (), "dev", true, ids[i], secs[i], tok) == kOk);
	}
	CHECK (n.clients () == kClients);
	g_ms += 1000;
	CHECK (n.resume (ids[0], secs[0], ntok) == kNoDevice);		// the first one was dropped
	g_ms += 1000;
	CHECK (n.resume (ids[kClients], secs[kClients], ntok) == kOk);
	// a file with more than kClients lines keeps the first kClients
	std::string many = "v=1\n";
	for (int i = 0; i < 20; i++)
		many += "c=0000000" + std::string (1, "0123456789abcdefghij"[i]) + " "
			+ std::string (64, '0') + " dev\n";
	n.load (many.c_str ());
	CHECK (n.clients () == kClients);

	// ---- the atomic save: a failing write or rename leaves the file as it was
	g_ms += 1000;
	Gate s = make ();
	CHECK (s.pair (s.code (), "keep", true, id, sec, tok) == kOk);
	CHECK (s.save (kFs, kPath));
	std::string before = slurp (kPath);
	g_ms += 1000;
	CHECK (s.pair (s.code (), "second", true, id2, sec2, tok2) == kOk);
	g_failWrite = 1;
	CHECK (!s.save (kFs, kPath));
	CHECK (slurp (kPath) == before && !f_exists ("/.rytm/pair.txt.part"));
	g_failRename = 2;			// the .part to pair.txt rename: pair.txt.prev goes back
	CHECK (!s.save (kFs, kPath));
	CHECK (slurp (kPath) == before && !f_exists ("/.rytm/pair.txt.part"));
	g_failRename = 0;
	CHECK (s.save (kFs, kPath));
	CHECK (slurp (kPath) != before && slurp ("/.rytm/pair.txt.prev") == before);
	CHECK (s.format (text, 60) == -1);	// does not fit: nothing written
	CHECK (text[0] == 0);

	// ---- a full file of the longest names still fits (kFileMax: format() must never fail silently, or pairings
	// would stop surviving a reboot)
	Gate f = make ();
	for (unsigned i = 0; i < kClients; i++)
	{
		g_ms += 1000;
		CHECK (f.pair (f.code (), "123456789012345678901234567890", true, ids[i], secs[i], tok) == kOk);
	}
	CHECK (f.clients () == kClients);
	int nf = f.format (text, sizeof text);
	CHECK (nf > 0 && (unsigned) nf < kFileMax);
	CHECK (f.save (kFs, kPath));
	Gate f2 = make ();
	CHECK (f_read (kPath, text, sizeof text));
	f2.load (text);
	CHECK (f2.clients () == kClients);		// every one of them came back
	g_ms += 1000;
	CHECK (f2.resume (ids[kClients - 1], secs[kClients - 1], ntok) == kOk);

	std::string cmd = "rm -rf " + g_root;
	CHECK (system (cmd.c_str ()) == 0);
}

// check_quiet does not keep a session alive: only real use does (GET /pair/status must not be a keep-alive)
static void test_quiet ()
{
	g_ms = 0;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	CHECK (g.pair (g.code (), "a", false, id, sec, tok) == kOk);
	CHECK (g.check_quiet (tok) && !g.check_quiet ("x") && !g.check_quiet (0));
	g_ms += kIdleMs - 1000;
	CHECK (g.check_quiet (tok));				// still inside the hour
	g_ms += 2000;
	CHECK (!g.check_quiet (tok) && !g.check (tok));		// polling it did not push the hour on
	// real use does, as before
	g_ms += 1000;
	char tok2[kTokenHex + 1];
	CHECK (g.pair (g.code (), "b", false, id, sec, tok2) == kOk);
	g_ms += kIdleMs - 1000;
	CHECK (g.check (tok2));
	g_ms += kIdleMs - 1000;
	CHECK (g.check_quiet (tok2));
}

// A flood of refused attempts makes at most one log line every 10 s, with the count (F3: the log ring must not be
// pushed out by an unauthenticated client)
static void test_report ()
{
	g_ms = 1000000;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	unsigned n = 0;
	CHECK (!g.take_report (&n));				// nothing happened yet
	for (unsigned i = 0; i < 50; i++) { g_ms += 10; g.pair ("00000000", 0, false, id, sec, tok); }
	CHECK (g.take_report (&n) && n == 50);			// one line for the fifty
	CHECK (!g.take_report (&n));				// and not again
	for (unsigned i = 0; i < 20; i++) { g_ms += 10; g.pair ("00000000", 0, false, id, sec, tok); }
	CHECK (!g.take_report (&n));				// still inside the 10 s
	g_ms += 10000;
	CHECK (g.take_report (&n) && n == 20);
	// a correct code is not a refusal
	g_ms += 10000;
	CHECK (g.pair (g.code (), 0, false, id, sec, tok) == kOk);
	CHECK (!g.take_report (&n));
}

// A browser (or sdboot.py) holding a stale saved pairing must not be able to lock its own owner out. On the device
// a stale localStorage entry burned a pairing attempt on every page load: five reloads reached the lockout, which
// then doubled, and because the lockout is shared with port 7310 it also blocked `sdboot.py install` - the way
// firmware is recovered. And it was reported as "the pairing code is 8 digits", about a code nobody had mistyped.
static void test_stale_device ()
{
	g_ms = 500000;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	char code[kDigits + 1];
	strcpy (code, g.code ());
	// the shapes a stale or junk store can hold, including the "undefined" a page writes when a field was missing
	const char *junk[][2] = {
		{"undefined", "undefined"}, {"", ""}, {"0123abcd", "short"}, {"zzzzzzzz", "0123456789abcdef0123456789abcdef"},
		{"0123abc", "0123456789abcdef0123456789abcdef"}, {"0123abcd", "0123456789abcdef0123456789abcdeff"},
		{"0123abcd", "0123456789abcdef0123456789abcdeg"},
	};
	for (unsigned pass = 0; pass < 3; pass++)		// several reloads, as a page does
		for (const auto &j : junk)
		{
			g_ms += 10;
			Result r = g.resume (j[0], j[1], tok);
			CHECK (r == kNoDevice);
			// never the code's complaint: that sent a user hunting for a typo in a code they had got right
			CHECK (strstr (err_text (r), "8 digits") == 0);
			CHECK (g.lock_left_s () == 0);		// and nothing is locked by it
		}
	// a well-formed secret this Sidecar simply does not know (revoked, a new card) is the same: not a code guess
	for (unsigned i = 0; i < 4 * kMaxFails; i++)
	{
		g_ms += 10;
		CHECK (g.resume ("0123abcd", "0123456789abcdef0123456789abcdef", tok) == kNoDevice);
	}
	CHECK (g.lock_left_s () == 0);
	// so the owner can still pair, and the SD file port is still reachable (they share the one limiter)
	CHECK (g.pair (code, "the owner", true, id, sec, tok) == kOk);
	char reply[400], line[64];
	snprintf (line, sizeof line, "PAIR %s", code);
	g_ms += 1000;
	Gate t = make (kFixed, "13572468");
	for (unsigned i = 0; i < 20; i++) { g_ms += 10; t.resume ("0123abcd", "0123456789abcdef0123456789abcdef", tok); }
	CHECK (t.tcp_auth ("PAIR 13572468", reply, sizeof reply));	// not locked out of installing firmware

	// a real wrong code still locks, as before: only pair() counts
	Gate w = make ();
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; w.pair ("00000000", 0, false, id, sec, tok); }
	CHECK (w.lock_left_s () > 0);
	// ... and the owner's own remembered device can still come back, and that lifts the lockout
	g_ms += 1000;
	Gate o = make ();
	CHECK (o.pair (o.code (), "owner", true, id, sec, tok) == kOk);
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; o.pair ("00000000", 0, false, tok, tok, tok); }
	CHECK (o.lock_left_s () > 0);
	char ntok[kTokenHex + 1];
	CHECK (o.resume (id, sec, ntok) == kOk);	// a lockout does not block a device that is already paired
	CHECK (o.lock_left_s () == 0);			// and only the owner can do that, so it lifts it
}

static void test_tcp ()
{
	g_ms = 0;
	Gate g = make ();
	char reply[400];
	// nothing is allowed before PAIR / AUTH, and the refusal says how
	CHECK (!g.tcp_auth ("LS", reply, sizeof reply));
	CHECK (strncmp (reply, "ERR unauthenticated", 19) == 0 && strstr (reply, "SIDECAR_PAIR_CODE") != 0);
	CHECK (strstr (reply, g.code ()) == 0);
	CHECK (!g.tcp_auth ("PUT engine.img 100 0", reply, sizeof reply) && strncmp (reply, "ERR", 3) == 0);
	CHECK (!g.tcp_auth ("", reply, sizeof reply));
	// a wrong code
	CHECK (!g.tcp_auth ("PAIR 00000000", reply, sizeof reply) || strcmp (g.code (), "00000000") == 0);
	g_ms += 1000;
	CHECK (!g.tcp_auth ("PAIR 123", reply, sizeof reply) && strstr (reply, "8 digits") != 0);
	// the right one: an id and a secret for the next time
	g_ms += 1000;
	char line[64];
	snprintf (line, sizeof line, "PAIR %s", g.code ());
	CHECK (g.tcp_auth (line, reply, sizeof reply));
	CHECK (strncmp (reply, "OK ", 3) == 0 && g.clients () == 1);
	char id[kIdHex + 1], sec[kSecretHex + 1];
	CHECK (sscanf (reply + 3, "%8s %32s", id, sec) == 2 && strlen (id) == kIdHex && strlen (sec) == kSecretHex);
	// AUTH with them, after a reboot
	g_ms += 1000;
	char text[1024];
	CHECK (g.format (text, sizeof text) > 0);
	Gate b = make ();
	b.load (text);
	snprintf (line, sizeof line, "AUTH %s %s", id, sec);
	CHECK (b.tcp_auth (line, reply, sizeof reply) && strcmp (reply, "OK") == 0);
	// a wrong secret, and malformed lines
	g_ms += 1000;
	snprintf (line, sizeof line, "AUTH %s %s", id, "00000000000000000000000000000000");
	CHECK (!b.tcp_auth (line, reply, sizeof reply) && strstr (reply, sec) == 0);
	g_ms += 1000;
	CHECK (!b.tcp_auth ("AUTH x", reply, sizeof reply) && strstr (reply, "AUTH <id> <secret>") != 0);
	g_ms += 1000;
	CHECK (!b.tcp_auth ("AUTH 00000000", reply, sizeof reply));
	// the reply never carries the code
	for (const char *l : {"LS", "AUTH x y", "PAIR 99999999", ""})
	{
		g_ms += 1000;
		b.tcp_auth (l, reply, sizeof reply);
		CHECK (strstr (reply, b.code ()) == 0);
	}
}

// A dead or stuck generator would make every code, token and secret the same. The gate refuses to run on it - a loud
// refusal, never a weak code (BACKLOG F4) - and the Pi side's own test of the raw generator can say the same.
static void test_stuck_random ()
{
	g_ms = 1000;
	g_stuckRandom = true;
	Gate g = make ();
	g_stuckRandom = false;
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1], reply[400];
	CHECK (!g.rng_ok () && g.gated ());
	CHECK (strspn (g.code (), "-") == kDigits);		// no digits at all: nothing to read off a screen
	CHECK (g.pair ("00000000", 0, true, id, sec, tok) == kNoRandom && g.pair (g.code (), 0, true, id, sec, tok) == kNoRandom);
	CHECK (g.resume ("0123abcd", "0123456789abcdef0123456789abcdef", tok) == kNoRandom);
	CHECK (!g.tcp_auth ("PAIR 00000000", reply, sizeof reply) && strstr (reply, "random number generator") != 0);
	CHECK (!g.check ("") && g.sessions () == 0 && g.clients () == 0);
	// a fixed code needs the generator too (every token comes from it)
	g_stuckRandom = true;
	Gate f = make (kFixed, "13572468");
	g_stuckRandom = false;
	CHECK (!f.rng_ok () && f.pair ("13572468", 0, false, id, sec, tok) == kNoRandom);
	// the gate off does not use it, and stays off
	g_stuckRandom = true;
	Gate o = make (kOff);
	g_stuckRandom = false;
	CHECK (o.rng_ok () && o.check (0));
	// the Pi's own test of the raw generator (paircode_pi.cpp) refuses the same way
	Gate h = make ();
	CHECK (h.rng_ok ());
	h.fail_rng ();
	CHECK (!h.rng_ok () && h.pair (h.code (), 0, false, id, sec, tok) == kNoRandom);
}

// The limiter is per peer: someone else on a shared Wi-Fi guessing locks themselves out, not the owner (audit
// 2026-10-26: one limiter for everyone let anyone hold pairing locked for good, on the web and on 7310 alike)
static const uint32_t kOwner = 0x0501a8c0, kMallory = 0x6601a8c0;	// 192.168.1.5, 192.168.1.102
static void test_per_peer ()
{
	g_ms = 2000000;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1], reply[400], line[64];
	const char *wrong = strcmp (g.code (), "11111111") == 0 ? "22222222" : "11111111";
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; CHECK (g.pair (wrong, 0, false, id, sec, tok, kMallory) == kWrongCode); }
	CHECK (g.lock_left_s (kMallory) >= 59 && g.lock_left_s (kOwner) == 0);
	CHECK (g.pair (g.code (), 0, false, id, sec, tok, kMallory) == kLocked);	// the guesser is locked, even with the code
	CHECK (g.pair (g.code (), "owner", true, id, sec, tok, kOwner) == kOk);	// the owner is not
	// and 7310 (sdboot.py install) on the owner's address is not either
	snprintf (line, sizeof line, "PAIR %s", g.code ());
	g_ms += 1000;
	CHECK (g.tcp_auth (line, reply, sizeof reply, kOwner));
	// the owner's success does not clear the guesser's lockout or its doubling
	CHECK (g.lock_left_s (kMallory) > 0);
	g_ms += 61000;
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; g.pair (wrong, 0, false, id, sec, tok, kMallory); }
	CHECK (g.lock_left_s (kMallory) > 110);					// two minutes: it doubled
	// a resume from the owner's address clears nothing of the guesser's (it used to reset everyone's escalation, so
	// the owner's reloads kept an attacker at one-minute lockouts)
	char ntok[kTokenHex + 1];
	CHECK (g.resume (id, sec, ntok, kOwner) == kOk);
	CHECK (g.lock_left_s (kMallory) > 100);
	g_ms += 121000;
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; g.pair (wrong, 0, false, id, sec, tok, kMallory); }
	CHECK (g.lock_left_s (kMallory) > 230);					// four minutes, not one
	// a resume from the locked address itself is the owner there: it lifts that address's lockout
	CHECK (g.resume (id, sec, ntok, kMallory) == kOk && g.lock_left_s (kMallory) == 0);
}

// A malformed code is not a guess: it cannot match, so counting it protects nothing - and a bare POST /pair, a
// 7-digit typo or a paste with a stray letter must never move the owner towards a lockout (audit 2026-10-26)
static void test_malformed ()
{
	g_ms = 3000000;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	for (const char *bad : {"", "1234567", "123456789", "1234 5678", "abcdefgh", "1234567a"})
		for (unsigned i = 0; i < 4; i++) CHECK (g.pair (bad, 0, false, id, sec, tok, kOwner) == kBadInput);
	CHECK (g.pair (0, 0, false, id, sec, tok, kOwner) == kBadInput);
	CHECK (g.lock_left_s (kOwner) == 0 && g.global_wait_s () == 0);
	CHECK (g.pair (g.code (), 0, false, id, sec, tok, kOwner) == kOk);	// at once: not even the second's gap
}

// The global backstop: however many addresses an attacker takes, wrong codes are bounded for everyone together, and
// it comes back by itself. A correct code never spends it.
static void test_backstop ()
{
	g_ms = 4000000;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	const char *wrong = strcmp (g.code (), "11111111") == 0 ? "22222222" : "11111111";
	for (uint32_t ip = 1; ip <= kGlobalBurst; ip++)		// one each from many addresses at once
	{
		g_ms += 1;
		CHECK (g.pair (wrong, 0, false, id, sec, tok, 0x0a000000u + ip) == kWrongCode);
	}
	g_ms += 1;
	CHECK (g.global_wait_s () > 0 && g.lock_left_s (kOwner) > 0);
	CHECK (g.pair (wrong, 0, false, id, sec, tok, 0x0b000001u) == kLocked);	// a fresh address gets nothing
	CHECK (g.pair (g.code (), 0, false, id, sec, tok, kOwner) == kLocked);	// nor the owner, while it is empty
	g_ms += kGlobalRefillMs;
	CHECK (g.global_wait_s () == 0);
	CHECK (g.pair (g.code (), 0, false, id, sec, tok, kOwner) == kOk);	// one back, and the right code spends nothing
	g_ms += 1000;
	CHECK (g.pair (g.code (), 0, false, id, sec, tok, kOwner) == kOk);
	// it refills to the burst and no further
	g_ms += 100u * kGlobalRefillMs;
	unsigned n = 0;
	for (uint32_t ip = 100; ip < 100 + 2 * kGlobalBurst; ip++)
		if (g.pair (wrong, 0, false, id, sec, tok, 0x0c000000u + ip) == kWrongCode) n++;
	CHECK (n == kGlobalBurst);
	// a resume never refills it (or an owner's reload would hand an attacker a new budget)
	CHECK (g.pair (g.code (), "x", true, id, sec, tok, kOwner) == kLocked);
	g_ms += kGlobalRefillMs;
	CHECK (g.pair (g.code (), "x", true, id, sec, tok, kOwner) == kOk);
	char ntok[kTokenHex + 1];
	g_ms += 1;
	for (uint32_t ip = 300; ip < 302; ip++) g.pair (wrong, 0, false, id + 0, sec + 0, ntok, 0x0d000000u + ip);
	CHECK (g.global_wait_s () > 0);
}

// The peer table is small; an attacker cycling addresses must not be able to push a locked peer out and start afresh
static void test_peer_table ()
{
	g_ms = 5000000;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	const char *wrong = strcmp (g.code (), "11111111") == 0 ? "22222222" : "11111111";
	for (unsigned i = 0; i < kMaxFails; i++) { g_ms += 1000; g.pair (wrong, 0, false, id, sec, tok, kMallory); }
	CHECK (g.lock_left_s (kMallory) > 0);
	// many other addresses, one try each (each one takes a peer entry, whatever the answer)
	for (uint32_t ip = 1; ip <= 3 * kPeers; ip++) { g_ms += 10; g.pair (wrong, 0, false, id, sec, tok, ip); }
	g_ms += kGlobalRefillMs;					// (the backstop has one back; Mallory's own lock still holds)
	CHECK (g.lock_left_s (kMallory) > 0);				// still locked: it was not pushed out
	CHECK (g.pair (g.code (), 0, false, id, sec, tok, kMallory) == kLocked);
	// every entry locked out: a new address is refused, not let in by evicting a locked one (review M1)
	g_ms = 7000000;
	Gate h = make ();
	// (the backstop paces them, so it takes a long run of guessing - 16 addresses escalating towards 16-minute locks -
	// before all of them are locked at the same moment; that is the moment the 17th address arrives)
	auto all_locked = [&] () { for (uint32_t ip = 1; ip <= kPeers; ip++) if (h.lock_left_s (ip) <= 1) return false; return true; };	// (1: only the second's gap)
	bool reached = false;
	for (unsigned step = 0; step < 3 * 3600 && !reached; step++)
	{
		g_ms += 1000;
		for (uint32_t ip = 1; ip <= kPeers; ip++) h.pair (wrong, 0, false, id, sec, tok, ip);
		reached = all_locked () && h.global_wait_s () == 0;
	}
	CHECK (reached);
	CHECK (h.pair (h.code (), 0, false, id, sec, tok, 999) == kLocked);	// refused
	for (uint32_t ip = 1; ip <= kPeers; ip++) CHECK (h.lock_left_s (ip) > 0);	// and nobody was reset
	g_ms += 20u * 60u * 1000u;					// the locks run out: the new address gets in
	CHECK (h.pair (h.code (), 0, false, id, sec, tok, 999) == kOk);
}

// The network's requests for the code on the Rytm (POST /pair/show): a fixed 10 s (it grew to 10 min before, and a
// headless Sidecar's button stopped working, device 2026-10-29); wait_s says how long is left; reset by a pairing
static void test_show_gap ()
{
	ShowGap g;
	uint32_t t = 1000;
	CHECK (g.wait_s (t) == 0 && g.take (t) && !g.take (t + 9));
	CHECK (g.wait_s (t) == 10 && g.wait_s (t + 3) == 7 && g.wait_s (t + 9) == 1 && g.wait_s (t + 10) == 0);
	for (unsigned i = 0; i < 20; i++) { t += kShowGapS; CHECK (!g.take (t - 1) && g.take (t)); }	// never grows
	g.reset ();
	CHECK (g.wait_s (t + 1) == 0 && g.take (t + 1) && !g.take (t + 10) && g.take (t + 11));
}

// SIDECAR MENU > WEB UI (the Rytm): NEW CODE - a fresh code, the paired devices and their sessions kept, the old code
// no longer pairs; not on a fixed code or with the gate off. And the web on / off kept in pair.txt ("web=0"), the
// clients still read with it, a file without it (or none) on, a full table of devices still fitting.
static void test_menu ()
{
	g_ms = 0;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1], old[kDigits + 1];
	CHECK (g.pair (g.code (), "iMac", true, id, sec, tok) == kOk);
	strcpy (old, g.code ());
	CHECK (g.new_code () && strlen (g.code ()) == kDigits && strcmp (g.code (), old) != 0);
	CHECK (g.check (tok) && g.clients () == 1);			// still paired, still in session
	char id2[kIdHex + 1], sec2[kSecretHex + 1], tok2[kTokenHex + 1];
	g_ms += 2000;
	CHECK (g.pair (old, "x", false, id2, sec2, tok2) != kOk);	// the old code is gone
	g_ms += 2000;
	CHECK (g.pair (g.code (), "iPad", true, id2, sec2, tok2) == kOk);
	Gate f = make (kFixed, "11223344");
	CHECK (!f.new_code () && strcmp (f.code (), "11223344") == 0);
	Gate o = make (kOff);
	CHECK (!o.new_code ());

	CHECK (g.web_on ());
	char text[kFileMax];
	g.saved ();
	g.set_web (false);
	CHECK (g.dirty () && !g.web_on ());
	CHECK (g.format (text, sizeof text) > 0 && strstr (text, "\nweb=0\n") && strstr (text, "c="));
	Gate b = make ();
	b.load (text);
	CHECK (!b.web_on () && b.clients () == 2);
	b.set_web (true);
	CHECK (b.format (text, sizeof text) > 0 && !strstr (text, "web="));
	Gate c = make ();
	c.load (text);
	CHECK (c.web_on () && c.clients () == 2);
	c.load ("");
	CHECK (c.web_on ());
	// a full table with web=0 still fits the file (kHeadMax has room for the line)
	Gate full = make ();
	for (unsigned i = 0; i < kClients; i++)
	{
		g_ms += 2000;
		char n[kNameMax + 1];
		memset (n, 'n', kNameMax), n[kNameMax] = 0;
		CHECK (full.pair (full.code (), n, true, id2, sec2, 0) == kOk);
	}
	full.set_web (false);
	CHECK (full.format (text, sizeof text) > 0);
	Gate r = make ();
	r.load (text);
	CHECK (r.clients () == kClients && !r.web_on ());
}

// The code at a Rytm's connect (paircode_pi::RytmHello): only while no device is paired, the gate on with a working
// RNG, at most once a minute
static void test_hello_show ()
{
	HelloShow h;
	CHECK (h.take (100, kOn, true, 0) && !h.take (101, kOn, true, 0) && !h.take (159, kOn, true, 0));
	CHECK (h.take (160, kOn, true, 0));
	CHECK (!h.take (300, kOn, true, 1));		// a device paired: a reconnect does not show it
	CHECK (!h.take (400, kOff, true, 0) && !h.take (500, kOn, false, 0));
	CHECK (h.take (600, kFixed, true, 0));
}

// pair.txt missing but pair.txt.prev there (a power cut between save()'s two renames): the devices come from .prev,
// marked dirty so the next save puts pair.txt back; neither: nothing, clean
static void test_load_prev ()
{
	char tmpl[] = "/tmp/paircode-prev-XXXXXX";
	CHECK (mkdtemp (tmpl));
	g_root = tmpl;
	CHECK (mkdir (mac ("/.rytm").c_str (), 0755) == 0);
	g_ms = 0;
	Gate g = make ();
	char id[kIdHex + 1], sec[kSecretHex + 1], tok[kTokenHex + 1];
	CHECK (g.pair (g.code (), "iMac", true, id, sec, tok) == kOk);
	g.set_web (false);
	CHECK (g.save (kFs, kPath));
	Gate n = make ();
	CHECK (n.load_card (kFs, kPath) == 1 && n.clients () == 1 && !n.dirty () && !n.web_on ());
	CHECK (f_rename (kPath, "/.rytm/pair.txt.prev"));	// the cut: only .prev left
	Gate p = make ();
	CHECK (p.load_card (kFs, kPath) == 2 && p.clients () == 1 && p.dirty () && !p.web_on ());
	char ntok[kTokenHex + 1];
	CHECK (p.resume (id, sec, ntok) == kOk);
	CHECK (p.save (kFs, kPath) && f_exists (kPath));
	p.saved ();
	CHECK (f_remove (kPath));
	f_remove ("/.rytm/pair.txt.prev");
	Gate e = make ();
	CHECK (e.load_card (kFs, kPath) == 0 && e.clients () == 0 && !e.dirty () && e.web_on ());
}

// A full table of remembered devices: the least recently used browser goes, and sdboot.py's record - used once in a
// while, for an install, and the way firmware is recovered - is the last to (audit 2026-10-26: the oldest-first rule
// always evicted it, because sdboot.py is usually paired first)
static void test_eviction ()
{
	g_ms = 6000000;
	Gate g = make ();
	char reply[400], line[64], tid[kIdHex + 1], tsec[kSecretHex + 1];
	snprintf (line, sizeof line, "PAIR %s", g.code ());
	CHECK (g.tcp_auth (line, reply, sizeof reply, kOwner));
	CHECK (sscanf (reply + 3, "%8s %32s", tid, tsec) == 2);
	CHECK (g.sessions () == 0);					// 7310 holds no session: the connection is the auth
	char id[kClients + 8][kIdHex + 1], sec[kClients + 8][kSecretHex + 1], tok[kTokenHex + 1];
	for (unsigned i = 0; i < kClients + 8; i++)
	{
		g_ms += 1000;
		CHECK (g.pair (g.code (), "MacIntel", true, id[i], sec[i], tok, kOwner) == kOk);
		if (i == 12) { g_ms += 1000; CHECK (g.resume (id[0], sec[0], tok, kOwner) == kOk); }	// browser 0 in use
	}
	CHECK (g.clients () == kClients);
	snprintf (line, sizeof line, "AUTH %s %s", tid, tsec);
	CHECK (g.tcp_auth (line, reply, sizeof reply, kOwner) && strcmp (reply, "OK") == 0);	// sdboot.py survived
	g_ms += 1000;
	CHECK (g.resume (id[0], sec[0], tok, kOwner) == kOk);			// the browser in use survived the first evictions
	CHECK (g.resume (id[1], sec[1], tok, kOwner) == kNoDevice);		// the oldest unused one went
	CHECK (g.resume (id[kClients + 7], sec[kClients + 7], tok, kOwner) == kOk);
	// after a reboot nothing is "used" yet: the file's order (oldest first) decides, and sdboot.py still stays
	char text[kFileMax];
	CHECK (g.format (text, sizeof text) > 0);
	Gate b = make ();
	b.load (text);
	char nid[kIdHex + 1], nsec[kSecretHex + 1];
	g_ms += 1000;
	CHECK (b.pair (b.code (), "new", true, nid, nsec, tok, kOwner) == kOk);
	g_ms += 1000;
	CHECK (b.tcp_auth (line, reply, sizeof reply, kOwner));
	// a table of only sdboot.py records still takes a new device (the least recently used of them goes)
	Gate t = make ();
	for (unsigned i = 0; i <= kClients; i++)
	{
		g_ms += 1000;
		snprintf (line, sizeof line, "PAIR %s", t.code ());
		CHECK (t.tcp_auth (line, reply, sizeof reply, kOwner));
	}
	CHECK (t.clients () == kClients);
}

int main ()
{
	test_uninit ();
	test_code ();
	test_cmdline ();
	test_pair ();
	test_limit ();
	test_sessions ();
	test_card ();
	test_quiet ();
	test_report ();
	test_stale_device ();
	test_tcp ();
	test_stuck_random ();
	test_per_peer ();
	test_malformed ();
	test_backstop ();
	test_peer_table ();
	test_eviction ();
	test_show_gap ();
	test_menu ();
	test_hello_show ();
	test_load_prev ();
	printf ("test_paircode: ok\n");
	return 0;
}
