// pi/engine/wifi.h - Wi-Fi on the Pi 4's BCM43455 (Circle's addon/wlan + its wpa_supplicant port), docs/research/pi-wifi.md
// and docs/pi-engine.md "Wi-Fi settings". Off by default.
//
// Circle runs one network stack on one device (CNetSubSystem is a singleton, wpa_supplicant's l2_packet sends
// through it), so Wi-Fi and Ethernet are either/or for the network; the chip itself can run beside Ethernet:
//   wifi=0     (cmdline.txt) off: no Wi-Fi code runs, nothing loaded
//   wifi=scan  the chip up beside Ethernet (Ethernet stays the network): scans for the settings page
//   wifi=1     Wi-Fi is the network: joins the network in /.rytm/wifi.txt (DHCP unless wifi.txt has ip=),
//              sidecar.local over Wi-Fi; Ethernet unused. No stored network: as wifi=scan.
// No wifi= in cmdline.txt (the release): wifi.txt's on=1 means wifi=1 (the web page, the console or the display enable it).
//
// /.rytm/wifi.txt (wifi_conf.h), key=value lines ('#' comments): on=0|1, ssid=, psk= (the 64-hex key; a passphrase
// written by hand is hashed at boot), open=1 (an open network), country= (ISO alpha2, default NL), band=2g|5g|any,
// ip= netmask= gw= dns= (all four: static, else DHCP). Firmware: SD:/firmware/brcmfmac43455-sdio.{bin,txt,clm_blob}.
//
// A join (the page's Connect and save, the console's wifi=join, wifi=on with a stored network) is a trial: wifi.txt is
// replaced atomically, the old one kept as wifi.txt.prev, /.rytm/wifi.try says "trying"; the page's join restarts the
// Pi on Wi-Fi now (one network at a time), the console's at the next boot. Not up (associated and a DHCP lease) within
// TrialSecs of that boot: wifi.txt.prev goes back, wifi.try says "failed" with the reason (wrong_password, not_found,
// no_dhcp, not_started) and the Pi restarts on its previous setting - Ethernet when Wi-Fi was off - so a typo cannot
// lock it out.
//
// Credentials: the passphrase is never logged, stored, echoed or put in /status; SSIDs only in the page's Wi-Fi
// settings (GET /wifi/status), never in /status or the log (web.cpp's log tee also blanks wpa_supplicant's).
//
// Console (serial ':' lines and the web page's /cmd; core 0): wifi (status), wifi=scan, wifi=on, wifi=off,
// wifi=band:2g|5g|any, wifi=join:SSID:PASS (SSID up to the first ':'; the passphrase is never logged). on/off/join take
// effect at the next boot; on in the off mode also brings the chip up for scans now.
#ifndef _wifi_h
#define _wifi_h
#include <circle/koptions.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle/types.h>

#include "wifi_conf.h"

class CBcm4343Device;
class CWPASupplicant;

class CWifi : public CTask
{
public:
	enum TMode { ModeOff, ModeScan, ModeNet };
	static const unsigned TrialSecs = 90;		// a join's window (the first association after a boot took 27 s .. 3 min)
	static const unsigned TrialRestartSecs = 40;	// the supplicant restarted once inside it (the watchdog's cure)

	CWifi (void);					// after the card is mounted (reads wifi.txt), before the network
	static CWifi *Get (void) { return s_pThis; }

	TMode BootMode (CKernelOptions *pOptions);	// cmdline wifi=, else wifi.txt on=; ModeNet needs a stored network
	boolean StartChip (void);			// the firmware loaded (blocking, ~1-2 s); before CNetSubSystem::Initialize
	boolean StaticIP (u8 IP[4], u8 Mask[4], u8 GW[4], u8 DNS[4]) const;	// wifi.txt's ip= etc. (ModeNet), else DHCP
	void StartSupplicant (boolean bNetOK);		// after CNetSubSystem::Initialize (always: the trial waits for it); ModeNet: joins
	TMode Mode (void) const { return m_Mode; }
	boolean TrialPending (void) const { return m_bTrial; }	// wifi.try says "trying": a join is judged this boot
	// the network runs on another interface this boot (usbnet_pi.h: "usb", or "ethernet" for net=lan); before
	// StartSupplicant. A stored network is not joined (the chip stays up for scans: Wi-Fi can be set up from the page),
	// and a join's trial waits for a boot on Wi-Fi (wifi.try is left as it is)
	void OtherNet (const char *pName);

	boolean Command (const char *pCmd);		// "wifi" or "wifi=...": TRUE if it was one
	static boolean IsSecret (const char *pCmd);	// a command that must not be logged as typed (wifi=join:)
	void Status (CString *pOut);			// /status's "wifi" member: no SSID

	// The settings page (web.cpp, core 0's web tasks). 0: done, else the reason (never with the input in it).
	const char *Join (const char *pSSID, unsigned nSSID, const char *pPSK, boolean bOpen);	// stored, then a restart
	const char *Forget (void);			// the network out of wifi.txt (and wifi.txt.prev), Wi-Fi off
	const char *Power (boolean bOn);		// on=1|0 (the next boot); on also starts the chip for scans now
	const char *Scan (void);			// a scan started (Run), the list in Settings
	void Settings (CString *pOut);			// GET /wifi/status: the stored SSID, the scan list, the join's state
	boolean TakeReboot (void);			// the kernel's loop: restart now (a join, or a failed one's revert)

	void Run (void) override;			// the scans, a chip started later (wifi=on), the join trial, the watchdog

private:
	static boolean ReadConf (wificonf::Conf *pConf);
	static boolean WriteConf (const wificonf::Conf *pConf, boolean bKeepPrev);
	static void WriteTrial (wificonf::Trial t, const char *pWhy);
	const char *Store (const char *pSSID, unsigned nSSID, const char *pPSK, boolean bOpen);	// a trial at the next boot
	const char *Busy (void) const;
	boolean WriteSupplicantConf (void);
	void RestartSupplicant (void);
	void DoScan (void);
	void Watchdog (void);
	void CheckTrial (void);
	void Fail (const char *pWhy, boolean bReboot);

private:
	TMode m_Mode;
	wificonf::Conf m_Conf;
	boolean m_bHaveConf;
	const char *m_pCmdline;				// cmdline.txt's wifi=, 0 without
	const char *m_pNetName;				// OtherNet: Settings' "net", 0: Wi-Fi or Ethernet as the mode says
	CBcm4343Device *m_pDev;
	CWPASupplicant *m_pSupplicant;
	volatile boolean m_bScanReq, m_bChipReq, m_bScanning;
	unsigned m_nScans, m_nScanMs, m_nScanAt;
	// ModeNet's join watchdog (Run): the network down (not associated, or no DHCP lease) since m_nDownSince (uptime
	// s); after JoinTimeout (m_nRestarts) the supplicant is restarted with a fresh state (its auth-failure backoff,
	// 10 s growing to minutes, and its BSSID ignore list cleared). m_nRestarts: since the last time it was up.
	unsigned m_nDownSince, m_nRestarts, m_nRestartsTotal;
	boolean m_bUp;

	// the join trial (wifi.try) and the page's view of it
	enum TJoin { JoinNone, JoinRestarting, JoinTrying, JoinUp, JoinFailed };
	TJoin m_Join;
	char m_Why[24];					// a reason token (wifi_conf's parse_trial)
	boolean m_bTrial, m_bStarted, m_bAssocSeen;
	unsigned m_nTrialT0, m_nWrongKey;
	unsigned m_nRebootAt;				// uptime s, 0: none

	static const unsigned MaxNets = 24;
	wificonf::Net m_Nets[MaxNets];
	unsigned m_nNets;

	static CWifi *s_pThis;
};
#endif
