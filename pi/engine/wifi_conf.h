// pi/engine/wifi_conf.h - the Wi-Fi settings' portable part (wifi.cpp on the Pi; pi/engine/tests/test_wifi_conf.cpp
// on the Mac): /.sidecar/wifi.txt's format, the join checks, the atomic save with the previous file kept as
// wifi.txt.prev, the join trial's marker (wifi.try), the same-origin check of the page's join/forget requests, the
// SSID redaction of log lines, and the scan list. No Circle, no allocation, no printf.
//
// Credentials: wifi.txt holds the 64-hex WPA key (PBKDF2 of the passphrase, made in wifi.cpp), never the passphrase.
// Nothing here logs or returns a key; err_text() never quotes the input.
#ifndef _pi_engine_wifi_conf_h
#define _pi_engine_wifi_conf_h

namespace wificonf {

constexpr unsigned kSsidMax = 32;

struct Conf
{
	bool on;			// on=1: Wi-Fi is the network at the next boot (with a network), else the chip only
	bool open;			// open=1: an open network (no key)
	char ssid[kSsidMax + 1];
	char psk[65];			// an 8-63 character passphrase or the 64-hex key ("" when open)
	char country[3];
	unsigned band;			// 0 any, 1 2g (the default), 2 5g
	char ip[16], mask[16], gw[16], dns[16];	// all four: static, else DHCP
};
extern const char *const kBandNames[3];

void defaults (Conf *c);				// off, no network, NL, 2g
void parse (const char *text, Conf *c);			// wifi.txt's key=value lines ('#' comments) over defaults
int format (const Conf *c, char *out, unsigned size);	// wifi.txt; its length, -1 if it does not fit
unsigned band_from_name (const char *p, unsigned nDefault);

bool is_hex64 (const char *p);
bool valid_psk (const char *p);			// 8-63 printable ASCII, or 64 hex digits
bool has_network (const Conf *c);		// an SSID and a valid key (or open)

enum Err { kOk, kSsid, kPsk, kOpenWithKey };
// A join's input: SSID 1-32 bytes (no control characters); a key 8-63 printable ASCII or 64 hex unless open; open
// needs the explicit flag and no key
Err check_join (const char *ssid, unsigned nSsid, const char *psk, bool open);
const char *err_text (Err e);			// for the page; never contains the input

// The card, without its drive (wifi.cpp: FatFs under "SD:"; the tests: a temp folder)
struct Fs
{
	bool (*write) (const char *path, const char *data, unsigned n);	// create or truncate
	bool (*rename) (const char *from, const char *to);		// to must not exist (FatFs)
	bool (*remove) (const char *path);
	bool (*exists) (const char *path);
	bool (*read) (const char *path, char *out, unsigned size);	// NUL-terminated, at most size-1 bytes (import_card only)
};
// path.part written, then it replaces path. keep_prev: the current path becomes path.prev first (an earlier .prev
// replaced); with no current file, path.prev says Wi-Fi off ("on=0"), so restoring it means "as before: off".
// FALSE: path is as it was.
bool save (const Fs &fs, const char *path, const Conf *c, bool keep_prev);
bool restore (const Fs &fs, const char *path);		// path.prev back in place of path
void recover (const Fs &fs, const char *path);		// a save cut short between its renames: path.part finished

// The join trial (wifi.try): "state=trying" from the join until the network is up; "state=failed" with reason=
// after a revert (shown on the page until the next join or forget)
enum Trial { kNone, kTrying, kFailed };
Trial parse_trial (const char *text, char *reason, unsigned nReason);
int format_trial (Trial t, const char *reason, char *out, unsigned size);

// The first-setup file at the card root (visible in the Mac Finder): sidecar-wifi.txt with ssid=, psk= (alias password=),
// optional band=2g|5g|any (default 2g) and open=1. '#' comments; CRLF, a UTF-8 BOM, spaces around '=' and quotes around
// values are tolerated. import_card runs at boot before Wi-Fi starts: no file: kNoFile; else the file is checked with
// check_join and imported like the page's join (wifi.txt via save() with on=1 and the derived key - never the
// passphrase -, the old one kept as wifi.txt.prev, wifi.try "trying"), then ALWAYS deleted (failed delete: contents
// overwritten, then renamed to sidecar-wifi.used). Success removes sidecar-wifi-error.txt; a rejection writes it with a
// short reason (never the SSID or password). *cur becomes the imported setting on success. why: the reason (never the input).
constexpr const char *kImportPath = "/sidecar-wifi.txt";
constexpr const char *kImportErrPath = "/sidecar-wifi-error.txt";
constexpr const char *kImportUsedPath = "/sidecar-wifi.used";
enum Import { kNoFile, kImported, kRejected };
typedef void (*KeyFn) (const char *pass, const char *ssid, char key[65]);	// the passphrase's 64-hex WPA key
Import import_card (const Fs &fs, const char *confPath, const char *tryPath, KeyFn key, Conf *cur, char *why, unsigned nWhy);

// A request that changes the Wi-Fi settings (join, forget, on/off) comes from the page itself: the Host header is one
// of the names the Pi answers to (names: "sidecar.local", its IP; a port allowed), so a DNS rebinding page is refused,
// and the Origin header (else the Referer) is http://<that host>. Neither header: refused (browsers send Origin with
// every POST from a page; this stops another site's page from posting to sidecar.local).
bool same_origin (const char *host, const char *origin, const char *referer, const char *const *names, unsigned nNames);
// The same, lenient for tools: TRUE when neither Origin nor Referer is sent (curl), else as same_origin
bool not_cross_site (const char *host, const char *origin, const char *referer, const char *const *names, unsigned nNames);

// A log line with an SSID in it (wpa_supplicant's "SSID 'x'", ssid="x", SSID='x'): the name replaced by "*", in
// place up to the last matching quote of the line. Its new length.
unsigned redact_ssids (char *line, unsigned n);

// The scan list: one entry per SSID (the strongest), hidden networks left out, at most max
struct Net
{
	char ssid[kSsidMax + 1];
	int rssi;
	unsigned ch;
	bool secured;
};
unsigned add_net (Net *nets, unsigned n, unsigned max, const char *ssid, unsigned nSsid, int rssi, unsigned ch, bool secured);
void sort_nets (Net *nets, unsigned n);		// strongest first
// a JSON string's body: quotes, backslashes and control characters escaped; at most size-1 bytes, never cut in an
// escape. Its length.
unsigned json_escape (const char *p, char *out, unsigned size);

}

#endif
