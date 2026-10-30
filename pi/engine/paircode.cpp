// pi/engine/paircode.cpp - see paircode.h.
#include "paircode.h"

#include <string.h>

#include "lib/sha256.h"

namespace paircode {

static const char kHex[] = "0123456789abcdef";

bool ct_equal (const void *a, const void *b, unsigned n)
{
	const uint8_t *p = (const uint8_t *) a, *q = (const uint8_t *) b;
	uint8_t d = 0;
	for (unsigned i = 0; i < n; i++) d = (uint8_t) (d | (p[i] ^ q[i]));
	return d == 0;
}

// a NUL-terminated string against another, in constant time for its own length: the lengths are public (both are
// fixed-width fields here), the bytes are not
static bool ct_equal_str (const char *a, const char *b, unsigned n)
{
	if (strlen (a) != n || strlen (b) != n) return false;
	return ct_equal (a, b, n);
}

static bool is_digits (const char *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return false;
	return p[n] == 0;
}

static bool is_hex (const char *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
	{
		char c = p[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
	}
	return p[n] == 0;
}

static int hexval (char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void hexify (const uint8_t *p, unsigned n, char *out)
{
	for (unsigned i = 0; i < n; i++) { out[2 * i] = kHex[p[i] >> 4]; out[2 * i + 1] = kHex[p[i] & 15]; }
	out[2 * n] = 0;
}

static bool unhexify (const char *p, unsigned n, uint8_t *out)	// n bytes from 2n hex digits
{
	for (unsigned i = 0; i < n; i++)
	{
		int a = hexval (p[2 * i]), b = hexval (p[2 * i + 1]);
		if (a < 0 || b < 0) return false;
		out[i] = (uint8_t) (a << 4 | b);
	}
	return true;
}

// the label a client gives itself: printable ASCII, no leading or trailing blanks, at most kNameMax (anything else
// becomes '?', so a name can never carry a newline into pair.txt)
static void clean_name (const char *p, char *out)
{
	unsigned n = 0;
	for (; p && *p && n < kNameMax; p++)
	{
		unsigned char c = (unsigned char) *p;
		if (c == ' ' && n == 0) continue;
		out[n++] = c >= 0x20 && c <= 0x7e ? (char) c : '?';
	}
	while (n && out[n - 1] == ' ') n--;
	out[n] = 0;
	if (n == 0) strcpy (out, "a client");
}

const char *err_text (Result r)
{
	switch (r)
	{
	case kOk:		return "ok";
	case kWrongCode:	return "that is not the pairing code on the Sidecar's screen";
	case kLocked:		return "too many wrong codes: pairing is locked for a while (the code on the screen still works after it)";
	case kBadInput:		return "the pairing code is 8 digits";
	case kNoDevice:		return "the saved pairing is no longer valid on this Sidecar: the code is needed again";
	case kOffGate:		return "the pairing code is off on this card (cmdline.txt paircode=0)";
	case kNoRandom:		return "the Sidecar's random number generator failed its self-test, so it refuses to pair "
				       "(a weak code would be worse): restart it, and report it if this stays";
	}
	return "refused";
}

Mode mode_from_cmdline (const char *p, char *fixed)
{
	fixed[0] = 0;
	if (p == 0 || p[0] == 0) return kOn;
	if (strcmp (p, "0") == 0) return kOff;
	if (is_digits (p, kDigits)) { memcpy (fixed, p, kDigits); fixed[kDigits] = 0; return kFixed; }
	return kOn;		// anything else: as if it were not there (never weaker than the default)
}

// Sixteen words from the generator, no two alike: a stuck or dead one (the same word, or zeros) fails at once, and a
// working 32-bit one fails this by chance about once in 2^25 boots. It does not prove the output is unpredictable -
// nothing this cheap can - but it does catch the failure that would silently hand every client the same secrets.
static bool rng_self_test (void (*random) (uint8_t *, unsigned))
{
	uint8_t b[64];
	random (b, sizeof b);
	bool ok = true;
	for (unsigned i = 0; i < 16; i++)
		for (unsigned k = i + 1; k < 16; k++) if (memcmp (b + 4 * i, b + 4 * k, 4) == 0) ok = false;
	memset (b, 0, sizeof b);
	return ok;
}

void Gate::init (Mode m, const char *fixed, const Deps &d)
{
	m_D = d;
	m_Mode = m;
	memset (m_Session, 0, sizeof m_Session);
	memset (m_Peer, 0, sizeof m_Peer);
	m_nGlobal = kGlobalBurst;
	m_GlobalAt = d.ms ? d.ms () : 0;
	m_bRngBad = m != kOff && (d.random == 0 || !rng_self_test (d.random));
	if (m_bRngBad)			// no code at all rather than a weak one: nothing can pair (kNoRandom), the displays say why
	{
		memset (m_Code, '-', kDigits);
		m_Code[kDigits] = 0;
		return;
	}
	if (m == kFixed && fixed && is_digits (fixed, kDigits)) { memcpy (m_Code, fixed, kDigits); m_Code[kDigits] = 0; return; }
	// one digit per random byte, drawing again at 250 and above: every code equally likely (no modulo bias)
	draw_code ();
}

bool Gate::new_code ()
{
	if (m_Mode != kOn || m_bRngBad || m_D.random == 0) return false;
	draw_code ();
	return true;
}

void Gate::draw_code ()
{
	for (unsigned i = 0; i < kDigits; )
	{
		uint8_t b[kDigits];
		m_D.random (b, sizeof b);
		for (unsigned k = 0; k < sizeof b && i < kDigits; k++) if (b[k] < 250) m_Code[i++] = (char) ('0' + b[k] % 10);
	}
	m_Code[kDigits] = 0;
}

void Gate::grouped (char *out, unsigned size) const
{
	out[0] = 0;
	if (size < kDigits + 2) return;
	memcpy (out, m_Code, 4);
	out[4] = ' ';
	memcpy (out + 5, m_Code + 4, 4);
	out[9] = 0;
}

unsigned Gate::clients () const { return m_nClients; }

unsigned Gate::sessions () const
{
	unsigned n = 0;
	for (unsigned i = 0; i < kSessions; i++) if (m_Session[i].token[0]) n++;
	return n;
}

// ---- the card's file
void Gate::load (const char *text)
{
	m_nClients = 0;
	memset (m_Client, 0, sizeof m_Client);
	m_bWebOn = true;
	while (*text && m_nClients < kClients)
	{
		const char *e = strchr (text, '\n');
		unsigned n = e ? (unsigned) (e - text) : (unsigned) strlen (text);
		char line[160];
		unsigned m = n < sizeof line - 1 ? n : sizeof line - 1;
		memcpy (line, text, m);
		line[m] = 0;
		text += n + (e ? 1 : 0);
		while (m && (line[m - 1] == '\r' || line[m - 1] == ' ')) line[--m] = 0;
		if (strcmp (line, "web=0") == 0) m_bWebOn = false;	// (format() writes it before the clients)
		if (strncmp (line, "c=", 2) != 0) continue;
		// c=<8 hex id> <64 hex of SHA-256> <name>
		char *id = line + 2, *sha = strchr (id, ' ');
		if (sha == 0) continue;
		*sha++ = 0;
		char *name = strchr (sha, ' ');
		if (name == 0) continue;
		*name++ = 0;
		Client c;
		memset (&c, 0, sizeof c);
		if (!is_hex (id, kIdHex) || !is_hex (sha, 64) || !unhexify (sha, 32, c.hash)) continue;
		for (unsigned i = 0; i < kIdHex; i++) c.id[i] = (char) (id[i] >= 'A' && id[i] <= 'F' ? id[i] - 'A' + 'a' : id[i]);
		clean_name (name, c.name);
		bool dup = false;
		for (unsigned i = 0; i < m_nClients; i++) if (strcmp (m_Client[i].id, c.id) == 0) dup = true;
		if (!dup) m_Client[m_nClients++] = c;
		memset (line, 0, sizeof line);
	}
	m_bDirty = false;
}

static bool put (char *out, unsigned size, unsigned *n, const char *p)
{
	unsigned l = strlen (p);
	if (*n + l + 1 > size) return false;
	memcpy (out + *n, p, l + 1);
	*n += l;
	return true;
}

static const char kHead[] = "# Sidecar paired devices (docs/pi-engine.md \"Pairing code\"). The code itself is never\n"
			    "# stored here: these are SHA-256 of each device's own secret. Delete this file to unpair all.\n"
			    "v=1\n";
static_assert (sizeof kHead - 1 + 6 <= kHeadMax, "pair.txt's header outgrew kHeadMax: raise it, or a full card file "
					     "would not fit and saves would fail silently");

int Gate::format (char *out, unsigned size) const
{
	unsigned n = 0;
	if (size == 0) return -1;
	out[0] = 0;
	bool ok = put (out, size, &n, kHead) && (m_bWebOn || put (out, size, &n, "web=0\n"));
	for (unsigned i = 0; ok && i < m_nClients; i++)
	{
		char sha[65];
		hexify (m_Client[i].hash, 32, sha);
		ok = put (out, size, &n, "c=") && put (out, size, &n, m_Client[i].id) && put (out, size, &n, " ")
		     && put (out, size, &n, sha) && put (out, size, &n, " ") && put (out, size, &n, m_Client[i].name)
		     && put (out, size, &n, "\n");
	}
	if (!ok) { memset (out, 0, size); return -1; }
	return (int) n;
}

// as wifi_conf.cpp's save: path.part written, the current path kept as path.prev, then the rename. A failure leaves
// path as it was.
bool Gate::save (const wificonf::Fs &fs, const char *path) const
{
	char text[kFileMax], part[96], prev[96];
	if (strlen (path) + 6 > sizeof part) return false;
	strcpy (part, path); strcat (part, ".part");
	strcpy (prev, path); strcat (prev, ".prev");
	int n = format (text, sizeof text);
	bool ok = n > 0 && fs.write (part, text, (unsigned) n);
	memset (text, 0, sizeof text);
	if (!ok) { fs.remove (part); return false; }
	bool had = fs.exists (path);
	if (fs.exists (prev) && !fs.remove (prev)) { fs.remove (part); return false; }
	if (had && !fs.rename (path, prev)) { fs.remove (part); return false; }
	if (fs.rename (part, path)) return true;
	if (had) fs.rename (prev, path);		// back as it was
	fs.remove (part);
	return false;
}

int Gate::load_card (const wificonf::Fs &fs, const char *path)
{
	char text[kFileMax > 1024 ? kFileMax : 1024], prev[96];
	int from = 0;
	if (fs.read (path, text, sizeof text)) from = 1;
	else if (strlen (path) + 6 <= sizeof prev && !fs.exists (path))
	{
		unsigned l = strlen (path);		// (not strcpy + strcat: the compiler makes that stpcpy, which the Pi lacks)
		memcpy (prev, path, l);
		memcpy (prev + l, ".prev", 6);
		if (fs.read (prev, text, sizeof text)) from = 2;
	}
	if (from) load (text);
	if (from == 2) m_bDirty = true;
	memset (text, 0, sizeof text);
	return from;
}

// The only file that may hold the code, and only when there is no display to show it on. It says so, in the file.
int Gate::format_recourse (char *out, unsigned size) const
{
	char g[16];
	unsigned n = 0;
	if (size == 0) return -1;
	out[0] = 0;
	grouped (g, sizeof g);
	bool ok = put (out, size, &n, "Sidecar pairing code: ") && put (out, size, &n, g)
		  && put (out, size, &n, "\n\n"
			 "# This Sidecar has no display, so there is nowhere to show you the code; this file is how you\n"
			 "# read it. Enter it once in the Sidecar's web page (http://sidecar.local/), or give it to\n"
			 "# pi/circle/sdboot.py as SIDECAR_PAIR_CODE. It changes at every boot and this file is written\n"
			 "# again then; a boot with a display attached deletes it.\n"
			 "# Nothing on the network can read this file: every read of the card needs the code itself\n"
			 "# (docs/pi-engine.md \"Pairing code\"). Whoever holds the card can read it, which is the\n"
			 "# deliberate limit of this scheme - see \"Threat model\" there.\n"
			 "# A unit that should never write it down takes a fixed code instead: cmdline.txt paircode=<8 digits>.\n");
	if (!ok) { memset (out, 0, size); return -1; }
	return (int) n;
}

// ---- sessions
void Gate::new_token (char *out)
{
	uint8_t b[16];
	m_D.random (b, sizeof b);
	hexify (b, sizeof b, out);
	memset (b, 0, sizeof b);
}

void Gate::expire ()
{
	if (m_D.ms == 0) return;
	uint32_t now = m_D.ms ();
	for (unsigned i = 0; i < kSessions; i++)
	{
		Session &s = m_Session[i];
		if (s.token[0] && (now - s.seen >= kIdleMs || now - s.born >= kLifeMs)) memset (&s, 0, sizeof s);
	}
}

Session *Gate::free_session (uint32_t now)
{
	expire ();
	Session *pOld = &m_Session[0];
	for (unsigned i = 0; i < kSessions; i++)
	{
		if (m_Session[i].token[0] == 0) return &m_Session[i];
		if (now - m_Session[i].seen > now - pOld->seen) pOld = &m_Session[i];	// the least recently used goes
	}
	memset (pOld, 0, sizeof *pOld);
	return pOld;
}

Session *Gate::find (const char *token)
{
	Session *pHit = 0;
	for (unsigned i = 0; i < kSessions; i++)		// every slot looked at: no early exit on the token's bytes
		if (m_Session[i].token[0] && ct_equal_str (m_Session[i].token, token, kTokenHex)) pHit = &m_Session[i];
	return pHit;
}

bool Gate::check_quiet (const char *token) const
{
	if (m_D.ms == 0) return false;
	if (m_Mode == kOff) return true;
	if (token == 0 || strlen (token) != kTokenHex) return false;
	uint32_t now = m_D.ms ();
	bool hit = false;
	for (unsigned i = 0; i < kSessions; i++)		// every slot looked at: no early exit on the token's bytes
	{
		const Session &s = m_Session[i];
		if (s.token[0] && now - s.seen < kIdleMs && now - s.born < kLifeMs && ct_equal_str (s.token, token, kTokenHex))
			hit = true;
	}
	return hit;
}

bool Gate::check (const char *token)
{
	if (m_D.ms == 0) return false;		// before init (a request cannot arrive then): refuse, never fall open
	if (m_Mode == kOff) return true;
	if (token == 0 || strlen (token) != kTokenHex) return false;
	expire ();
	Session *s = find (token);
	if (s == 0) return false;
	s->seen = m_D.ms ();
	return true;
}

// ---- the limiter: per peer, with a global backstop
const Gate::Peer *Gate::peer_find (uint32_t ip) const
{
	for (unsigned i = 0; i < kPeers; i++) if (m_Peer[i].live && m_Peer[i].ip == ip) return &m_Peer[i];
	return 0;
}

unsigned Gate::peer_wait_ms (const Peer &p, uint32_t now) const
{
	if (p.lock_until != 0 && (int32_t) (p.lock_until - now) > 0) return p.lock_until - now;
	if (p.tried && now - p.last < kGapMs) return kGapMs - (now - p.last);
	return 0;
}

// A new peer takes a free entry, else the one seen longest ago that is not locked out. When every entry is locked out
// there is none: the new address is refused (0, kLocked) rather than evicting a locked one, which would have started
// it afresh and let 17 addresses walk past the per-peer limit (review M1). The global backstop holds either way.
Gate::Peer *Gate::peer_of (uint32_t ip, uint32_t now)
{
	Peer *p = (Peer *) peer_find (ip);
	if (p == 0)
	{
		Peer *pFree = 0, *pIdle = 0;
		for (unsigned i = 0; i < kPeers; i++)
		{
			Peer &q = m_Peer[i];
			if (!q.live) { if (!pFree) pFree = &q; continue; }
			bool locked = q.lock_until != 0 && (int32_t) (q.lock_until - now) > 0;
			if (!locked && (pIdle == 0 || now - q.seen > now - pIdle->seen)) pIdle = &q;
		}
		p = pFree ? pFree : pIdle;
		if (p == 0) return 0;
		memset (p, 0, sizeof *p);
		p->ip = ip;
		p->live = true;
	}
	p->seen = now;
	return p;
}

unsigned Gate::global_level (uint32_t now) const
{
	unsigned n = m_nGlobal + (now - m_GlobalAt) / kGlobalRefillMs;
	return n < kGlobalBurst ? n : kGlobalBurst;
}

void Gate::global_spend (uint32_t now)
{
	unsigned add = (now - m_GlobalAt) / kGlobalRefillMs;
	if (m_nGlobal + add >= kGlobalBurst) { m_nGlobal = kGlobalBurst; m_GlobalAt = now; }
	else { m_nGlobal += add; m_GlobalAt += add * kGlobalRefillMs; }
	if (m_nGlobal) m_nGlobal--;
}

unsigned Gate::global_wait_s () const
{
	if (m_D.ms == 0) return 0;
	uint32_t now = m_D.ms ();
	if (global_level (now) != 0) return 0;
	unsigned ms = kGlobalRefillMs - (now - m_GlobalAt) % kGlobalRefillMs;
	return (ms + 999) / 1000;
}

unsigned Gate::lock_left_s (uint32_t peer) const
{
	if (m_D.ms == 0) return 0;
	uint32_t now = m_D.ms ();
	const Peer *p = peer_find (peer);
	unsigned s = p ? (peer_wait_ms (*p, now) + 999) / 1000 : 0, g = global_wait_s ();
	return s > g ? s : g;
}

// At most one line every 10 s, with how many refusals it covers
bool Gate::take_report (unsigned *nSince)
{
	if (m_D.ms == 0 || m_nRefused == m_nReported) return false;
	uint32_t now = m_D.ms ();
	if (m_ReportAt != 0 && now - m_ReportAt < 10000) return false;
	*nSince = m_nRefused - m_nReported;
	m_nReported = m_nRefused;
	m_ReportAt = now | 1;
	return true;
}

void Gate::count_fail (Peer &p, uint32_t now)
{
	global_spend (now);
	p.last = now;
	p.tried = true;
	if (++p.fails < kMaxFails) return;
	p.fails = 0;
	unsigned shift = p.locks < kLockShiftMax ? p.locks : kLockShiftMax;
	p.lock_until = now + (kLockMs << shift);
	if (p.lock_until == 0) p.lock_until = 1;
	if (p.locks < kLockShiftMax) p.locks++;
}

// The remembered client a full table gives up: the least recently used browser, never sdboot.py's record while a
// browser's is there to go instead (it is used for an install once in a while, so plain LRU would always pick it,
// and that is how firmware is recovered - audit 2026-10-26). Among equals the oldest in the file, which is the order
// they paired in. Not used this run (0) is older than anything used.
unsigned Gate::victim () const
{
	unsigned v = 0;
	bool vTool = true;
	for (unsigned i = 0; i < m_nClients; i++)
	{
		bool tool = strcmp (m_Client[i].name, kToolName) == 0;
		if (i == 0 || (vTool && !tool) || (vTool == tool && m_Client[i].used < m_Client[v].used)) { v = i; vTool = tool; }
	}
	return v;
}

// ---- pairing
Result Gate::pair (const char *code, const char *name, bool bRemember, char *id, char *secret, char *token, uint32_t peer)
{
	if (m_D.ms == 0) return kLocked;
	if (m_Mode == kOff) return kOffGate;
	if (m_bRngBad) return kNoRandom;
	uint32_t now = m_D.ms ();
	// Counted before anything else: an attempt turned away by the limiter is still an attempt, and take_report is
	// what keeps a flood of them out of the log. It does not extend the lockout - that would let anyone keep the
	// owner locked out for good - it only makes the flood visible, once every 10 s.
	m_nRefused++;
	// a malformed code is not a guess (it cannot match): refused, never counted, and it needs no peer entry
	if (code == 0 || !is_digits (code, kDigits)) return kBadInput;
	Peer *p = peer_of (peer, now);
	if (p == 0 || peer_wait_ms (*p, now) != 0 || global_level (now) == 0) return kLocked;
	if (!ct_equal (code, m_Code, kDigits)) { count_fail (*p, now); return kWrongCode; }
	m_nRefused--;					// it was the right code: not a refusal after all
	p->fails = p->locks = 0;			// this peer's slate, nobody else's
	p->lock_until = 0;
	p->tried = false;
	Session *s = 0;
	if (token)
	{
		s = free_session (now);
		new_token (s->token);
		s->born = s->seen = now;
		s->id[0] = 0;
	}
	if (bRemember)
	{
		uint8_t sec[16], idb[4];
		char idhex[kIdHex + 1];
		for (unsigned t = 0; ; t++)			// an id of its own (a collision is drawn again)
		{
			m_D.random (idb, sizeof idb);
			hexify (idb, sizeof idb, idhex);
			bool clash = false;
			for (unsigned i = 0; i < m_nClients; i++) if (strcmp (m_Client[i].id, idhex) == 0) clash = true;
			if (!clash || t > 8) break;
		}
		if (m_nClients == kClients) forget_client (victim ());	// a full table: see victim()
		m_D.random (sec, sizeof sec);
		Client &c = m_Client[m_nClients++];
		memset (&c, 0, sizeof c);
		strcpy (c.id, idhex);
		core::lib::sha256 (sec, sizeof sec, c.hash);
		clean_name (name, c.name);
		c.used = now | 1;
		hexify (sec, sizeof sec, secret);
		strcpy (id, c.id);
		if (s) strcpy (s->id, c.id);
		memset (sec, 0, sizeof sec);
		m_bDirty = true;
	}
	else { id[0] = 0; secret[0] = 0; }
	if (token) strcpy (token, s->token);
	return kOk;
}

// A device that was remembered comes back with its id and secret. Deliberately outside the code's lockout, in both
// directions: a failure here never counts (see paircode.h), and a lockout never blocks it.
Result Gate::resume (const char *id, const char *secret, char *token, uint32_t peer)
{
	if (m_D.ms == 0) return kNoDevice;
	if (m_Mode == kOff) return kOffGate;
	if (m_bRngBad) return kNoRandom;
	uint32_t now = m_D.ms ();
	m_nRefused++;			// reported, throttled (take_report); it does not lock anything
	if (id == 0 || secret == 0 || !is_hex (id, kIdHex) || !is_hex (secret, kSecretHex)) return kNoDevice;
	uint8_t sec[16], hash[32];
	if (!unhexify (secret, 16, sec)) return kNoDevice;
	core::lib::sha256 (sec, sizeof sec, hash);
	memset (sec, 0, sizeof sec);
	int hit = -1;
	for (unsigned i = 0; i < m_nClients; i++)		// every record looked at
		if (strcmp (m_Client[i].id, id) == 0 && ct_equal (m_Client[i].hash, hash, 32)) hit = (int) i;
	memset (hash, 0, sizeof hash);
	if (hit < 0) return kNoDevice;			// well formed, but not one this Sidecar knows (revoked, a new card)
	m_nRefused--;					// the secret was right
	m_Client[hit].used = now | 1;
	// Only the owner can produce a valid secret, so this lifts the lockout of the address it came from - that one
	// only: the others' escalation and the global backstop stay as they are (a reload is not a reset for an attacker)
	Peer *p = (Peer *) peer_find (peer);
	if (p) { p->fails = p->locks = 0; p->lock_until = 0; p->tried = false; }
	if (token == 0) return kOk;
	Session *s = free_session (now);
	new_token (s->token);
	s->born = s->seen = now;
	strcpy (s->id, m_Client[hit].id);
	strcpy (token, s->token);
	return kOk;
}

void Gate::forget_client (unsigned i)
{
	if (i >= m_nClients) return;
	for (unsigned k = 0; k < kSessions; k++)
		if (m_Session[k].id[0] && strcmp (m_Session[k].id, m_Client[i].id) == 0) memset (&m_Session[k], 0, sizeof m_Session[k]);
	for (unsigned k = i + 1; k < m_nClients; k++) m_Client[k - 1] = m_Client[k];
	memset (&m_Client[--m_nClients], 0, sizeof m_Client[0]);
	m_bDirty = true;
}

void Gate::forget (const char *token)
{
	Session *s = token ? find (token) : 0;
	if (s == 0) return;
	char id[kIdHex + 1];
	strcpy (id, s->id);
	memset (s, 0, sizeof *s);
	for (unsigned i = 0; i < m_nClients; i++) if (strcmp (m_Client[i].id, id) == 0) { forget_client (i); return; }
}

void Gate::revoke_all ()
{
	memset (m_Session, 0, sizeof m_Session);
	memset (m_Client, 0, sizeof m_Client);
	m_nClients = 0;
	m_bDirty = true;
}

bool HelloShow::take (uint32_t now_s, Mode m, bool rng_ok, unsigned clients)
{
	if (m == kOff || !rng_ok || clients > 0) return false;
	if (m_bAny && now_s - m_At < kHelloGapS) return false;
	m_bAny = true;
	m_At = now_s;
	return true;
}

unsigned ShowGap::wait_s (uint32_t now_s) const
{
	return m_bAny && now_s - m_At < kShowGapS ? kShowGapS - (now_s - m_At) : 0;
}

bool ShowGap::take (uint32_t now_s)
{
	if (m_bAny && now_s - m_At < kShowGapS) return false;
	m_bAny = true;
	m_At = now_s;
	return true;
}

// ---- the SD file port's first line (sdfiles.cpp)
bool Gate::tcp_auth (const char *line, char *reply, unsigned size, uint32_t peer)
{
	char id[kIdHex + 1], secret[kSecretHex + 1];
	unsigned n = 0;
	if (strncmp (line, "PAIR ", 5) == 0)
	{
		Result r = pair (line + 5, kToolName, true, id, secret, 0, peer);
		if (r == kOk)
		{
			put (reply, size, &n, "OK ") && put (reply, size, &n, id) && put (reply, size, &n, " ")
				&& put (reply, size, &n, secret);
			memset (secret, 0, sizeof secret);
			return true;
		}
		n = 0;
		put (reply, size, &n, "ERR ") && put (reply, size, &n, err_text (r));
		return false;
	}
	if (strncmp (line, "AUTH ", 5) == 0)
	{
		const char *p = line + 5, *sp = strchr (p, ' ');
		Result r = kBadInput;
		if (sp != 0 && (unsigned) (sp - p) == kIdHex && strlen (sp + 1) == kSecretHex)
		{
			memcpy (id, p, kIdHex);
			id[kIdHex] = 0;
			memcpy (secret, sp + 1, kSecretHex);
			secret[kSecretHex] = 0;
			r = resume (id, secret, 0, peer);
			memset (secret, 0, sizeof secret);
		}
		if (r == kOk) { put (reply, size, &n, "OK"); return true; }
		put (reply, size, &n, "ERR ") && put (reply, size, &n, r == kBadInput ? "AUTH <id> <secret>" : err_text (r));
		return false;
	}
	put (reply, size, &n, "ERR unauthenticated: PAIR <code> with the code on the Sidecar's screen, or AUTH <id> <secret> "
			      "(sdboot.py does this: SIDECAR_PAIR_CODE=<code>)");
	return false;
}

}
