// pi/engine/tests/test_wifi_conf.cpp - the Wi-Fi settings' portable part (wifi_conf.cpp): the join checks, wifi.txt's
// format round trip, the atomic save with wifi.txt.prev and its restore on a temp folder (a failing rename or write
// leaves the file as it was), the trial marker, the same-origin check, SSID redaction of log lines, the scan list.
// Only obviously fake networks and passwords.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "check.h"
#include "wifi_conf.h"

using namespace wificonf;

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
static int g_failRemove = 0;
static bool f_remove (const char *p) { if (g_failRemove && strstr (p, "sidecar-wifi.txt")) return false; return unlink (mac (p).c_str ()) == 0; }
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
static const Fs kFs = {f_write, f_rename, f_remove, f_exists, f_read};
static std::string slurp (const char *p)
{
	FILE *f = fopen (mac (p).c_str (), "rb");
	if (!f) return "<none>";
	std::string s;
	char b[512];
	size_t n;
	while ((n = fread (b, 1, sizeof b, f)) > 0) s.append (b, n);
	fclose (f);
	return s;
}

static const char *Fake = "not-a-real-pass";
static const char *FakeKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void test_check ()
{
	CHECK (check_join ("Fake-Net", 8, Fake, false) == kOk);
	CHECK (check_join ("Fake-Net", 8, FakeKey, false) == kOk);
	CHECK (check_join ("", 0, Fake, false) == kSsid);
	CHECK (check_join ("123456789012345678901234567890123", 33, Fake, false) == kSsid);
	CHECK (check_join ("12345678901234567890123456789012", 32, Fake, false) == kOk);
	CHECK (check_join ("bad\nname", 8, Fake, false) == kSsid);
	CHECK (check_join ("Fake-Net", 8, "short", false) == kPsk);
	CHECK (check_join ("Fake-Net", 8, "", false) == kPsk);			// open needs the flag
	CHECK (check_join ("Fake-Net", 8, "", true) == kOk);
	CHECK (check_join ("Fake-Net", 8, Fake, true) == kOpenWithKey);
	CHECK (check_join ("Fake-Net", 8, "1234567890123456789012345678901234567890123456789012345678901234", false) == kOk);	// 64 hex
	CHECK (check_join ("Fake-Net", 8, "123456789012345678901234567890123456789012345678901234567890123x", false) == kPsk);	// 64, not hex
	CHECK (check_join ("Fake-Net", 8, "fake\x01pass", false) == kPsk);
	for (Err e : {kSsid, kPsk, kOpenWithKey}) CHECK (strstr (err_text (e), Fake) == 0);
}

static void test_format ()
{
	Conf c, d;
	defaults (&c);
	c.on = true;
	strcpy (c.ssid, "Fake Net=1");
	strcpy (c.psk, FakeKey);
	c.band = 0;
	strcpy (c.ip, "10.0.0.5"); strcpy (c.mask, "255.255.255.0"); strcpy (c.gw, "10.0.0.1"); strcpy (c.dns, "10.0.0.1");
	char t[512];
	int n = format (&c, t, sizeof t);
	CHECK (n > 0 && strstr (t, "ssid=Fake Net=1\n") && strstr (t, "band=any\n") && !strstr (t, "open="));
	parse (t, &d);
	CHECK (d.on && !d.open && strcmp (d.ssid, c.ssid) == 0 && strcmp (d.psk, FakeKey) == 0 && d.band == 0);
	CHECK (strcmp (d.ip, "10.0.0.5") == 0 && strcmp (d.dns, "10.0.0.1") == 0 && strcmp (d.country, "NL") == 0);
	CHECK (has_network (&d));
	c.open = true; c.psk[0] = 0; c.ip[0] = 0;
	n = format (&c, t, sizeof t);
	CHECK (n > 0 && strstr (t, "open=1\n") && !strstr (t, "psk=") && !strstr (t, "ip="));
	parse (t, &d);
	CHECK (d.open && has_network (&d));
	CHECK (format (&c, t, 20) == -1);
	parse ("# x\r\non=1\r\nssid=Fake\r\npsk=not-a-real-pass\r\ncountry=DEU\r\nband=5g\r\n", &d);	// CRLF; a bad country kept NL
	CHECK (d.on && strcmp (d.ssid, "Fake") == 0 && strcmp (d.psk, Fake) == 0 && strcmp (d.country, "NL") == 0 && d.band == 2);
	parse ("", &d);
	CHECK (!d.on && !has_network (&d) && d.band == 1);
}

static void test_save ()
{
	char tmpl[] = "/tmp/wificonf-XXXXXX";
	CHECK (mkdtemp (tmpl));
	g_root = tmpl;
	mkdir (mac ("/.rytm").c_str (), 0755);
	const char *P = "/.rytm/wifi.txt";
	Conf a, b, r;
	defaults (&a); a.on = true; strcpy (a.ssid, "Fake-A"); strcpy (a.psk, FakeKey);
	defaults (&b); b.on = true; strcpy (b.ssid, "Fake-B"); b.open = true;
	// no wifi.txt yet (the release): .prev says off
	CHECK (save (kFs, P, &a, true));
	parse (slurp (P).c_str (), &r);
	CHECK (strcmp (r.ssid, "Fake-A") == 0);
	parse (slurp ("/.rytm/wifi.txt.prev").c_str (), &r);
	CHECK (!r.on && !has_network (&r));
	CHECK (!f_exists ("/.rytm/wifi.txt.part"));
	// A to B, A kept
	CHECK (save (kFs, P, &b, true));
	parse (slurp (P).c_str (), &r);
	CHECK (strcmp (r.ssid, "Fake-B") == 0 && r.open);
	parse (slurp ("/.rytm/wifi.txt.prev").c_str (), &r);
	CHECK (strcmp (r.ssid, "Fake-A") == 0);
	// restore: A back, no .prev left
	CHECK (restore (kFs, P));
	parse (slurp (P).c_str (), &r);
	CHECK (strcmp (r.ssid, "Fake-A") == 0 && !f_exists ("/.rytm/wifi.txt.prev"));
	CHECK (!restore (kFs, P));
	// a failing write or rename: the file as it was
	std::string before = slurp (P);
	g_failWrite = 1;
	CHECK (!save (kFs, P, &b, true));
	CHECK (slurp (P) == before && !f_exists ("/.rytm/wifi.txt.part"));
	g_failRename = 2;			// the second rename (.part to wifi.txt): wifi.txt.prev goes back
	CHECK (!save (kFs, P, &b, true));
	CHECK (slurp (P) == before && !f_exists ("/.rytm/wifi.txt.part"));
	// no prev: in place (wifi=on/off/band), .prev untouched
	CHECK (save (kFs, P, &b, true));
	std::string prev = slurp ("/.rytm/wifi.txt.prev");
	b.on = false;
	CHECK (save (kFs, P, &b, false));
	parse (slurp (P).c_str (), &r);
	CHECK (!r.on && slurp ("/.rytm/wifi.txt.prev") == prev);
	// recover: cut between the renames
	f_rename (P, "/.rytm/wifi.txt.part");
	recover (kFs, P);
	parse (slurp (P).c_str (), &r);
	CHECK (strcmp (r.ssid, "Fake-B") == 0 && !f_exists ("/.rytm/wifi.txt.part"));
	// the trial marker
	char t[128], why[24];
	CHECK (format_trial (kTrying, 0, t, sizeof t) > 0 && parse_trial (t, why, sizeof why) == kTrying && why[0] == 0);
	CHECK (format_trial (kFailed, "wrong_password", t, sizeof t) > 0 && parse_trial (t, why, sizeof why) == kFailed);
	CHECK (strcmp (why, "wrong_password") == 0);
	CHECK (parse_trial ("state=failed\nreason=<b>x\"y</b>\n", why, sizeof why) == kFailed && strcmp (why, "bxyb") == 0);
	CHECK (parse_trial ("junk", why, sizeof why) == kNone);
	std::string cmd = "rm -rf " + g_root;
	CHECK (system (cmd.c_str ()) == 0);
}

static void fakeKey (const char *pass, const char *ssid, char key[65])
{
	for (int i = 0; i < 64; i++) key[i] = "0123456789abcdef"[(pass[i % strlen (pass)] + ssid[0] + i) & 15];
	key[64] = 0;
}
static void put (const char *p, const std::string &s) { CHECK (f_write (p, s.data (), (unsigned) s.size ())); }

static void test_import ()
{
	char tmpl[] = "/tmp/wificonf-XXXXXX";
	CHECK (mkdtemp (tmpl));
	g_root = tmpl;
	mkdir (mac ("/.rytm").c_str (), 0755);
	const char *P = "/.rytm/wifi.txt", *T = "/.rytm/wifi.try";
	Conf cur, r;
	char why[160];
	defaults (&cur);
	CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kNoFile);
	// variants: plain, CRLF + BOM + spaces + quotes + comment + alias + case, the 2g default
	const char *ok[] = {
		"ssid=Fake Net\npsk=not-a-real-pass\n",
		"\xEF\xBB\xBF# my network\r\n  SSID = \"Fake Net\"  \r\n Password = 'not-a-real-pass'\r\nband=2G\r\n",
		"ssid=Fake Net\npsk=not-a-real-pass",
	};
	for (const char *t : ok)
	{
		put (kImportPath, t);
		put (kImportErrPath, "old");
		CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kImported);
		CHECK (!f_exists (kImportPath) && !f_exists (kImportErrPath) && !f_exists (kImportUsedPath));
		std::string w = slurp (P);
		parse (w.c_str (), &r);
		char k[65];
		fakeKey ("not-a-real-pass", "Fake Net", k);
		CHECK (r.on && !r.open && strcmp (r.ssid, "Fake Net") == 0 && strcmp (r.psk, k) == 0 && r.band == 1);
		CHECK (w.find ("not-a-real-pass") == std::string::npos);
		CHECK (slurp (T).find ("trying") != std::string::npos);
		CHECK (strcmp (cur.psk, k) == 0);
		CHECK (f_exists ("/.rytm/wifi.txt.prev"));
	}
	// band, hex key kept as is, open
	put (kImportPath, "ssid=Fake-5\npsk=" + std::string (FakeKey) + "\nband=5g\n");
	CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kImported);
	parse (slurp (P).c_str (), &r);
	CHECK (strcmp (r.psk, FakeKey) == 0 && r.band == 2);
	put (kImportPath, "ssid=Fake-Open\nopen=1\nband=any\n");
	CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kImported);
	parse (slurp (P).c_str (), &r);
	CHECK (r.open && r.psk[0] == 0 && r.band == 0 && has_network (&r));
	// rejections: file deleted, error file with a reason, never the SSID or password; wifi.txt untouched
	std::string before = slurp (P);
	const char *bad[] = {
		"ssid=SecretNetName\npsk=short\n", "ssid=SecretNetName\n", "psk=not-a-real-pass\n",
		"ssid=SecretNetName\npsk=not-a-real-pass\nband=6g\n", "ssid=SecretNetName\nopen=1\npsk=not-a-real-pass\n",
		"ssid=123456789012345678901234567890123\npsk=not-a-real-pass\n", "",
	};
	for (const char *t : bad)
	{
		put (kImportPath, t);
		CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kRejected);
		CHECK (!f_exists (kImportPath) && why[0]);
		std::string e = slurp (kImportErrPath);
		CHECK (e != "<none>" && e.find ("SecretNetName") == std::string::npos && e.find ("not-a-real-pass") == std::string::npos);
		CHECK (slurp (P) == before);
	}
	put (kImportPath, "ssid=SecretNetName\npsk=short\n");
	CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kRejected);
	CHECK (slurp (kImportErrPath).find ("8-63") != std::string::npos);
	// a failed delete: overwritten, renamed to .used
	g_failRemove = 1;
	put (kImportPath, "ssid=Fake Net\npsk=not-a-real-pass\n");
	CHECK (import_card (kFs, P, T, fakeKey, &cur, why, sizeof why) == kImported);
	g_failRemove = 0;
	CHECK (!f_exists (kImportPath) && f_exists (kImportUsedPath));
	CHECK (slurp (kImportUsedPath).find ("not-a-real-pass") == std::string::npos);
	CHECK (!f_exists (kImportErrPath));
	std::string cmd = "rm -rf " + g_root;
	CHECK (system (cmd.c_str ()) == 0);
}

static void test_origin ()
{
	const char *names[] = {"sidecar.local", "192.168.1.20"};
	CHECK (same_origin ("sidecar.local", "http://sidecar.local", 0, names, 2));
	CHECK (same_origin ("SIDECAR.local", "http://sidecar.LOCAL", "", names, 2));
	CHECK (same_origin ("192.168.1.20", "", "http://192.168.1.20/#status", names, 2));
	CHECK (same_origin ("sidecar.local:80", "http://sidecar.local:80", 0, names, 2));
	CHECK (!same_origin ("sidecar.local", "http://evil.example", 0, names, 2));
	CHECK (!same_origin ("sidecar.local", "http://sidecar.local.evil.example", 0, names, 2));
	CHECK (!same_origin ("sidecar.local", "null", 0, names, 2));
	CHECK (!same_origin ("sidecar.local", "", "http://evil.example/sidecar.local", names, 2));
	CHECK (!same_origin ("sidecar.local", "", "http://sidecar.localx/", names, 2));
	CHECK (!same_origin ("sidecar.local", 0, 0, names, 2));			// no header: refused
	CHECK (!same_origin ("evil.example", "http://evil.example", 0, names, 2));	// DNS rebinding
	CHECK (!same_origin ("", "http://sidecar.local", 0, names, 2));
	CHECK (!same_origin ("sidecar.local:8x", "http://sidecar.local:8x", 0, names, 2));
	CHECK (!same_origin ("sidecar.local", "https://sidecar.local", 0, names, 2));
	CHECK (not_cross_site ("sidecar.local", 0, 0, names, 2));			// curl
	// the Host, Origin and Referer a browser sends for the page's POST /cmd (as recorded from Chrome 153 on macOS,
	// 2026-11-06), the page opened at the address and at sidecar.local: both checks pass (the 0.4.1 rc refusals were
	// the daemon's case-sensitive Content-Type, pi/circle/circle-http-case.patch, not these)
	{
		const char *const ip[] = {"sidecar.local", "169.254.61.60"};
		CHECK (same_origin ("169.254.61.60", "http://169.254.61.60", "http://169.254.61.60/", ip, 2));
		CHECK (not_cross_site ("169.254.61.60", "http://169.254.61.60", "http://169.254.61.60/", ip, 2));
		CHECK (same_origin ("sidecar.local", "http://sidecar.local", "http://sidecar.local/", ip, 2));
		CHECK (not_cross_site ("sidecar.local", "http://sidecar.local", "http://sidecar.local/#sound", ip, 2));
		CHECK (same_origin ("169.254.61.60", "http://169.254.61.60", 0, ip, 2));	// Referrer-Policy no-referrer
	}
	CHECK (!not_cross_site ("sidecar.local", "http://evil.example", 0, names, 2));
}

static void test_redact ()
{
	auto R = [] (const char *in) { static char b[256]; strcpy (b, in); unsigned n = redact_ssids (b, strlen (b)); b[n] = 0; return std::string (b); };
	CHECK (R ("wpa: wlan0: Trying to associate with 00:11:22:33:44:55 (SSID='Fake Net' freq=2412 MHz)")
	       == "wpa: wlan0: Trying to associate with 00:11:22:33:44:55 (SSID='*' freq=2412 MHz)");
	CHECK (R ("wpa: CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Fake\" auth_failures=1 duration=10 reason=WRONG_KEY")
	       == "wpa: CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"*\" auth_failures=1 duration=10 reason=WRONG_KEY");
	CHECK (R ("wpa: Trying to associate with SSID 'it's fake'\n") == "wpa: Trying to associate with SSID '*'\n");
	CHECK (R ("wpa: cut SSID 'Fake") == "wpa: cut SSID '*");
	CHECK (R ("engine: nothing here, ssid none") == "engine: nothing here, ssid none");
	CHECK (R ("SSID=''") == "SSID=''");
}

static void test_nets ()
{
	Net n[3];
	unsigned c = 0;
	c = add_net (n, c, 3, "Fake-1", 6, -70, 1, true);
	c = add_net (n, c, 3, "Fake-2", 6, -40, 36, false);
	c = add_net (n, c, 3, "Fake-1", 6, -50, 6, true);		// the stronger one kept
	c = add_net (n, c, 3, "", 0, -30, 1, true);			// hidden
	c = add_net (n, c, 3, "\0\0\0", 3, -30, 1, true);		// hidden (padded)
	c = add_net (n, c, 3, "Fake-3", 6, -90, 11, true);
	c = add_net (n, c, 3, "Fake-4", 6, -20, 11, true);		// full
	CHECK (c == 3 && n[0].rssi == -50 && n[0].ch == 6);
	sort_nets (n, c);
	CHECK (strcmp (n[0].ssid, "Fake-2") == 0 && strcmp (n[1].ssid, "Fake-1") == 0 && strcmp (n[2].ssid, "Fake-3") == 0);
	char j[16];
	CHECK (json_escape ("a\"b\\c\x01", j, sizeof j) == 13 && strcmp (j, "a\\\"b\\\\c\\u0001") == 0);
	CHECK (json_escape ("a\"b\\c\x01", j, 7) == 6 && strcmp (j, "a\\\"b\\\\") == 0);	// never cut in an escape
}

int main ()
{
	test_check ();
	test_format ();
	test_save ();
	test_import ();
	test_origin ();
	test_redact ();
	test_nets ();
	printf ("test_wifi_conf: ok\n");
	return 0;
}
