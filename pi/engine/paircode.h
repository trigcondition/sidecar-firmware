// pi/engine/paircode.h - the pairing code that gates the network (docs/pi-engine.md "Pairing code"): the portable part
// (kernel.cpp and web.cpp on the Pi; pi/engine/tests/test_paircode.cpp on the Mac). No Circle, no allocation, no printf.
//
// WHAT IT IS. The device makes an 8-digit code at every boot and shows it on the Rytm's screen (a host-drawn popup) and
// on the touchscreen's STATUS page. A client enters it once; it gets a session token for this run and, if it asks to be
// remembered, a client id and a 128-bit secret it keeps. The card holds only SHA-256 of that secret, so a stolen card
// gives no client's secret away (it does give the card, which is the whole device: see the docs).
//
// WHAT CROSSES THE WIRE. The code: once, in the body of POST /pair (or the 7310 PAIR line). After that a token (every
// gated request) and, once per run, the secret (POST /pair/resume, or the 7310 AUTH line). The transport is plain HTTP
// on a LAN: anyone sniffing it can take the token or the secret and use them. This gate stops the person on the network
// who has not seen the screen; it is not confidentiality.
//
// WHAT IS NEVER WRITTEN. The code goes into no log line, no /status, no /log and no answer of any endpoint. err_text()
// never quotes the input. The one file that may hold it is the card-root recourse file below, written only when the
// unit has no display at all - and every read of the card needs the code, so it is not reachable over the network.
//
// IT DOES NOT PERSIST. A new code is drawn at every boot and shown on the displays; nothing stores it. A remembered
// client does not need it again - it resumes with its own secret - so there is nothing to keep.
//
// THE CODE IS NEVER COMPARED BYTE BY BYTE WITH AN EARLY EXIT (ct_equal), and a wrong one is rate-limited PER PEER (the
// client's IPv4 address, on the web and on 7310 alike): at most one attempt a second, five wrong ones lock that peer for
// 1, 2, 4, 8, then 16 minutes (a correct one from it clears it). So someone else on a shared Wi-Fi guessing locks
// THEMSELVES out, not the owner (audit 2026-10-26: one limiter for everyone let anyone keep pairing locked for good).
// Behind it a GLOBAL BACKSTOP bounds the guessing however many addresses an attacker takes: a bucket of kGlobalBurst
// wrong codes for everyone together, one back every kGlobalRefillMs. Only wrong codes spend it - a malformed one, a
// resume, a correct code never do - and an attacker's own peers are locked while it refills, so draining it for long
// takes dozens of addresses at once (docs/pi-engine.md "Rate limiting" has the numbers).
//
// A MALFORMED CODE IS NOT A GUESS: it is refused (kBadInput) without counting - it cannot match, so it protects
// nothing to count it, and a mistyped 7-digit code or a bare POST /pair must not move the owner towards a lockout.
//
// ONLY pair() COUNTS TOWARDS THAT LOCKOUT. The lockout exists to stop someone guessing eight digits, and resume() is
// the only other way in: it takes a 128-bit secret, which is not guessable, so rate-limiting it protects nothing and
// costs a great deal. Counting it meant a browser holding a stale entry locked its own owner out by reloading five
// times - and the lockout is shared with port 7310, so that also blocked `sdboot.py install`, the way firmware is
// recovered (hardware, 2026-10-25). resume() therefore neither counts a failure nor waits out a lockout; a successful
// one does clear a lockout, because only the owner can produce a valid secret, which is a way back in when someone on
// the network is holding pairing locked.
// A resume clears ITS OWN PEER's lockout and escalation only: before, it reset everyone's, so the owner's browsers
// resuming on every reload kept an attacker's lockouts at one minute instead of sixteen (audit 2026-10-26). It never
// refills the global backstop, for the same reason.
//
// THE RANDOM GENERATOR IS TESTED BEFORE IT IS TRUSTED. init() draws from it and refuses to run a usable gate when it
// repeats itself (fail_rng() lets the Pi side add its own test of the raw hardware generator): every pairing and every
// resume then answers kNoRandom, the displays say RNG FAILED, and the log says why. A loud refusal, never a weak code -
// the same generator makes every session token and client secret.
#ifndef _pi_engine_paircode_h
#define _pi_engine_paircode_h

#include <stdint.h>

#include "wifi_conf.h"		// wificonf::Fs: the card, without its drive

namespace paircode {

constexpr unsigned kDigits = 8;			// the code: 8 decimal digits, shown as "1234 5678"
constexpr unsigned kIdHex = 8;			// a remembered client's id (32 bits, plain: not a secret)
constexpr unsigned kSecretHex = 32;		// its secret (128 bits), held by the client, hashed on the card
constexpr unsigned kTokenHex = 32;		// a session token (128 bits), this run only
constexpr unsigned kNameMax = 24;		// the label a client gives itself ("Studio iMac")
constexpr unsigned kClients = 16;		// remembered clients kept (when a 17th pairs, the least recently used browser goes)
// pair.txt: the fixed header and one "c=<id> <64 hex> <name>\n" a client. format() checks this at compile time, so a
// longer header can never make a save fail silently and stop pairings surviving a reboot.
constexpr unsigned kHeadMax = 256, kLineMax = 2 + kIdHex + 1 + 64 + 1 + kNameMax + 1;
constexpr unsigned kFileMax = kHeadMax + kClients * kLineMax + 1;
constexpr unsigned kSessions = 16;		// sessions at once (the least recently used goes)

constexpr const char *kPath = "/.rytm/pair.txt";		// the remembered clients (as /.rytm/wifi.txt is written)
// The recourse file at the card root, written ONLY when there is genuinely no display to show the code on. Every
// read of the card is behind the gate (web.cpp's Open()), so it is not reachable over the network; it is readable by
// whoever holds the card, which is the documented and deliberate limit of this scheme (docs/pi-engine.md "Pairing
// code" > "Threat model"). It is deleted again as soon as a unit boots with a display.
constexpr const char *kRecoursePath = "/sidecar-paircode.txt";

// cmdline.txt paircode=: absent the code is random and the gate is on; "0" the gate is off (a dev card: loud in the
// log, in /status and on the STATUS page); 8 digits a fixed code (still gated).
enum Mode { kOn, kOff, kFixed };
Mode mode_from_cmdline (const char *p, char *fixed);	// fixed: kDigits+1 bytes, "" unless kFixed

// per peer: one attempt a second; five wrong ones lock that peer, 1 minute doubling to 16
constexpr unsigned kGapMs = 1000, kMaxFails = 5, kLockMs = 60u * 1000u, kLockShiftMax = 4;
constexpr unsigned kPeers = 16;			// peers tracked (the stalest unlocked one goes; all locked: refused)
// the global backstop: kGlobalBurst wrong codes for everyone together, one more every kGlobalRefillMs (6 a minute)
constexpr unsigned kGlobalBurst = 30, kGlobalRefillMs = 10u * 1000u;
// the client name sdboot.py's PAIR line gets (tcp_auth): such a record is the last a full table gives up, because
// sdboot.py is used once in a while (an install) and is how firmware is recovered, while browsers resume every day
constexpr const char *kToolName = "sdboot.py";
// a session ends after an hour with no gated request, and after a day whatever happens (the page resumes by itself
// with its stored secret, so the owner does not retype the code)
constexpr unsigned kIdleMs = 60u * 60u * 1000u, kLifeMs = 24u * 60u * 60u * 1000u;

struct Deps
{
	void (*random) (uint8_t *out, unsigned n);	// the hardware RNG, mixed with the timer (kernel.cpp)
	uint32_t (*ms) (void);				// milliseconds since boot (u32: wraps after 49.7 days, docs "For 0.3.1" L4)
};

struct Client
{
	char id[kIdHex + 1];
	uint8_t hash[32];				// SHA-256 of the 16 secret bytes
	char name[kNameMax + 1];
	uint32_t used;					// this run's last pair/resume (ms | 1), 0 not yet: the eviction order
};

struct Session
{
	char token[kTokenHex + 1];			// "" free
	char id[kIdHex + 1];				// the remembered client, "" a session that is not remembered
	uint32_t born, seen;
};

// kBadInput is only ever about the 8-digit code, so its text can name it. A resume that fails - malformed stored
// values, or a device this Sidecar does not know - is kNoDevice: it is this browser's or this tool's own saved
// state going stale, not someone guessing, and saying "the pairing code is 8 digits" at it sent a user hunting for
// a typo in a code they had entered correctly (hardware, 2026-10-25).
enum Result { kOk, kWrongCode, kLocked, kBadInput, kNoDevice, kOffGate, kNoRandom };
const char *err_text (Result r);			// for the page and the 7310 reply; never quotes the input

struct Gate
{
	// ---- setup (kernel.cpp, once at boot)
	void init (Mode m, const char *fixed, const Deps &d);
	Mode mode () const			{ return m_Mode; }
	bool gated () const			{ return m_Mode != kOff; }
	bool rng_ok () const			{ return !m_bRngBad; }	// false: every pairing and resume is refused
	void fail_rng ()			{ m_bRngBad = true; }	// the Pi side's own test of the raw generator failed
	const char *code () const		{ return m_Code; }	// THE DISPLAYS ONLY: never a log line, never /status
	void grouped (char *out, unsigned size) const;			// "1234 5678"
	// A fresh random code (the Rytm's SIDECAR MENU > WEB UI > NEW CODE). The remembered devices and their sessions
	// stay; only the code a new device types changes. false: not on a fixed code (cmdline.txt), the gate off, or no RNG.
	bool new_code ();
	// The web page and port 7310's network side on or off (the Rytm's SIDECAR MENU > WEB UI), kept in pair.txt as
	// "web=0" (default on: a file without the line, or no file). The serial console's :sdf is not affected.
	bool web_on () const			{ return m_bWebOn; }
	void set_web (bool on)			{ if (on != m_bWebOn) m_bWebOn = on, m_bDirty = true; }

	// ---- the remembered clients (the card)
	void load (const char *text);					// pair.txt; junk lines are ignored
	int format (char *out, unsigned size) const;			// pair.txt's text, its length; -1 does not fit
	bool save (const wificonf::Fs &fs, const char *path) const;	// path.part written, then it replaces path (path.prev kept)
	// path read and loaded; when it is missing, path.prev (a power cut between save()'s two renames leaves only that):
	// loaded and marked dirty, so the next save puts path back. 0 neither, 1 path, 2 path.prev.
	int load_card (const wificonf::Fs &fs, const char *path);
	unsigned clients () const;
	bool dirty () const			{ return m_bDirty; }	// a client was added or removed: save it
	void saved ()				{ m_bDirty = false; }

	// ---- pairing. id/secret/token are filled only on kOk; id and secret only when bRemember.
	// name may be 0. On kOk the client is remembered (bRemember) and, unless token is 0, holds a session (7310 passes
	// 0: the connection itself is what is authenticated, and a session there would only push a browser's out).
	// peer: the client's IPv4 address (the rate limiter's key; 0 the serial console or a test).
	Result pair (const char *code, const char *name, bool bRemember, char *id, char *secret, char *token, uint32_t peer = 0);
	Result resume (const char *id, const char *secret, char *token, uint32_t peer = 0);	// a remembered client
	bool check (const char *token);					// a live session? (and it is used now). Gate off: always true
	bool check_quiet (const char *token) const;			// the same, without counting it as use (GET /pair/status)
	void forget (const char *token);				// that client's record and every session of it
	void revoke_all ();						// every record and every session
	unsigned lock_left_s (uint32_t peer = 0) const;			// 0: that peer may try now (the page shows the wait)
	unsigned global_wait_s () const;				// the backstop's own wait (/status), 0 most of the time
	// Every refused attempt is counted; this hands out at most one report every 10 s, with how many came since. The
	// caller logs that one line instead of one a request, so an unauthenticated flood cannot push the log's tail out
	// (the reasoning of sdfiles.cpp's m_bToldUnpaired). *nSince: the refusals this report covers.
	bool take_report (unsigned *nSince);

	// The card-root recourse file's text, for a unit with no display at all (paircode_pi.cpp decides). Its length,
	// or -1 if it does not fit.
	int format_recourse (char *out, unsigned size) const;
	unsigned sessions () const;
	void expire ();							// idle / old sessions dropped (any time)

	// ---- the SD file port and the serial console (sdfiles.cpp): one line, PAIR <code> or AUTH <id> <secret>.
	// true: the connection is authenticated from now on. reply is one line, never an existing client's secret.
	bool tcp_auth (const char *line, char *reply, unsigned size, uint32_t peer = 0);

private:
	struct Peer
	{
		uint32_t ip, seen, last, lock_until;
		uint8_t fails, locks;
		bool tried, live;
	};
	Peer *peer_of (uint32_t ip, uint32_t now);	// its entry, made if new (the stalest unlocked one goes); 0 none
	const Peer *peer_find (uint32_t ip) const;
	unsigned peer_wait_ms (const Peer &p, uint32_t now) const;
	unsigned global_level (uint32_t now) const;
	void global_spend (uint32_t now);
	unsigned victim () const;			// the remembered client a full table gives up
	Session *free_session (uint32_t now);
	Session *find (const char *token);
	void forget_client (unsigned i);		// its record and its sessions
	void new_token (char *out);
	void draw_code ();				// m_Code: one digit per random byte below 250 (no modulo bias)
	void count_fail (Peer &p, uint32_t now);
	Mode m_Mode = kOn;
	Deps m_D = {0, 0};
	char m_Code[kDigits + 1] = {0};
	Client m_Client[kClients] = {};
	unsigned m_nClients = 0;
	Session m_Session[kSessions] = {};
	Peer m_Peer[kPeers] = {};
	unsigned m_nRefused = 0, m_nReported = 0;
	unsigned m_nGlobal = kGlobalBurst;		// the backstop's bucket, as of m_GlobalAt
	uint32_t m_GlobalAt = 0, m_ReportAt = 0;
	bool m_bDirty = false, m_bRngBad = false, m_bWebOn = true;
};

bool ct_equal (const void *a, const void *b, unsigned n);	// n bytes, no early exit, no branch on the data

// The gap between two code modals that the network asked for (POST /pair/show, open to anyone): a fixed 10 s. It
// used to grow (30 s doubling to 10 min, review L1) and refused silently, so on a Sidecar with no touchscreen - whose
// only screen is the Rytm's - the button stopped working (device 2026-10-29). Anyone on the network can now put the
// box up every 10 s: acceptable, since it only shows the code to whoever stands at the Rytm, which is physical access
// (docs/pi-engine.md "Pairing code" > "Threat model"), and a key takes it off. reset(): a device paired, `pair=show`.
constexpr unsigned kShowGapS = 10;
// paircode_pi::RytmHello's rule: the code on the Rytm at its connect (a hello) while the gate is on and no device is
// paired yet, at most once a minute (the Rytm says hello again 1 s and 2 s later, and after a silence).
constexpr unsigned kHelloGapS = 60;
struct HelloShow
{
	bool take (uint32_t now_s, Mode m, bool rng_ok, unsigned clients);
private:
	uint32_t m_At = 0;
	bool m_bAny = false;
};
struct ShowGap
{
	bool take (uint32_t now_s);			// true: show it now
	unsigned wait_s (uint32_t now_s) const;		// seconds until take() would say yes (0: now)
	void reset ()				{ m_bAny = false; }
private:
	uint32_t m_At = 0;				// the last one shown (seconds)
	bool m_bAny = false;
};

}

#endif
