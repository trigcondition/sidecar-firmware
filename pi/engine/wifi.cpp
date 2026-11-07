// pi/engine/wifi.cpp - Wi-Fi on the Pi 4's BCM43455 (wifi.h, docs/research/pi-wifi.md, docs/pi-engine.md "Wi-Fi
// settings"). Never logs, stores or returns a passphrase, and never logs an SSID.
#include "wifi.h"

#include <circle/logger.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>

extern "C" int pbkdf2_sha1 (const char *passphrase, const u8 *ssid, size_t ssid_len, int iterations, u8 *buf,
			    size_t buflen);		// hostap's src/crypto/sha1-pbkdf2.c (in libwpa_supplicant.a)

using namespace wificonf;

static const char From[] = "wifi";
static const char ConfPath[] = "/.rytm/wifi.txt";	// on the card (FatFs "SD:"), through s_Fs
static const char TryPath[] = "/.rytm/wifi.try";
static const char SupplicantPath[] = "SD:/.rytm/wpa.conf";	// written from wifi.txt at a Wi-Fi boot (the key only)
static const char FirmwarePath[] = "SD:/firmware/";

CWifi *CWifi::s_pThis = 0;

// the escan result the chip sends (as hostap's driver_circle.cpp, which parses the same queue)
struct TBssInfo
{
	u32 version;
	u32 length;
	u8 BSSID[6];
	u16 beacon_period;
	u16 capability;
	u8 SSID_len;
	u8 SSID[32];
	struct { u32 count; u8 rates[16]; } rateset;
	u16 chanspec;
	u16 atim_window;
	u8 dtim_period;
	u16 RSSI;
	s8 phy_noise;
	u8 n_cap;
	u32 nbss_cap;
	u8 ctl_ch;
	u32 reserved32[1];
	u8 flags;
	u8 reserved[3];
	u8 basic_mcs[16];
	u16 ie_offset;
	u32 ie_length;
	u16 SNR;
};
struct TEscanResult
{
	u32 buflen;
	u32 version;
	u16 sync_id;
	u16 bss_count;
	TBssInfo bss;
};
static const u16 CapPrivacy = 0x0010;			// IEEE 802.11 capability: WEP/WPA/WPA2 required

// ---- the card for wifi_conf (paths without the drive)
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
static bool FsRename (const char *a, const char *b) { char s[96]; Sd (s, sizeof s, a); return f_rename (s, b) == FR_OK; }	// b without its drive (FatFs)
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
static const Fs s_Fs = {FsWrite, FsRename, FsRemove, FsExists, FsRead};

static boolean ReadText (const char *p, char *pOut, unsigned nSize)
{
	char s[96];
	Sd (s, sizeof s, p);
	FIL f;
	if (f_open (&f, s, FA_READ) != FR_OK) return FALSE;
	UINT n = 0;
	FRESULT r = f_read (&f, pOut, nSize - 1, &n);
	f_close (&f);
	pOut[r == FR_OK ? n : 0] = 0;
	return r == FR_OK;
}

static void Key64 (const char *pPass, const char *pSSID, char Key[65])	// WPA2's PBKDF2-SHA1, as hex
{
	u8 PMK[32];
	pbkdf2_sha1 (pPass, (const u8 *) pSSID, strlen (pSSID), 4096, PMK, sizeof PMK);
	static const char H[] = "0123456789abcdef";
	for (unsigned i = 0; i < 32; i++)
	{
		Key[2 * i] = H[PMK[i] >> 4];
		Key[2 * i + 1] = H[PMK[i] & 15];
	}
	Key[64] = 0;
	memset (PMK, 0, sizeof PMK);
}

static void JsonAppend (CString *pOut, const char *p)
{
	char b[6 * kSsidMax + 1];
	json_escape (p, b, sizeof b);
	pOut->Append (b);
}

CWifi::CWifi (void)
:	m_Mode (ModeOff), m_bHaveConf (FALSE), m_pCmdline (0), m_pNetName (0), m_pDev (0), m_pSupplicant (0),
	m_bScanReq (FALSE), m_bChipReq (FALSE), m_bScanning (FALSE), m_nScans (0), m_nScanMs (0), m_nScanAt (0),
	m_nDownSince (0), m_nRestarts (0), m_nRestartsTotal (0), m_bUp (FALSE),
	m_Join (JoinNone), m_bTrial (FALSE), m_bStarted (FALSE), m_bAssocSeen (FALSE), m_nTrialT0 (0), m_nWrongKey (0), m_nRebootAt (0),
	m_nNets (0)
{
	s_pThis = this;
	m_Why[0] = 0;
	SetName ("wifi");
	recover (s_Fs, ConfPath);			// a save cut short between its renames
	m_bHaveConf = ReadConf (&m_Conf);
	{						// sidecar-wifi.txt at the card root: imported like the page's join, then deleted
		char Reason[160];
		Conf c = m_Conf;
		Import r = import_card (s_Fs, ConfPath, TryPath, Key64, &c, Reason, sizeof Reason);
		if (r == kImported)
		{
			CLogger::Get ()->Write (From, LogNotice, "wifi: imported sidecar-wifi.txt from the card, joining (trial %u s)", TrialSecs);
			m_Conf = c;
		}
		else if (r == kRejected)
			CLogger::Get ()->Write (From, LogWarning, "wifi: sidecar-wifi.txt not used (%s); see sidecar-wifi-error.txt", Reason);
		memset (&c, 0, sizeof c);
		if (r == kImported) m_bHaveConf = TRUE;
	}
	char t[128];
	if (ReadText (TryPath, t, sizeof t))
	{
		Trial tr = parse_trial (t, m_Why, sizeof m_Why);
		if (tr == kTrying) { m_bTrial = TRUE; m_Join = JoinTrying; }
		else if (tr == kFailed) m_Join = JoinFailed;
	}
}

boolean CWifi::ReadConf (Conf *c)
{
	char t[512];
	defaults (c);
	if (!ReadText (ConfPath, t, sizeof t)) return FALSE;
	parse (t, c);
	memset (t, 0, sizeof t);
	return TRUE;
}

boolean CWifi::WriteConf (const Conf *c, boolean bKeepPrev)
{
	return save (s_Fs, ConfPath, c, bKeepPrev);
}

void CWifi::WriteTrial (Trial t, const char *pWhy)
{
	if (t == kNone) { FsRemove (TryPath); return; }
	char b[128];
	int n = format_trial (t, pWhy, b, sizeof b);
	if (n <= 0 || !FsWrite (TryPath, b, n)) CLogger::Get ()->Write (From, LogError, "cannot write %s", TryPath);
}

CWifi::TMode CWifi::BootMode (CKernelOptions *pOptions)
{
	const char *p = pOptions->GetAppOptionString ("wifi", 0);
	m_pCmdline = p;
	TMode m = p == 0 ? (m_bHaveConf && m_Conf.on ? ModeNet : ModeOff)
		: strcmp (p, "1") == 0 || strcmp (p, "on") == 0 ? ModeNet
		: strcmp (p, "scan") == 0 ? ModeScan : ModeOff;
	if (m == ModeNet && !(m_bHaveConf && has_network (&m_Conf)))
	{
		CLogger::Get ()->Write (From, LogWarning, "no stored network in %s: the chip for scans only, Ethernet stays", ConfPath);
		m = ModeScan;
	}
	m_Mode = m;
	return m;
}

void CWifi::OtherNet (const char *pName)
{
	m_pNetName = pName;
	if (m_Mode == ModeNet)
	{
		m_Mode = ModeScan;
		CLogger::Get ()->Write (From, LogNotice, "the network is on %s this boot: the stored network is not joined (the chip for scans)",
					pName);
	}
	if (m_bTrial)
	{
		m_bTrial = FALSE;
		m_Join = JoinNone;
		CLogger::Get ()->Write (From, LogNotice, "the join being tried waits for a boot on Wi-Fi");
	}
}

boolean CWifi::StartChip (void)
{
	if (m_pDev != 0) return TRUE;
	unsigned t0 = CTimer::GetClockTicks ();
	CBcm4343Device *pDev = new CBcm4343Device (FirmwarePath);
	if (!pDev->Initialize ())
	{
		CLogger::Get ()->Write (From, LogError, "the chip did not start (firmware in %s?): Wi-Fi off", FirmwarePath);
		m_Mode = ModeOff;		// pDev is left alone (no destructor: Circle's driver has none that resets it)
		return FALSE;
	}
	m_pDev = pDev;
	pDev->Control ("country %s", m_Conf.country);
	CString Mac;
	pDev->GetMACAddress ()->Format (&Mac);
	CLogger::Get ()->Write (From, LogNotice, "BCM43455 up in %u ms (MAC %s, country %s)",
				(CTimer::GetClockTicks () - t0) / 1000, (const char *) Mac, m_Conf.country);
	return TRUE;
}

static boolean ParseIP (const char *p, u8 IP[4])
{
	for (int i = 0; i < 4; i++)
	{
		if (*p < '0' || *p > '9') return FALSE;
		unsigned v = 0;
		while (*p >= '0' && *p <= '9' && v <= 255) v = v * 10 + (*p++ - '0');
		if (v > 255) return FALSE;
		IP[i] = (u8) v;
		if (i < 3 && *p++ != '.') return FALSE;
	}
	return *p == 0;
}

boolean CWifi::StaticIP (u8 IP[4], u8 Mask[4], u8 GW[4], u8 DNS[4]) const
{
	return m_Conf.ip[0] && ParseIP (m_Conf.ip, IP) && ParseIP (m_Conf.mask, Mask) && ParseIP (m_Conf.gw, GW)
	       && ParseIP (m_Conf.dns, DNS);
}

// wpa_supplicant's own config, from wifi.txt: the SSID as hex (no quoting), the 64-hex key (a passphrase in wifi.txt
// is hashed here, PBKDF2-SHA1 as WPA2 defines it, so wpa.conf never holds it); an open network: key_mgmt=NONE
boolean CWifi::WriteSupplicantConf (void)
{
	char Key[65] = "";
	if (!m_Conf.open)
	{
		if (is_hex64 (m_Conf.psk)) strcpy (Key, m_Conf.psk);
		else Key64 (m_Conf.psk, m_Conf.ssid, Key);
	}
	CString s;
	s.Format ("country=%s\nnetwork={\n\tscan_ssid=1\n\tkey_mgmt=%s\n\tssid=", m_Conf.country, m_Conf.open ? "NONE" : "WPA-PSK");
	for (const char *p = m_Conf.ssid; *p; p++)
	{
		CString h;
		h.Format ("%02x", (unsigned char) *p);
		s.Append (h);
	}
	s.Append ("\n");
	if (!m_Conf.open) { s.Append ("\tpsk="); s.Append (Key); s.Append ("\n"); }
	if (m_Conf.band == 1)		// 2.4 GHz channels 1-13
		s.Append ("\tfreq_list=2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462 2467 2472\n"
			  "\tscan_freq=2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462 2467 2472\n");
	else if (m_Conf.band == 2)	// 5 GHz, UNII-1..3
		s.Append ("\tfreq_list=5180 5200 5220 5240 5260 5280 5300 5320 5500 5520 5540 5560 5580 5600 5620 5640 5660 5680 5700 5720 5745 5765 5785 5805 5825\n");
	s.Append ("}\n");
	memset (Key, 0, sizeof Key);
	FIL f;
	if (f_open (&f, SupplicantPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return FALSE;
	UINT nDone = 0;
	FRESULT r = f_write (&f, (const char *) s, s.GetLength (), &nDone);
	f_close (&f);
	return r == FR_OK && nDone == s.GetLength ();
}

void CWifi::StartSupplicant (boolean bNetOK)
{
	m_bStarted = TRUE;				// the trial may judge from now on
	if (!bNetOK || m_Mode != ModeNet || m_pDev == 0 || m_pSupplicant != 0) return;
	if (!WriteSupplicantConf ())
	{
		CLogger::Get ()->Write (From, LogError, "cannot write %s", SupplicantPath);
		return;
	}
	m_pSupplicant = new CWPASupplicant (SupplicantPath);
	m_pSupplicant->Initialize ();
	m_nDownSince = m_nTrialT0 = CTimer::Get ()->GetUptime ();
	CLogger::Get ()->Write (From, LogNotice, "joining the stored network (%s, %s, band %s)%s", m_Conf.open ? "open" : "WPA2-PSK",
				m_Conf.ip[0] ? "static" : "DHCP", kBandNames[m_Conf.band < 3 ? m_Conf.band : 1],
				m_bTrial ? ": a trial (the previous setting back if it is not up in time)" : "");
}

void CWifi::RestartSupplicant (void)
{
	delete m_pSupplicant;		// eloop_terminate, waits for its task to end (wpa_supplicant_deinit: deauthenticates)
	m_pSupplicant = new CWPASupplicant (SupplicantPath);	// wpa.conf as written at StartSupplicant
	m_pSupplicant->Initialize ();
}

boolean CWifi::IsSecret (const char *pCmd)
{
	return strncmp (pCmd, "wifi=join", 9) == 0;
}

const char *CWifi::Busy (void) const
{
	if (m_nRebootAt != 0) return "busy: Sidecar is restarting";
	if (m_bTrial) return "busy: a join is being tried";
	return 0;
}

// a new network into wifi.txt (the previous one kept as wifi.txt.prev) and the trial marker: tried at the next boot
const char *CWifi::Store (const char *pSSID, unsigned nSSID, const char *pPSK, boolean bOpen)
{
	if (const char *pBusy = Busy ()) return pBusy;
	if (m_pCmdline != 0 && strcmp (m_pCmdline, "1") != 0 && strcmp (m_pCmdline, "on") != 0)
		return "cmdline.txt has wifi= set (not 1): Wi-Fi from this page needs it removed (pi/circle/sdboot.py cmdline)";
	Err e = check_join (pSSID, nSSID, pPSK, bOpen);
	if (e != kOk) return err_text (e);
	Conf n = m_Conf;
	memcpy (n.ssid, pSSID, nSSID);
	n.ssid[nSSID] = 0;
	n.open = bOpen;
	memset (n.psk, 0, sizeof n.psk);
	if (!bOpen)
	{
		if (is_hex64 (pPSK)) strcpy (n.psk, pPSK);
		else Key64 (pPSK, n.ssid, n.psk);	// the key, never the passphrase
	}
	n.on = TRUE;
	boolean bOK = WriteConf (&n, TRUE);
	if (bOK) { m_Conf = n; m_bHaveConf = TRUE; }
	memset (&n, 0, sizeof n);
	if (!bOK)
	{
		CLogger::Get ()->Write (From, LogError, "join: cannot write %s", ConfPath);
		return "cannot write /.rytm/wifi.txt";
	}
	WriteTrial (kTrying, 0);
	m_Join = JoinRestarting;
	m_Why[0] = 0;
	return 0;
}

const char *CWifi::Join (const char *pSSID, unsigned nSSID, const char *pPSK, boolean bOpen)
{
	const char *pErr = Store (pSSID, nSSID, pPSK, bOpen);
	if (pErr != 0) return pErr;
	m_nRebootAt = CTimer::Get ()->GetUptime () + 2;	// the answer goes out first
	CLogger::Get ()->Write (From, LogNotice, "join: a new network stored in %s (its key, not the passphrase; the previous "
				"file kept as wifi.txt.prev): restarting on Wi-Fi, the previous setting back if it is not up in %u s",
				ConfPath, TrialSecs);
	return 0;
}

const char *CWifi::Forget (void)
{
	if (const char *pBusy = Busy ()) return pBusy;
	Conf n = m_Conf;
	memset (n.ssid, 0, sizeof n.ssid);
	memset (n.psk, 0, sizeof n.psk);
	n.open = FALSE;
	n.on = FALSE;
	boolean bOK = WriteConf (&n, FALSE);
	memset (&n, 0, sizeof n);
	if (!bOK) return "cannot write /.rytm/wifi.txt";
	char Prev[40];
	strcpy (Prev, ConfPath);
	strcat (Prev, ".prev");
	FsRemove (Prev);				// its key too
	WriteTrial (kNone, 0);
	ReadConf (&m_Conf);
	m_bHaveConf = TRUE;
	m_Join = JoinNone;
	m_Why[0] = 0;
	CLogger::Get ()->Write (From, LogNotice, "forget: the stored network removed from %s (and wifi.txt.prev), on=0%s", ConfPath,
				m_Mode == ModeNet ? ": Ethernet from the next boot" : "");
	return 0;
}

const char *CWifi::Power (boolean bOn)
{
	if (const char *pBusy = Busy ()) return pBusy;
	Conf n = m_Conf;
	n.on = bOn;
	// on with a stored network is a join at the next boot: a trial, wifi.txt.prev (off) back if it does not come up
	boolean bTrial = bOn && !m_Conf.on && has_network (&m_Conf);
	boolean bOK = WriteConf (&n, bTrial);
	memset (&n, 0, sizeof n);
	if (!bOK) return "cannot write /.rytm/wifi.txt";
	m_Conf.on = bOn;
	m_bHaveConf = TRUE;
	if (bTrial) WriteTrial (kTrying, 0);
	else if (!bOn) WriteTrial (kNone, 0);
	if (bOn && m_pDev == 0 && !m_bChipReq) { m_Mode = ModeScan; m_bChipReq = TRUE; }	// scans now
	CLogger::Get ()->Write (From, LogNotice, "stored on=%u in %s (the next boot%s)", bOn ? 1 : 0, ConfPath,
				m_pCmdline ? "; cmdline.txt's wifi= wins" : "");
	return 0;
}

const char *CWifi::Scan (void)
{
	if (m_pDev == 0) return m_bChipReq ? "the chip is starting" : "Wi-Fi is off: turn it on to scan";
	if (m_bTrial) return "busy: a join is being tried";
	if (m_Mode == ModeNet && !(m_pSupplicant != 0 && CWPASupplicant::IsConnected ()))
		return "not while Wi-Fi is connecting";
	m_bScanReq = TRUE;
	return 0;
}

boolean CWifi::TakeReboot (void)
{
	if (m_nRebootAt == 0 || CTimer::Get ()->GetUptime () < m_nRebootAt) return FALSE;
	m_nRebootAt = 0;
	return TRUE;
}

boolean CWifi::Command (const char *pCmd)
{
	if (strncmp (pCmd, "wifi", 4) != 0 || (pCmd[4] != 0 && pCmd[4] != '=')) return FALSE;
	CLogger *L = CLogger::Get ();
	const char *v = pCmd[4] == '=' ? pCmd + 5 : "";
	if (*v == 0)
	{
		CString s;
		Status (&s);
		L->Write (From, LogNotice, "%s", (const char *) s);
	}
	else if (strcmp (v, "scan") == 0)
	{
		const char *pErr = Scan ();
		if (pErr) L->Write (From, LogWarning, "scan: %s", pErr);
	}
	else if (strcmp (v, "on") == 0 || strcmp (v, "off") == 0)
	{
		const char *pErr = Power (v[1] == 'n');
		if (pErr) L->Write (From, LogError, "%s: %s", v, pErr);
	}
	else if (strncmp (v, "band:", 5) == 0)
	{
		unsigned b = band_from_name (v + 5, 3);
		if (b == 3) { L->Write (From, LogWarning, "band: wifi=band:2g|5g|any"); return TRUE; }
		m_Conf.band = b;
		boolean bOK = WriteConf (&m_Conf, FALSE);
		L->Write (From, bOK ? LogNotice : LogError, "%s band=%s in %s (the next boot)", bOK ? "stored" : "cannot store",
			  kBandNames[b], ConfPath);
	}
	else if (strncmp (v, "join:", 5) == 0)
	{
		const char *s = v + 5, *c = strchr (s, ':');
		const char *pErr = c == 0 ? "wifi=join:SSID:PASS" : Store (s, c - s, c + 1, FALSE);
		if (pErr) L->Write (From, LogWarning, "join: %s", pErr);
		else L->Write (From, LogNotice, "a network stored in %s (its key, not the passphrase), on=1: Wi-Fi tries it at the "
			       "next boot (the previous setting back if it is not up in %u s)", ConfPath, TrialSecs);
	}
	else L->Write (From, LogWarning, "wifi, wifi=scan|on|off|band:2g|5g|any|join:SSID:PASS");
	return TRUE;
}

static const char *const Modes[] = {"off", "scan", "net"};
static const char *const Joins[] = {"none", "restarting", "trying", "up", "failed"};

void CWifi::Status (CString *pOut)
{
	CString s;
	s.Format ("{\"mode\":\"%s\",\"chip\":%s,\"stored\":%s,\"on\":%s,\"connected\":%s,\"rejoins\":%u,\"scanning\":%s,"
		  "\"scans\":%u,\"networks\":%u,\"join\":\"%s\"}",
		  Modes[m_Mode], m_pDev ? "true" : "false", m_bHaveConf && m_Conf.ssid[0] ? "true" : "false",
		  m_bHaveConf && m_Conf.on ? "true" : "false",
		  m_pSupplicant != 0 && CWPASupplicant::IsConnected () ? "true" : "false", m_nRestartsTotal,
		  m_bScanning ? "true" : "false", m_nScans, m_nNets, Joins[m_Join]);
	pOut->Append (s);
}

void CWifi::Settings (CString *pOut)
{
	CNetSubSystem *pNet = CNetSubSystem::Get ();
	CString Ip ("0.0.0.0");
	boolean bUp = pNet != 0 && pNet->IsRunning ();
	if (pNet != 0) pNet->GetConfig ()->GetIPAddress ()->Format (&Ip);
	unsigned nNow = CTimer::Get ()->GetUptime ();
	CString s;
	s.Format ("{\"mode\":\"%s\",\"chip\":%s,\"starting\":%s,\"on\":%s,\"net\":\"%s\",\"up\":%s,\"ip\":\"%s\",\"cmdline\":",
		  Modes[m_Mode], m_pDev ? "true" : "false", m_bChipReq ? "true" : "false", m_bHaveConf && m_Conf.on ? "true" : "false",
		  m_pNetName ? m_pNetName : m_Mode == ModeNet ? "wifi" : "ethernet", bUp ? "true" : "false", (const char *) Ip);
	pOut->Append (s);
	if (m_pCmdline) { pOut->Append ("\""); JsonAppend (pOut, m_pCmdline); pOut->Append ("\""); }
	else pOut->Append ("null");
	pOut->Append (",\"ssid\":");
	if (m_bHaveConf && m_Conf.ssid[0]) { pOut->Append ("\""); JsonAppend (pOut, m_Conf.ssid); pOut->Append ("\""); }
	else pOut->Append ("null");
	unsigned nLeft = m_bTrial && m_Mode == ModeNet && nNow - m_nTrialT0 < TrialSecs ? TrialSecs - (nNow - m_nTrialT0) : 0;
	s.Format (",\"open\":%s,\"connected\":%s,\"scanning\":%s,\"scans\":%u,\"scanAge\":%d,\"trialSecs\":%u,"
		  "\"join\":{\"state\":\"%s\",\"reason\":\"%s\",\"left\":%u},\"networks\":[",
		  m_Conf.open ? "true" : "false", m_pSupplicant != 0 && CWPASupplicant::IsConnected () ? "true" : "false",
		  m_bScanning || m_bScanReq ? "true" : "false", m_nScans, m_nScans ? (int) (nNow - m_nScanAt) : -1, TrialSecs,
		  Joins[m_Join], m_Why, nLeft);
	pOut->Append (s);
	for (unsigned i = 0; i < m_nNets; i++)
	{
		pOut->Append (i ? ",{\"ssid\":\"" : "{\"ssid\":\"");
		JsonAppend (pOut, m_Nets[i].ssid);
		s.Format ("\",\"rssi\":%d,\"ch\":%u,\"band\":\"%s\",\"secured\":%s}", m_Nets[i].rssi, m_Nets[i].ch,
			  m_Nets[i].ch > 14 ? "5" : "2.4", m_Nets[i].secured ? "true" : "false");
		pOut->Append (s);
	}
	pOut->Append ("]}");
}

void CWifi::DoScan (void)
{
	CScheduler *S = CScheduler::Get ();
	m_bScanning = TRUE;
	unsigned t0 = CTimer::GetClockTicks ();
	static u8 Buf[FRAME_BUFFER_SIZE];
	unsigned nLen;
	while (m_pDev->ReceiveScanResult (Buf, &nLen)) {}	// stale results out
	Net Nets[MaxNets];
	unsigned nNets = 0;
	m_pDev->Control ("escan 6");
	for (unsigned ms = 0; ms <= 5000; ms += 100)		// at most 5 s, in 100 ms sleeps (core 0's other tasks run)
	{
		S->MsSleep (100);
		if (ms == 5000) m_pDev->Control ("escan 0");	// stop (as driver_circle's timeout)
		while (m_pDev->ReceiveScanResult (Buf, &nLen))
		{
			if (nLen < sizeof (TEscanResult) - sizeof (TBssInfo)) continue;
			TEscanResult *r = (TEscanResult *) Buf;
			const u8 *p = (const u8 *) &r->bss, *pEnd = Buf + nLen;
			for (unsigned i = 0; i < r->bss_count && p + sizeof (TBssInfo) <= pEnd; i++)
			{
				const TBssInfo *b = (const TBssInfo *) p;
				if (b->length < sizeof (TBssInfo)) break;
				p += b->length;
				unsigned nCh = b->ctl_ch ? b->ctl_ch : b->chanspec & 0xFF;
				nNets = add_net (Nets, nNets, MaxNets, (const char *) b->SSID, b->SSID_len, (s16) b->RSSI, nCh,
						 (b->capability & CapPrivacy) != 0);
			}
		}
	}
	sort_nets (Nets, nNets);
	memcpy (m_Nets, Nets, sizeof Nets);
	m_nNets = nNets;
	m_nScans++;
	m_nScanMs = (CTimer::GetClockTicks () - t0) / 1000;
	m_nScanAt = CTimer::Get ()->GetUptime ();
	m_bScanning = FALSE;
	CLogger::Get ()->Write (From, LogNotice, "scan: %u networks in %u ms (their names on the Wi-Fi settings only)", nNets, m_nScanMs);
}

// The join watchdog (ModeNet). wpa_supplicant retries by itself, but each failed 4-way handshake disables the network
// for 10 s, then 20-40 s, 30-60 s, 60-100 s ... (wpas_auth_failed), and a failed BSSID is ignored: a first association
// that fails a few times (an access point still holding the previous boot's session, a handshake lost while the
// chip settles) keeps Wi-Fi down for minutes although the next attempt would work (seen: ~3 min at a first boot, 27 s
// at the reboot after it). Down for JoinTimeout: the supplicant restarted from scratch (it deauthenticates, its
// backoff and ignore list start over), the timeout growing 45, 60, 90, 120 s and staying at 120 s; Circle's DHCP
// client keeps trying on its own (4 + 8 + 16 + 32 s, then again). Up (associated and the lease bound): reset.
static unsigned JoinTimeout (unsigned nRestarts)
{
	static const unsigned Secs[] = {45, 60, 90, 120};
	return Secs[nRestarts < 4 ? nRestarts : 3];
}

void CWifi::Watchdog (void)
{
	unsigned nNow = CTimer::Get ()->GetUptime ();
	CNetSubSystem *pNet = CNetSubSystem::Get ();
	boolean bUp = CWPASupplicant::IsConnected () && pNet != 0 && pNet->IsRunning ();
	if (bUp)
	{
		if (!m_bUp && m_nRestarts)
			CLogger::Get ()->Write (From, LogNotice, "joined after %u supplicant restart(s)", m_nRestarts);
		m_bUp = TRUE;
		m_nRestarts = 0;
		return;
	}
	if (m_bUp) { m_bUp = FALSE; m_nDownSince = nNow; }	// lost: wpa_supplicant reconnects, the watchdog backs it
	if (nNow - m_nDownSince < JoinTimeout (m_nRestarts)) return;
	CLogger::Get ()->Write (From, LogWarning, "not up after %u s (%s): restarting the supplicant (%u)", nNow - m_nDownSince,
				CWPASupplicant::IsConnected () ? "associated, no DHCP lease" : "not associated", m_nRestarts + 1);
	RestartSupplicant ();
	m_nRestarts++;
	m_nRestartsTotal++;
	m_nDownSince = CTimer::Get ()->GetUptime ();
}

// The join trial (a boot after a join): wpa_supplicant's own lines (the logger's event queue, its source "wpa") tell a
// wrong key (its WRONG_KEY reason, "pre-shared key may be incorrect"); association is wpa_supplicant's COMPLETED state.
void CWifi::CheckTrial (void)
{
	CLogger *L = CLogger::Get ();
	TLogSeverity Sev;
	char Source[LOG_MAX_SOURCE], Msg[LOG_MAX_MESSAGE];
	time_t Time;
	unsigned nHund;
	int nTZ;
	while (L->ReadEvent (&Sev, Source, Msg, &Time, &nHund, &nTZ))
		if (strcmp (Source, "wpa") == 0 && (strstr (Msg, "reason=WRONG_KEY") || strstr (Msg, "pre-shared key may be incorrect")))
			m_nWrongKey++;
	memset (Msg, 0, sizeof Msg);
	if (m_Mode != ModeNet || m_pSupplicant == 0) { Fail ("not_started", m_Mode == ModeNet); return; }
	CNetSubSystem *pNet = CNetSubSystem::Get ();
	boolean bAssoc = CWPASupplicant::IsConnected ();
	if (bAssoc) m_bAssocSeen = TRUE;
	unsigned nNow = CTimer::Get ()->GetUptime ();
	if (bAssoc && pNet != 0 && pNet->IsRunning ())
	{
		WriteTrial (kNone, 0);
		m_bTrial = FALSE;
		m_Join = JoinUp;
		m_bUp = TRUE;
		L->Write (From, LogNotice, "join: the new network is up in %u s (wifi.txt kept, wifi.txt.prev stays as a backup)", nNow - m_nTrialT0);
		return;
	}
	unsigned t = nNow - m_nTrialT0;
	if (t >= TrialSecs) { Fail (m_bAssocSeen ? "no_dhcp" : m_nWrongKey ? "wrong_password" : "not_found", TRUE); return; }
	if (t >= TrialRestartSecs && m_nRestarts == 0)
	{
		L->Write (From, LogWarning, "join: not up after %u s (%s): restarting the supplicant once", t,
			  bAssoc ? "associated, no DHCP lease" : "not associated");
		RestartSupplicant ();
		m_nRestarts = 1;
		m_nRestartsTotal++;
	}
}

void CWifi::Fail (const char *pWhy, boolean bReboot)
{
	boolean bBack = restore (s_Fs, ConfPath);
	m_bHaveConf = ReadConf (&m_Conf);
	WriteTrial (kFailed, pWhy);
	m_bTrial = FALSE;
	m_Join = JoinFailed;
	strncpy (m_Why, pWhy, sizeof m_Why - 1);
	m_Why[sizeof m_Why - 1] = 0;
	CLogger::Get ()->Write (From, LogWarning, "join: the new network did not come up (%s): %s%s", pWhy,
				bBack ? "the previous wifi.txt is back" : "no wifi.txt.prev to restore",
				bReboot ? ", restarting on it" : "");
	if (bReboot) m_nRebootAt = CTimer::Get ()->GetUptime () + 1;
}

void CWifi::Run (void)
{
	CScheduler *S = CScheduler::Get ();
	for (;;)
	{
		if (m_bChipReq) { if (StartChip ()) m_bScanReq = TRUE; m_bChipReq = FALSE; }
		if (m_bScanReq && m_pDev != 0
		    && (m_Mode != ModeNet || (!m_bTrial && m_pSupplicant != 0 && CWPASupplicant::IsConnected ())))
		{
			m_bScanReq = FALSE;
			DoScan ();
		}
		if (m_bTrial && m_bStarted && m_nRebootAt == 0) CheckTrial ();
		else if (m_Mode == ModeNet && m_pSupplicant != 0) Watchdog ();
		S->MsSleep (50);
	}
}
