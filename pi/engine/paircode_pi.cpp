// pi/engine/paircode_pi.cpp - see paircode_pi.h.
#include "paircode_pi.h"
#include "midibridge_pi.h"

#include <circle/bcmrandom.h>
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>

static const char From[] = "pair";

namespace paircode_pi {

// ---- the card for paircode (paths without the drive), as wifi.cpp's
static void Sd (char *pOut, unsigned nSize, const char *p) { strcpy (pOut, "SD:"); strncpy (pOut + 3, p, nSize - 4); pOut[nSize - 1] = 0; }
static bool FsWrite (const char *p, const char *pData, unsigned n)
{
	char s[96];
	Sd (s, sizeof s, p);
	FIL f;
	if (f_open (&f, s, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
	UINT nDone = 0;
	FRESULT r = f_write (&f, pData, n, &nDone);
	FRESULT s2 = f_sync (&f);
	f_close (&f);
	return r == FR_OK && s2 == FR_OK && nDone == n;
}
static bool FsRename (const char *a, const char *b) { char s[96]; Sd (s, sizeof s, a); return f_rename (s, b) == FR_OK; }
static bool FsRemove (const char *p) { char s[96]; Sd (s, sizeof s, p); return f_unlink (s) == FR_OK; }
static bool FsExists (const char *p) { char s[96]; Sd (s, sizeof s, p); FILINFO fi; return f_stat (s, &fi) == FR_OK; }
static bool FsRead (const char *p, char *pOut, unsigned nSize)
{
	char s[96];
	Sd (s, sizeof s, p);
	FIL f;
	if (f_open (&f, s, FA_READ) != FR_OK) return false;
	UINT n = 0;
	FRESULT r = f_read (&f, pOut, nSize - 1, &n);
	f_close (&f);
	pOut[r == FR_OK ? n : 0] = 0;
	return r == FR_OK;
}
static const wificonf::Fs s_Fs = {FsWrite, FsRename, FsRemove, FsExists, FsRead};

// ---- the random numbers: the SoC's hardware generator, every word mixed with the free-running timer so a generator
// that is not there (or not yet warm) cannot make the code predictable on its own. Init says so if it repeats itself.
static CBcmRandomNumberGenerator *s_pRng = 0;
void Random (u8 *pOut, unsigned n)
{
	for (unsigned i = 0; i < n; i += 4)
	{
		u32 v = (s_pRng != 0 ? s_pRng->GetNumber () : 0) ^ CTimer::GetClockTicks ();
		for (unsigned k = 0; k < 4 && i + k < n; k++) pOut[i + k] = (u8) (v >> (8 * k));
	}
}
static u32 Ms (void) { return CTimer::Get ()->GetUptime () * 1000u; }	// seconds since boot: never wraps in a life

static paircode::Gate s_Gate;
static volatile unsigned s_nShowAsked = 0, s_nShowDone = 0, s_nHideAsked = 0, s_nHideDone = 0;
static unsigned s_nShowTries = 0, s_nShowAt = 0;
static bool s_bShowTried = false;	// TakeShow handed out the line at s_nShowAt (a try a second at most)
static paircode::ShowGap s_ShowGap;	// the network's requests for the modal (POST /pair/show), core 0
static bool s_bDisplay = true;
static volatile bool s_bReady = false;	// Init has run: the mode and the code are final (StatusLine, RytmHello)
static volatile bool s_bWebOn = true;		// what the menu asked for, served at once (web.cpp, sdfiles.cpp)
static volatile unsigned s_nCodeGen = 1;	// a new code (StatusLine's change; 1: Init's)
static paircode::HelloShow s_Hello;	// RytmHello's rule (paircode.h), the control core's

paircode::Gate &Gate () { return s_Gate; }

void Save ()
{
	if (!s_Gate.dirty ()) return;
	if (s_Gate.save (s_Fs, paircode::kPath))
	{
		s_Gate.saved ();
		CLogger::Get ()->Write (From, LogNotice, "%u paired device(s) stored in %s (their secrets are not: only SHA-256 of each)",
					s_Gate.clients (), paircode::kPath);
	}
	else CLogger::Get ()->Write (From, LogWarning, "cannot write %s: the paired devices are this run's only", paircode::kPath);
}

void Init (const char *pCmdline, bool bDisplay, bool bWanted)
{
	CLogger *L = CLogger::Get ();
	char Fixed[paircode::kDigits + 1];
	paircode::Mode m = paircode::mode_from_cmdline (pCmdline, Fixed);
	s_bDisplay = bDisplay;
	if (s_pRng == 0) s_pRng = new CBcmRandomNumberGenerator;
	// The raw hardware generator, before the timer is mixed in (that would hide a dead one): sixteen words, no two
	// alike. A failure refuses the gate outright (paircode.h, BACKLOG F4) - no weak code, no weak tokens.
	u32 w[16];
	boolean bRng = TRUE;
	for (unsigned i = 0; i < 16; i++) w[i] = s_pRng->GetNumber ();
	for (unsigned i = 0; i < 16; i++) for (unsigned k = i + 1; k < 16; k++) if (w[i] == w[k]) bRng = FALSE;
	memset (w, 0, sizeof w);
	paircode::Deps d = {Random, Ms};
	s_Gate.init (m, Fixed, d);
	if (!bRng) s_Gate.fail_rng ();
	if (m != paircode::kOff && !s_Gate.rng_ok ())
	{
		L->Write (From, LogError, "THE HARDWARE RANDOM GENERATOR FAILED ITS SELF-TEST (it repeats itself): pairing is "
					  "REFUSED for this boot rather than handing out a guessable code or token. Nothing on "
					  "the network can pair or resume; the serial console still works. Restart; if it stays, "
					  "report it.");
		Show ();		// RNG FAILED on the Rytm too (TakeShow)
		DataMemBarrier ();
		s_bReady = true;
		return;
	}
	if (m == paircode::kOff)
	{
		L->Write (From, LogError, "THE PAIRING CODE IS OFF (cmdline.txt paircode=0): anyone on this network can install "
					  "firmware, run console commands and change the Wi-Fi settings. A shipped card must not "
					  "have this.");
		DataMemBarrier ();
		s_bReady = true;
		return;
	}
	char Text[paircode::kFileMax > 1024 ? paircode::kFileMax : 1024];
	if (s_Gate.load_card (s_Fs, paircode::kPath) == 2)	// pair.txt missing: a cut between save's renames
	{
		L->Write (From, LogWarning, "%s was missing: the paired devices are read from %s.prev and written back",
			  paircode::kPath, paircode::kPath);
		Save ();
	}
	s_bWebOn = s_Gate.web_on ();		// SIDECAR MENU > WEB UI, kept in pair.txt
	if (!s_bWebOn)
		L->Write (From, LogWarning, "the web UI is OFF (the Rytm's SIDECAR MENU > WEB UI): the web page and port 7310 "
					    "from the network refuse until it is turned on there");
	L->Write (From, LogNotice, "the pairing code is on%s: it is on the Rytm's screen%s, and %u device(s) are already paired "
				   "(%s). Port 7310 and every request that changes something need it.",
		  m == paircode::kFixed ? " with the fixed code from cmdline.txt (a dev card)" : "",
		  bDisplay ? " and on the touchscreen's STATUS page" : " (no touchscreen on this one)", s_Gate.clients (),
		  paircode::kPath);
	// The code has to be readable somewhere. With a display it is on the touchscreen (and on the Rytm's screen);
	// with none it would be nowhere, so it goes to the card's root. Every read of the card is behind this gate, so
	// that file is not reachable over the network - but it is readable by whoever holds the card, which is the
	// deliberate limit of the scheme (docs/pi-engine.md "Pairing code" > "Threat model").
	if (!bDisplay && m != paircode::kOff)
	{
		int n = s_Gate.format_recourse (Text, sizeof Text);
		bool bOK = n > 0 && FsWrite (paircode::kRecoursePath, Text, (unsigned) n);
		// bWanted: cmdline.txt asked for a display and none came up. The owner thinks this unit has a screen, so
		// say plainly that the code was written down instead of quietly doing it.
		L->Write (From, bWanted ? LogWarning : LogNotice,
			  "%s: this boot's pairing code is written to %s at the card's root%s. Nothing on the network can "
			  "read it (every read of the card needs the code); whoever holds the card can. A unit that should "
			  "never write it down takes a fixed code instead: cmdline.txt paircode=<8 digits>.",
			  bWanted ? "A DISPLAY WAS CONFIGURED BUT DID NOT COME UP" : "no display on this unit",
			  paircode::kRecoursePath, bOK ? "" : " - BUT IT COULD NOT BE WRITTEN, so the code is on the Rytm's "
							 "screen only");
		memset (Text, 0, sizeof Text);
	}
	else if (FsExists (paircode::kRecoursePath))		// a display now: an earlier headless boot's file goes
	{
		bool bGone = FsRemove (paircode::kRecoursePath);
		L->Write (From, bGone ? LogNotice : LogWarning, "%s is from a boot with no display and holds an old pairing "
			  "code: %s", paircode::kRecoursePath,
			  bGone ? "deleted (the code is on the screens)" : "IT COULD NOT BE DELETED - delete it from the card");
	}
	Show ();
	DataMemBarrier ();
	s_bReady = true;
}

void RytmHello ()
{
	if (!s_bReady) return;
	if (s_Hello.take (CTimer::Get ()->GetUptime (), s_Gate.mode (), s_Gate.rng_ok (), s_Gate.clients ())) Show ();
}

bool StatusLine (char *pLine, unsigned nSize, unsigned *pGen)
{
	if (!s_bReady || *pGen == s_nCodeGen) return false;
	DataMemBarrier ();
	*pGen = s_nCodeGen;
	pLine[0] = 0;
	if (s_Gate.mode () == paircode::kOff || !s_Gate.rng_ok () || nSize < 16) return true;
	s_Gate.grouped (pLine, nSize);
	return true;
}

// ---- SIDECAR MENU > WEB UI
static volatile unsigned s_nNewAsked = 0, s_nNewDone = 0, s_nForgetAsked = 0, s_nForgetDone = 0;
bool WebOn () { return s_bWebOn; }
void MenuWeb (bool bOn) { s_bWebOn = bOn; }
int MenuGate ()
{
	if (s_Gate.mode () == paircode::kOff) return 0;
	if (!s_Gate.rng_ok ()) return 3;
	return s_Gate.mode () == paircode::kFixed ? 2 : 1;
}
int MenuDevices () { return (int) s_Gate.clients (); }
bool MenuNewCode ()
{
	if (MenuGate () != 1) return false;
	s_nNewAsked++;
	return true;
}
void MenuForget () { s_nForgetAsked++; }

void Poll ()
{
	if (!s_bReady) return;
	CLogger *L = CLogger::Get ();
	// (the gate off or with no RNG never loaded pair.txt: nothing is written over it from here)
	const bool bKeep = s_Gate.mode () != paircode::kOff && s_Gate.rng_ok ();
	if (s_bWebOn != s_Gate.web_on ())
	{
		s_Gate.set_web (s_bWebOn);
		if (bKeep) Save (); else s_Gate.saved ();
		L->Write (From, LogNotice, "%s", s_bWebOn ? "the web UI is on again (the Rytm's SIDECAR MENU > WEB UI)"
						    : "the web UI is OFF (the Rytm's SIDECAR MENU > WEB UI): the web page and port "
						      "7310 from the network refuse until it is turned on there; the serial :sdf still works");
	}
	if (s_nNewDone != s_nNewAsked)
	{
		s_nNewDone = s_nNewAsked;
		if (s_Gate.new_code ())
		{
			s_ShowGap.reset ();
			if (!s_bDisplay)		// the card-root recourse file holds the code on a unit with no display
			{
				char Text[1024];
				int n = s_Gate.format_recourse (Text, sizeof Text);
				if (n > 0) FsWrite (paircode::kRecoursePath, Text, (unsigned) n);
				memset (Text, 0, sizeof Text);
			}
			DataMemBarrier ();
			s_nCodeGen++;
			L->Write (From, LogNotice, "pair: a new pairing code (the Rytm's SIDECAR MENU > WEB UI): it is on the Rytm "
						   "there; the %u paired device(s) stay paired", s_Gate.clients ());
		}
	}
	if (s_nForgetDone != s_nForgetAsked)
	{
		s_nForgetDone = s_nForgetAsked;
		unsigned n = s_Gate.clients ();
		s_Gate.revoke_all ();
		if (bKeep) Save (); else s_Gate.saved ();
		midibridge_pi::PairingReset ();	// the Rytm MIDI bridge closes with it (as pair=revoke)
		L->Write (From, LogNotice, "pair: %u paired device(s) and every session forgotten (the Rytm's SIDECAR MENU > "
					   "WEB UI): each one needs the code again", n);
	}
}

void Status (CString *pOut)
{
	CString s;
	// "locked": the global backstop's wait (paircode.h); a single address locking itself is not the unit's state
	s.Format (",\"pair\":{\"gate\":\"%s\",\"rng\":%s,\"clients\":%u,\"sessions\":%u,\"locked\":%u}",
		  s_Gate.mode () == paircode::kOff ? "off" : s_Gate.mode () == paircode::kFixed ? "fixed" : "on",
		  s_Gate.rng_ok () ? "true" : "false", s_Gate.clients (), s_Gate.sessions (), s_Gate.global_wait_s ());
	pOut->Append (s);	// the code is deliberately not here: /status is open to anyone on the network
}

void WarnTick ()
{
	if (s_Gate.mode () == paircode::kOff)
		CLogger::Get ()->Write (From, LogWarning, "the pairing code is OFF on this card (cmdline.txt paircode=0): the network "
							  "is open to anyone who can reach it");
	else if (s_Gate.mode () == paircode::kFixed)
		CLogger::Get ()->Write (From, LogWarning, "the pairing code is the fixed one from cmdline.txt (a dev card): it does "
							  "not change at a boot");
}

void UiFill (sidecar_status_view_t *v)
{
	char g[16];
	if (s_Gate.mode () == paircode::kOff) { strcpy (v->pair, "OFF (cmdline)"); v->pair_warn = 1; return; }
	if (!s_Gate.rng_ok ()) { strcpy (v->pair, "RNG FAILED"); v->pair_warn = 1; return; }
	s_Gate.grouped (g, sizeof g);
	strcpy (v->pair, g);
	if (s_Gate.mode () == paircode::kFixed) strcat (v->pair, " FIXED");
	v->pair_warn = s_Gate.mode () == paircode::kFixed ? 1 : 0;
}

void Show () { s_nShowAsked++; s_nShowTries = 0; s_bShowTried = false; }

bool ShowUnpaired (unsigned *pWait)
{
	*pWait = 0;
	if (s_Gate.mode () == paircode::kOff) return false;
	unsigned now = CTimer::Get ()->GetUptime ();
	if (!s_ShowGap.take (now)) { *pWait = s_ShowGap.wait_s (now); return false; }
	Show ();
	return true;
}

void Hide () { s_nHideAsked++; s_ShowGap.reset (); }

bool TakeHide ()
{
	if (s_nHideDone == s_nHideAsked) return false;
	s_nHideDone = s_nHideAsked;
	s_nShowDone = s_nShowAsked;		// and nothing owed: the code is not wanted on screen any more
	s_nShowTries = 0;
	return true;
}

// The control core asks for the line to show. It stays owed until ShowDone: remote_popup drops a popup whenever the
// Rytm cannot take one (no Rytm, an older mod with no 36, or its Sidecar page open), and before this the request was
// consumed anyway and the code was silently never shown.
bool TakeShow (char *pLine, unsigned nSize)
{
	if (s_nShowDone == s_nShowAsked) return false;
	unsigned now = CTimer::Get ()->GetUptime ();
	if (s_bShowTried && now - s_nShowAt < 1) return false;	// a retry a second at most
	s_bShowTried = true;
	s_nShowAt = now;
	if (s_Gate.mode () == paircode::kOff) { strncpy (pLine, "PAIRING OFF", nSize - 1); pLine[nSize - 1] = 0; return true; }
	if (!s_Gate.rng_ok ()) { strncpy (pLine, "RNG FAILED", nSize - 1); pLine[nSize - 1] = 0; return true; }
	char g[16];
	s_Gate.grouped (g, sizeof g);
	strncpy (pLine, g, nSize - 1);
	pLine[nSize - 1] = 0;
	return true;
}

void ShowDone () { s_nShowDone = s_nShowAsked; s_nShowTries = 0; s_bShowTried = false; }

// The Rytm would not take it. After ten tries (about ten seconds) give up and say so, because the Rytm's screen is
// one of only two places the code ever appears and a silent drop would leave the user with nothing to type.
void ShowFailed ()
{
	if (++s_nShowTries < 10) return;
	s_nShowDone = s_nShowAsked;
	s_nShowTries = 0;
	CLogger::Get ()->Write (From, LogWarning, "the Rytm would not show the pairing code (no Rytm, an older mod without "
						  "the popups, or its Sidecar page is open). %s",
				s_bDisplay ? "It is on the touchscreen's STATUS page."
					   : "There is no touchscreen either: put a fixed code on the card "
					     "(cmdline.txt paircode=<8 digits>) and reboot.");
}

// The console's own (core 0, as rec_pi::Command and stor_pi::Command are: CConsole::Command runs on core 0 whether
// the line came from the serial port or from POST /cmd, so the gate needs no lock here)
bool Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (strcmp (p, "pair") == 0)
	{
		L->Write (From, LogNotice, "pair: the gate is %s, %u device(s) paired, %u session(s)%s. `pair=show` puts the code "
					   "on the Rytm's screen, `pair=revoke` unpairs every device.",
			  s_Gate.mode () == paircode::kOff ? "OFF (cmdline paircode=0)"
			  : s_Gate.mode () == paircode::kFixed ? "on with a fixed code (cmdline)" : "on",
			  s_Gate.clients (), s_Gate.sessions (),
			  !s_Gate.rng_ok () ? " - REFUSING: the random generator failed its self-test"
			  : s_Gate.global_wait_s () ? " (pairing is held for a moment: many wrong codes from the network)" : "");
		return true;
	}
	if (strcmp (p, "pair=show") == 0)
	{
		s_ShowGap.reset ();		// the console is the owner: the network's gap starts again too
		Show ();
		L->Write (From, LogNotice, "pair: the code is on the Rytm's screen%s (it is never in the log)",
			  s_bDisplay ? " and on the touchscreen's STATUS page" : "");
		return true;
	}
	if (strcmp (p, "pair=revoke") == 0)
	{
		unsigned n = s_Gate.clients ();
		s_Gate.revoke_all ();
		Save ();
		midibridge_pi::PairingReset ();	// the Rytm MIDI bridge closes with it (midibridge_pi.h)
		L->Write (From, LogNotice, "pair: %u paired device(s) and every session dropped: each one needs the code again", n);
		return true;
	}
	return false;
}

}
