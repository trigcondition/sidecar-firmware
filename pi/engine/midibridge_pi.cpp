// pi/engine/midibridge_pi.cpp - see midibridge_pi.h and midibridge.h.
#include "midibridge_pi.h"
#include "paircode_pi.h"

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/socket.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <circle/util.h>

static const char From[] = "midibridge";

namespace midibridge_pi {

static midibridge::Gate s_Gate;
static midibridge::ToRytm s_To;				// 300 KB
static midibridge::Pacer s_Pace;
static midibridge::FromRytm<1u << 18> s_From;		// 256 KB: about 1 s of the Rytm's fastest dumps
static unsigned s_nRytmFail = 0, s_nConnects = 0, s_nReplaced = 0;
static volatile boolean s_bClose = FALSE;		// the relay closes its client (off, a pairing reset)
static boolean s_bStarted = FALSE;

// the hand-over from the listener to the relay: an authenticated socket, its address, the bytes after its hello
// (written only for the accepted socket, after the relay took the last one)
static CSocket *s_pNew = 0;
static char s_NewIP[16];
static u8 s_Left[FRAME_BUFFER_SIZE];
static unsigned s_nLeft = 0;
// the relay's client
static boolean s_bConnected = FALSE;
static char s_IP[16] = "";

static void Random (u8 *pOut, unsigned n) { paircode_pi::Random (pOut, n); }
static u32 Ms (void) { return CTimer::Get ()->GetUptime () * 1000u; }	// seconds since boot, as paircode_pi's

static void Sync (void) { s_To.allow (s_Gate.os_allowed (), s_Gate.data_allowed ()); }

static void Off (const char *pWhy)
{
	boolean bWas = s_Gate.on ();
	s_Gate.disable ();				// the switches and the token go with it
	Sync ();
	s_bClose = TRUE;
	s_To.flush ();					// queued for the Rytm and not yet sent: dropped (a half-sent SysEx is finished)
	if (bWas) CLogger::Get ()->Write (From, LogNotice, "off (%s)", pWhy);
}

// ---- the listener: one connection at a time, its answer to the challenge within kHelloMs, then the relay takes it
class CBridgeListen : public CTask
{
public:
	CBridgeListen (CNetSubSystem *pNet) : m_pNet (pNet), m_nLeft (0) { SetName ("midibridge"); }
	void Run (void) override;
private:
	boolean Hello (CSocket *pConn, midibridge::HelloLine *pLine);
	CNetSubSystem *m_pNet;
	u8 m_Rx[FRAME_BUFFER_SIZE];
	u8 m_Left[FRAME_BUFFER_SIZE];			// this connection's bytes after its line: local until it is accepted
	unsigned m_nLeft;
};

// One line within kHelloMs; what came after it in the same segment is kept in m_Left (this connection's only)
boolean CBridgeListen::Hello (CSocket *pConn, midibridge::HelloLine *pLine)
{
	m_nLeft = 0;
	unsigned t0 = CTimer::GetClockTicks ();
	pConn->SetOptionReceiveTimeout (200000);
	while (CTimer::GetClockTicks () - t0 < midibridge::kHelloMs * 1000)
	{
		int r = pConn->Receive (m_Rx, sizeof m_Rx, 0);
		if (r == -NET_ERROR_WOULD_BLOCK) continue;
		if (r <= 0) return FALSE;
		unsigned k = pLine->feed (m_Rx, (unsigned) r);
		if (pLine->bad) return FALSE;
		if (pLine->done)
		{
			m_nLeft = (unsigned) r - k;
			memcpy (m_Left, m_Rx + k, m_nLeft);
			memset (m_Rx, 0, sizeof m_Rx);
			return TRUE;
		}
	}
	return FALSE;
}

static void Reply (CSocket *pConn, const char *p)
{
	pConn->Send (p, strlen (p), MSG_DONTWAIT);
}

void CBridgeListen::Run (void)
{
	CScheduler *S = CScheduler::Get ();
	CLogger *L = CLogger::Get ();
	while (!m_pNet->IsRunning ()) S->MsSleep (100);
	CSocket Listen (m_pNet, IPPROTO_TCP);
	if (Listen.Bind (midibridge::kPort) < 0 || Listen.Listen (1) < 0)
	{
		L->Write (From, LogError, "cannot listen on port %u", midibridge::kPort);
		return;
	}
	unsigned nRefusedAt = 0, nRefusedSince = 0;
	char LastIP[16] = "";
	for (;;)
	{
		CIPAddress IP;
		u16 nPort;
		CSocket *pConn = Listen.Accept (&IP, &nPort);
		if (pConn == 0) { S->MsSleep (100); continue; }
		CString Ip;
		IP.Format (&Ip);
		char Addr[16];
		strncpy (Addr, (const char *) Ip, sizeof Addr - 1);
		Addr[sizeof Addr - 1] = 0;
		u32 nIP = (u32) IP;
		const char *pQuiet = 0;			// a refusal logged at most every 10 s
		if (!s_Gate.on ()) pQuiet = "ERR the Rytm MIDI bridge is off (the Sidecar web page: Status > Rytm MIDI bridge)\n";
		else if (s_Gate.locked (nIP, Ms ())) pQuiet = "ERR this address is locked out after wrong answers\n";
		if (pQuiet != 0)
		{
			Reply (pConn, pQuiet);
			S->MsSleep (20);
			delete pConn;
			nRefusedSince++;
			strcpy (LastIP, Addr);
		}
		else
		{
			char Nonce[midibridge::kNonceHex + 1], Greet[64];
			midibridge::Gate::nonce (Random, Nonce);
			strcpy (Greet, "SIDECAR-MIDIBRIDGE 1 ");
			strcat (Greet, Nonce);
			strcat (Greet, "\n");
			Reply (pConn, Greet);
			midibridge::HelloLine Line;
			boolean bLine = Hello (pConn, &Line);
			midibridge::Gate::Hello h = bLine ? s_Gate.hello (Line.line, Nonce, nIP, Ms ())
							  : midibridge::Gate::kNotBridge;
			Line.wipe ();
			if (h == midibridge::Gate::kOk && s_Gate.on ())
			{
				Reply (pConn, "OK sidecar-midibridge\n");
				L->Write (From, LogNotice, "client %s connected (port %u): the Rytm's USB MIDI is relayed to it%s%s", Addr,
					  midibridge::kPort, s_Gate.os_allowed () ? " - OS UPGRADES ALLOWED" : "",
					  s_Gate.data_allowed () ? " - +DRIVE WRITES ALLOWED" : "");
				while (s_pNew != 0) S->MsSleep (1);	// the relay has not taken the last one yet
				memcpy (s_Left, m_Left, m_nLeft);	// only now, and only for the accepted socket
				s_nLeft = m_nLeft;
				strcpy (s_NewIP, Addr);
				s_pNew = pConn;
				s_nConnects++;
			}
			else
			{
				Reply (pConn, h == midibridge::Gate::kOff ? "ERR the Rytm MIDI bridge is off\n"
					      : h == midibridge::Gate::kLocked ? "ERR wrong answer: this address is locked out\n"
					      : h == midibridge::Gate::kNotBridge ? "ERR not a bridge client\n" : "ERR wrong answer\n");
				S->MsSleep (20);
				delete pConn;
				if (h == midibridge::Gate::kLocked)
					L->Write (From, LogWarning, "client %s: %u wrong answers - this address is locked out for %u min",
						  Addr, midibridge::kMaxFails, midibridge::kLockMs / 60000);
				else if (h == midibridge::Gate::kBad) L->Write (From, LogWarning, "client %s: wrong answer, closed", Addr);
				else { nRefusedSince++; strcpy (LastIP, Addr); }
			}
			memset (m_Left, 0, sizeof m_Left);
			m_nLeft = 0;
			memset (Nonce, 0, sizeof Nonce);
		}
		unsigned now = CTimer::Get ()->GetUptime ();
		if (nRefusedSince != 0 && now - nRefusedAt >= 10)
		{
			L->Write (From, LogNotice, "%u connection(s) refused (off, locked out, or not a bridge client; the last from %s)",
				  nRefusedSince, LastIP);
			nRefusedAt = now;
			nRefusedSince = 0;
		}
	}
}

// ---- the relay: the client's bytes to ToRytm (whole messages; a full queue stops the reading), the Rytm's to the
// client (a bounded queue: a client that stops reading loses whole messages, counted, and nothing here waits on it)
class CBridgeRelay : public CTask
{
public:
	CBridgeRelay (void) : m_pConn (0), m_nRxPos (0), m_nRxLen (0) { SetName ("midibridge-relay"); }
	void Run (void) override;
private:
	void Close (const char *pWhy);
	CSocket *m_pConn;
	u8 m_Rx[FRAME_BUFFER_SIZE];
	unsigned m_nRxPos, m_nRxLen;
	u8 m_Tx[512];					// one segment at most (the minimum MSS is 536): a Send is all or nothing
};

void CBridgeRelay::Close (const char *pWhy)
{
	s_From.set_active (FALSE);
	s_bConnected = FALSE;
	if (m_pConn == 0) return;
	delete m_pConn;
	m_pConn = 0;
	m_nRxPos = m_nRxLen = 0;
	CLogger::Get ()->Write (From, LogNotice, "client %s closed (%s): %lu bytes to the Rytm, %lu from it, %u message(s) "
				"dropped while it did not read", s_IP, pWhy, (unsigned long) s_To.stats ().in,
				(unsigned long) s_From.sent (), s_From.drop_msgs ());
}

void CBridgeRelay::Run (void)
{
	CScheduler *S = CScheduler::Get ();
	for (;;)
	{
		if (s_pNew != 0)				// a newer authenticated client: it wins
		{
			if (m_pConn != 0) { s_nReplaced++; Close ("a newer client connected"); }
			m_pConn = s_pNew;
			strcpy (s_IP, s_NewIP);
			memcpy (m_Rx, s_Left, s_nLeft);
			m_nRxPos = 0;
			m_nRxLen = s_nLeft;
			memset (s_Left, 0, s_nLeft);
			s_nLeft = 0;
			s_pNew = 0;
			m_pConn->SetOptionSendTimeout (2000);	// Circle waits only when 64 KB are queued: then 2 ms at most
			s_bClose = FALSE;
			s_From.clear ();
			s_From.set_active (TRUE);
			s_bConnected = TRUE;
		}
		if (m_pConn == 0) { S->MsSleep (5); continue; }
		if (s_bClose || !s_Gate.on ()) { Close ("the bridge was turned off"); s_bClose = FALSE; continue; }
		boolean bBusy = FALSE;
		// the client's bytes: the next segment only once the last one is all queued (TCP's flow control does the rest).
		// A client that closed: Circle's Receive answers -NET_ERROR_CONNECTION_RESET once its FIN is in (CloseWait,
		// nothing queued) - with MSG_DONTWAIT 0 means only "nothing yet".
		if (m_nRxPos == m_nRxLen)
		{
			int n = m_pConn->Receive (m_Rx, sizeof m_Rx, MSG_DONTWAIT);
			if (n < 0) { Close ("the client went"); continue; }
			m_nRxPos = 0;
			m_nRxLen = (unsigned) n;
		}
		if (m_nRxPos < m_nRxLen)
		{
			unsigned k = s_To.feed (m_Rx + m_nRxPos, m_nRxLen - m_nRxPos);
			m_nRxPos += k;
			bBusy = k > 0;
		}
		// the Rytm's bytes
		for (unsigned nRound = 0; nRound < 16; nRound++)
		{
			unsigned n = s_From.peek (m_Tx, sizeof m_Tx);
			if (n == 0) break;
			int r = m_pConn->Send (m_Tx, n, 0);
			if (r == (int) n) { s_From.consume (n); bBusy = TRUE; continue; }
			if (r == -NET_ERROR_WOULD_BLOCK) break;	// the client is not reading: the queue fills, then drops
			Close ("a send failed");
			break;
		}
		if (m_pConn == 0) continue;
		if (bBusy) S->Yield ();
		else S->MsSleep (1);
	}
}

void Start (CNetSubSystem *pNet)
{
	if (s_bStarted) return;
	s_bStarted = TRUE;
	new CBridgeListen (pNet);
	new CBridgeRelay;
	CLogger::Get ()->Write (From, LogNotice, "the Rytm MIDI bridge (TCP port %u) is OFF; turn it on for this boot on the web "
				"page (Status > Rytm MIDI bridge) or with midibridge=on", midibridge::kPort);
}

void FromRytm (const u8 *pMidi, unsigned nLength) { s_From.packet (pMidi, nLength); }
midibridge::ToRytm *Queue (void) { return &s_To; }
midibridge::Pacer *Pace (void) { return &s_Pace; }
void SetRytmFail (unsigned n) { s_nRytmFail = n; }

static int Json (char *pOut, unsigned nSize, boolean bPrivate)
{
	midibridge::Snapshot x;
	x.state = !s_Gate.on () ? "off" : s_bConnected ? "connected" : "waiting";
	x.client = s_bConnected ? s_IP : "";
	x.connects = s_nConnects, x.replaced = s_nReplaced, x.refused = s_Gate.refused (), x.locked = s_Gate.locked_count (Ms ());
	x.drops = s_From.drop_msgs (), x.drop_bytes = s_From.drop_bytes (), x.rytm_fail = s_nRytmFail;
	x.queued = s_To.queued_bytes (), x.from_rytm = s_From.sent ();
	return midibridge::status_json (pOut, nSize, s_Gate, s_To, x, bPrivate);
}

void Status (CString *pOut)
{
	char J[1024];
	if (Json (J, sizeof J, FALSE) < 0) return;
	pOut->Append (",\"midibridge\":");
	pOut->Append (J);
}

int WebJson (char *pOut, unsigned nSize) { return Json (pOut, nSize, TRUE); }

static const char *On (void)
{
	// paircode=0 (cmdline.txt): anyone on the network could turn it on and read the token from the page
	if (paircode_pi::Gate ().mode () == paircode::kOff)
		return "refused: the pairing code is off on this card (cmdline.txt paircode=0); the bridge needs it";
	if (!paircode_pi::Gate ().rng_ok ())		// its token comes from the same generator (paircode.h)
		return "refused: the random number generator failed its self-test at boot";
	if (s_Gate.on ()) return 0;
	s_Gate.enable (Random);
	Sync ();
	s_Pace = midibridge::Pacer ();
	CLogger::Get ()->Write (From, LogNotice, "ON for this boot: TCP port %u, a client must answer the challenge with this "
				"enable's token (on the web page only); OS upgrades and +Drive writes are blocked",
				midibridge::kPort);
	return 0;
}

static const char *Switch (boolean bOs, boolean bAllow)
{
	if (bAllow && !s_Gate.on ()) return "turn the bridge on first";
	boolean bWas = bOs ? s_Gate.os_allowed () : s_Gate.data_allowed ();
	if (bOs) s_Gate.allow_os (bAllow);
	else s_Gate.allow_data (bAllow);
	Sync ();				// a switch turned off: what was queued under it is dropped when it would start (L1)
	if (bWas != bAllow)
		CLogger::Get ()->Write (From, bAllow ? LogWarning : LogNotice, "%s %s through the bridge", bOs ? "OS upgrades" : "+Drive writes",
					bAllow ? "ALLOWED for this session: a client can change the Rytm" : "blocked again");
	return 0;
}

static boolean Bit (const char *p) { return p != 0 && (strcmp (p, "1") == 0 || strcmp (p, "0") == 0); }

const char *WebSet (const char *pOn, const char *pOs, const char *pData)
{
	if ((pOn != 0 && !Bit (pOn)) || (pOs != 0 && !Bit (pOs)) || (pData != 0 && !Bit (pData))) return "on=, os=, data= are 0 or 1";
	const char *pErr = 0;
	if (pOn != 0 && pOn[0] == '0') Off ("the web page");
	if (pOn != 0 && pOn[0] == '1') pErr = On ();
	if (pErr == 0 && pOs != 0) pErr = Switch (TRUE, pOs[0] == '1');
	if (pErr == 0 && pData != 0) pErr = Switch (FALSE, pData[0] == '1');
	return pErr;
}

bool Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (strcmp (p, "midibridge") == 0)
	{
		const midibridge::ToRytm::Stats &t = s_To.stats ();
		L->Write (From, LogNotice, "%s%s%s, port %u, OS upgrades %s, +Drive writes %s | to the Rytm %lu B (%u msgs, %u + %u "
			  "blocked, %u too long, %u junk, %u flushed) | from it %lu B, %u dropped | %u connects, %u refused, %u locked",
			  !s_Gate.on () ? "off" : s_bConnected ? "connected" : "waiting", s_bConnected ? " from " : "",
			  s_bConnected ? s_IP : "", midibridge::kPort, s_Gate.os_allowed () ? "ALLOWED" : "blocked",
			  s_Gate.data_allowed () ? "ALLOWED" : "blocked", (unsigned long) t.sent, t.msgs, t.blocked, t.blocked_data,
			  t.too_long, t.junk, t.flushed, (unsigned long) s_From.sent (), s_From.drop_msgs (), s_nConnects,
			  s_Gate.refused (), s_Gate.locked_count (Ms ()));
		return true;
	}
	if (strncmp (p, "midibridge=", 11) != 0) return false;
	const char *v = p + 11, *pErr = 0;
	if (strcmp (v, "on") == 0) pErr = On ();
	else if (strcmp (v, "off") == 0) Off ("the console");
	else if (strcmp (v, "os:on") == 0) pErr = Switch (TRUE, TRUE);
	else if (strcmp (v, "os:off") == 0) pErr = Switch (TRUE, FALSE);
	else if (strcmp (v, "data:on") == 0) pErr = Switch (FALSE, TRUE);
	else if (strcmp (v, "data:off") == 0) pErr = Switch (FALSE, FALSE);
	else pErr = "midibridge, midibridge=on|off, midibridge=os:on|off, midibridge=data:on|off";
	if (pErr != 0) L->Write (From, LogWarning, "%s: %s", p, pErr);
	return true;
}

void PairingReset (void)
{
	boolean bWas = s_Gate.on ();
	Off ("every device was unpaired");
	s_Gate.pairing_reset ();
	Sync ();
	if (bWas) CLogger::Get ()->Write (From, LogNotice, "the pairing was reset: the bridge is off (a new token when it is turned on)");
}

}
