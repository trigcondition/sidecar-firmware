// pi/engine/wifi_conf.cpp - see wifi_conf.h.
#include "wifi_conf.h"

#include <string.h>

namespace wificonf {

const char *const kBandNames[3] = {"any", "2g", "5g"};

static void copy (char *out, unsigned size, const char *p, unsigned n)
{
	if (n > size - 1) n = size - 1;
	memcpy (out, p, n);
	out[n] = 0;
}

static char lower (char c) { return c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c; }

static bool ieq (const char *a, const char *b, unsigned n)	// n bytes, case-insensitive
{
	for (unsigned i = 0; i < n; i++) if (lower (a[i]) != lower (b[i])) return false;
	return true;
}

void defaults (Conf *c)
{
	memset (c, 0, sizeof *c);
	strcpy (c->country, "NL");
	c->band = 1;
}

unsigned band_from_name (const char *p, unsigned nDefault)
{
	for (unsigned i = 0; i < 3; i++) if (strcmp (p, kBandNames[i]) == 0) return i;
	return nDefault;
}

void parse (const char *text, Conf *c)
{
	defaults (c);
	while (*text)
	{
		const char *e = strchr (text, '\n');
		unsigned n = e ? (unsigned) (e - text) : (unsigned) strlen (text);
		char line[160];
		copy (line, sizeof line, text, n);
		text += n + (e ? 1 : 0);
		unsigned m = strlen (line);
		while (m > 0 && (line[m - 1] == '\r' || line[m - 1] == ' ')) line[--m] = 0;
		char *v = strchr (line, '=');
		if (line[0] == '#' || v == 0) { memset (line, 0, sizeof line); continue; }
		*v++ = 0;
		const char *k = line;
		if (strcmp (k, "on") == 0) c->on = *v == '1';
		else if (strcmp (k, "open") == 0) c->open = *v == '1';
		else if (strcmp (k, "ssid") == 0) copy (c->ssid, sizeof c->ssid, v, strlen (v));
		else if (strcmp (k, "psk") == 0) copy (c->psk, sizeof c->psk, v, strlen (v));
		else if (strcmp (k, "country") == 0 && strlen (v) == 2) copy (c->country, sizeof c->country, v, 2);
		else if (strcmp (k, "band") == 0) c->band = band_from_name (v, c->band);
		else if (strcmp (k, "ip") == 0) copy (c->ip, sizeof c->ip, v, strlen (v));
		else if (strcmp (k, "netmask") == 0) copy (c->mask, sizeof c->mask, v, strlen (v));
		else if (strcmp (k, "gw") == 0) copy (c->gw, sizeof c->gw, v, strlen (v));
		else if (strcmp (k, "dns") == 0) copy (c->dns, sizeof c->dns, v, strlen (v));
		memset (line, 0, sizeof line);
	}
}

// appends p to out (n: its length so far); false when it does not fit
static bool put (char *out, unsigned size, unsigned *n, const char *p)
{
	unsigned l = strlen (p);
	if (*n + l + 1 > size) return false;
	memcpy (out + *n, p, l + 1);
	*n += l;
	return true;
}

int format (const Conf *c, char *out, unsigned size)
{
	unsigned n = 0;
	if (size == 0) return -1;
	out[0] = 0;
	bool ok = put (out, size, &n, "# Sidecar Wi-Fi (docs/research/pi-wifi.md)\non=") && put (out, size, &n, c->on ? "1" : "0")
		  && put (out, size, &n, "\ncountry=") && put (out, size, &n, c->country)
		  && put (out, size, &n, "\nband=") && put (out, size, &n, kBandNames[c->band < 3 ? c->band : 1])
		  && put (out, size, &n, "\n");
	if (ok && c->ssid[0])
	{
		ok = put (out, size, &n, "ssid=") && put (out, size, &n, c->ssid) && put (out, size, &n, "\n");
		if (ok && c->open) ok = put (out, size, &n, "open=1\n");
		else if (ok) ok = put (out, size, &n, "psk=") && put (out, size, &n, c->psk) && put (out, size, &n, "\n");
	}
	if (ok && c->ip[0])
		ok = put (out, size, &n, "ip=") && put (out, size, &n, c->ip) && put (out, size, &n, "\nnetmask=")
		     && put (out, size, &n, c->mask) && put (out, size, &n, "\ngw=") && put (out, size, &n, c->gw)
		     && put (out, size, &n, "\ndns=") && put (out, size, &n, c->dns) && put (out, size, &n, "\n");
	if (!ok) { memset (out, 0, size); return -1; }
	return (int) n;
}

bool is_hex64 (const char *p)
{
	unsigned n = 0;
	for (; *p; p++, n++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
	return n == 64;
}

bool valid_psk (const char *p)
{
	if (is_hex64 (p)) return true;
	unsigned n = strlen (p);
	if (n < 8 || n > 63) return false;
	for (; *p; p++) if (*p < 0x20 || *p > 0x7e) return false;
	return true;
}

bool has_network (const Conf *c)
{
	return c->ssid[0] != 0 && (c->open ? c->psk[0] == 0 : valid_psk (c->psk));
}

Err check_join (const char *ssid, unsigned nSsid, const char *psk, bool open)
{
	if (nSsid == 0 || nSsid > kSsidMax) return kSsid;
	for (unsigned i = 0; i < nSsid; i++) if ((unsigned char) ssid[i] < 0x20 || ssid[i] == 0x7f) return kSsid;
	if (open) return psk[0] ? kOpenWithKey : kOk;
	return valid_psk (psk) ? kOk : kPsk;
}

const char *err_text (Err e)
{
	switch (e)
	{
	case kOk:		return "ok";
	case kSsid:		return "the network name must be 1-32 bytes, no control characters";
	case kPsk:		return "the password must be 8-63 characters (printable ASCII) or 64 hex digits; an open network needs \"open network\" ticked";
	case kOpenWithKey:	return "an open network takes no password";
	}
	return "invalid";
}

static void with (char *out, unsigned size, const char *path, const char *ext)
{
	out[0] = 0;
	if (strlen (path) + strlen (ext) + 1 > size) return;
	strcpy (out, path);
	strcat (out, ext);
}

bool save (const Fs &fs, const char *path, const Conf *c, bool keep_prev)
{
	char text[512], part[96], prev[96];
	with (part, sizeof part, path, ".part");
	with (prev, sizeof prev, path, ".prev");
	if (!part[0] || !prev[0]) return false;
	int n = format (c, text, sizeof text);
	bool ok = n > 0 && fs.write (part, text, (unsigned) n);
	memset (text, 0, sizeof text);
	if (!ok) { fs.remove (part); return false; }
	bool had = fs.exists (path);
	if (keep_prev)
	{
		if (fs.exists (prev) && !fs.remove (prev)) { fs.remove (part); return false; }
		if (had) ok = fs.rename (path, prev);
		else
		{
			Conf off;
			defaults (&off);
			n = format (&off, text, sizeof text);
			ok = n > 0 && fs.write (prev, text, (unsigned) n);
		}
		if (!ok) { fs.remove (part); return false; }
	}
	else if (had && !fs.remove (path)) { fs.remove (part); return false; }
	if (fs.rename (part, path)) return true;
	if (keep_prev && had) fs.rename (prev, path);	// back as it was
	fs.remove (part);
	return false;
}

bool restore (const Fs &fs, const char *path)
{
	char prev[96];
	with (prev, sizeof prev, path, ".prev");
	if (!prev[0] || !fs.exists (prev)) return false;
	if (fs.exists (path) && !fs.remove (path)) return false;
	return fs.rename (prev, path);
}

void recover (const Fs &fs, const char *path)
{
	char part[96];
	with (part, sizeof part, path, ".part");
	if (!part[0] || fs.exists (path) || !fs.exists (part)) return;
	fs.rename (part, path);		// complete: path is removed or renamed only after .part was written
}

Trial parse_trial (const char *text, char *reason, unsigned nReason)
{
	Trial t = kNone;
	if (nReason) reason[0] = 0;
	while (*text)
	{
		const char *e = strchr (text, '\n');
		unsigned n = e ? (unsigned) (e - text) : (unsigned) strlen (text);
		if (n >= 6 && strncmp (text, "state=", 6) == 0)
		{
			if (n == 12 && strncmp (text + 6, "trying", 6) == 0) t = kTrying;
			else if (n == 12 && strncmp (text + 6, "failed", 6) == 0) t = kFailed;
		}
		else if (n >= 7 && strncmp (text, "reason=", 7) == 0 && nReason)
		{
			unsigned m = 0;
			for (unsigned i = 7; i < n && m + 1 < nReason; i++)
			{
				char c = text[i];
				if ((c >= 'a' && c <= 'z') || c == '_') reason[m++] = c;	// a token, never free text
			}
			reason[m] = 0;
		}
		text += n + (e ? 1 : 0);
	}
	return t;
}

int format_trial (Trial t, const char *reason, char *out, unsigned size)
{
	unsigned n = 0;
	if (size == 0) return -1;
	out[0] = 0;
	bool ok = put (out, size, &n, "# Sidecar Wi-Fi join (wifi.cpp)\nstate=")
		  && put (out, size, &n, t == kTrying ? "trying" : t == kFailed ? "failed" : "none") && put (out, size, &n, "\n");
	if (ok && t == kFailed && reason && reason[0])
		ok = put (out, size, &n, "reason=") && put (out, size, &n, reason) && put (out, size, &n, "\n");
	return ok ? (int) n : -1;
}

static bool host_ok (const char *host, const char *const *names, unsigned nNames)
{
	if (host == 0 || host[0] == 0) return false;
	const char *c = strchr (host, ':');
	unsigned n = c ? (unsigned) (c - host) : (unsigned) strlen (host);
	if (c != 0)
		for (const char *p = c + 1; *p; p++) if (*p < '0' || *p > '9') return false;
	for (unsigned i = 0; i < nNames; i++)
		if (names[i] && names[i][0] && strlen (names[i]) == n && ieq (host, names[i], n)) return true;
	return false;
}

static bool from_host (const char *url, const char *host, bool path)	// url is http://<host>, then (path) '/' or nothing
{
	unsigned h = strlen (host);
	if (strncmp (url, "http://", 7) != 0 || strlen (url) < 7 + h || !ieq (url + 7, host, h)) return false;
	char next = url[7 + h];
	return next == 0 || (path && next == '/');
}

bool same_origin (const char *host, const char *origin, const char *referer, const char *const *names, unsigned nNames)
{
	if (!host_ok (host, names, nNames)) return false;
	if (origin && origin[0]) return from_host (origin, host, false);	// "null" too: refused
	if (referer && referer[0]) return from_host (referer, host, true);
	return false;
}

bool not_cross_site (const char *host, const char *origin, const char *referer, const char *const *names, unsigned nNames)
{
	if ((origin == 0 || origin[0] == 0) && (referer == 0 || referer[0] == 0)) return true;
	return same_origin (host, origin, referer, names, nNames);
}

unsigned redact_ssids (char *line, unsigned n)
{
	for (unsigned i = 0; i + 6 <= n; i++)
	{
		if (!ieq (line + i, "ssid", 4) || (line[i + 4] != ' ' && line[i + 4] != '=')) continue;
		char q = line[i + 5];
		if (q != '\'' && q != '"') continue;
		unsigned a = i + 6, b = n;		// the name: a .. the last q of the line (none: the end, a cut line)
		for (unsigned k = n; k > a; k--) if (line[k - 1] == q) { b = k - 1; break; }
		if (b <= a) continue;
		unsigned tail = n - b;
		line[a] = '*';
		memmove (line + a + 1, line + b, tail);
		n = a + 1 + tail;
		i = a;
	}
	return n;
}

unsigned add_net (Net *nets, unsigned n, unsigned max, const char *ssid, unsigned nSsid, int rssi, unsigned ch, bool secured)
{
	if (nSsid == 0 || nSsid > kSsidMax) return n;	// hidden (or junk)
	char s[kSsidMax + 1];
	copy (s, sizeof s, ssid, nSsid);
	if (strlen (s) != nSsid) return n;		// a NUL inside: a hidden network's padding
	for (unsigned k = 0; k < n; k++)
		if (strcmp (nets[k].ssid, s) == 0)
		{
			if (rssi > nets[k].rssi) { nets[k].rssi = rssi; nets[k].ch = ch; nets[k].secured = secured; }
			return n;
		}
	if (n == max) return n;
	strcpy (nets[n].ssid, s);
	nets[n].rssi = rssi;
	nets[n].ch = ch;
	nets[n].secured = secured;
	return n + 1;
}

void sort_nets (Net *nets, unsigned n)
{
	for (unsigned i = 1; i < n; i++)
		for (unsigned j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; j--)
		{
			Net t = nets[j];
			nets[j] = nets[j - 1];
			nets[j - 1] = t;
		}
}

unsigned json_escape (const char *p, char *out, unsigned size)
{
	static const char H[] = "0123456789abcdef";
	unsigned n = 0;
	if (size == 0) return 0;
	for (; *p; p++)
	{
		unsigned char c = (unsigned char) *p;
		char e[7];
		unsigned l;
		if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char) c; l = 2; }
		else if (c < 0x20) { e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = H[c >> 4]; e[5] = H[c & 15]; l = 6; }
		else { e[0] = (char) c; l = 1; }
		if (n + l + 1 > size) break;
		memcpy (out + n, e, l);
		n += l;
	}
	out[n] = 0;
	return n;
}


// ---- the card-root first-setup file
static void trim_span (const char *&p, unsigned &n)
{
	while (n && (*p == ' ' || *p == '\t' || *p == '\r')) { p++; n--; }
	while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) n--;
}

// 0 or the reason; ssid/psk/band filled. The text is the file's, so nothing of it is copied into a reason.
static const char *parse_import (const char *text, char *ssid, char *psk, bool *open, unsigned *band)
{
	ssid[0] = psk[0] = 0;
	*open = false;
	*band = 1;
	bool haveSsid = false, havePsk = false;
	if ((unsigned char) text[0] == 0xEF && (unsigned char) text[1] == 0xBB && (unsigned char) text[2] == 0xBF) text += 3;
	while (*text)
	{
		const char *e = strchr (text, '\n');
		const char *l = text;
		unsigned n = e ? (unsigned) (e - text) : (unsigned) strlen (text);
		text += n + (e ? 1 : 0);
		trim_span (l, n);
		if (n == 0 || l[0] == '#') continue;
		const char *eq = (const char *) memchr (l, '=', n);
		if (!eq) continue;
		const char *k = l, *v = eq + 1;
		unsigned nk = (unsigned) (eq - l), nv = n - nk - 1;
		trim_span (k, nk);
		trim_span (v, nv);
		if (nv >= 2 && (v[0] == '"' || v[0] == '\'') && v[nv - 1] == v[0]) { v++; nv -= 2; }
		char key[16];
		if (nk >= sizeof key) continue;
		for (unsigned i = 0; i < nk; i++) key[i] = lower (k[i]);
		key[nk] = 0;
		if (strcmp (key, "ssid") == 0)
		{
			if (nv > kSsidMax) return "the network name must be 1-32 bytes";
			copy (ssid, kSsidMax + 1, v, nv);
			haveSsid = true;
		}
		else if (strcmp (key, "psk") == 0 || strcmp (key, "password") == 0)
		{
			if (nv > 64) return "password must be 8-63 characters";
			copy (psk, 65, v, nv);
			havePsk = true;
		}
		else if (strcmp (key, "band") == 0)
		{
			char b[8];
			if (nv >= sizeof b) return "band must be 2g, 5g or any";
			for (unsigned i = 0; i < nv; i++) b[i] = lower (v[i]);
			b[nv] = 0;
			unsigned x = band_from_name (b, 99);
			if (x == 99) return "band must be 2g, 5g or any";
			*band = x;
		}
		else if (strcmp (key, "open") == 0)
			*open = nv == 1 && v[0] == '1' ? true : nv == 4 && ieq (v, "true", 4) ? true : nv == 3 && ieq (v, "yes", 3);
	}
	if (!haveSsid || ssid[0] == 0) return "ssid is missing";
	if (!*open && !havePsk) return "password is missing (or open=1 for an open network)";
	return 0;
}

static void scrub_remove (const Fs &fs, const char *path)
{
	if (fs.remove (path)) return;
	static const char Over[] = "# used - the password was removed\n";
	fs.write (path, Over, sizeof Over - 1);			// the contents gone even if the file stays
	if (fs.exists (kImportUsedPath)) fs.remove (kImportUsedPath);
	fs.rename (path, "/sidecar-wifi.used");
}

Import import_card (const Fs &fs, const char *confPath, const char *tryPath, KeyFn key, Conf *cur, char *why, unsigned nWhy)
{
	why[0] = 0;
	if (!fs.exists (kImportPath)) return kNoFile;
	char text[1024], ssid[kSsidMax + 1], psk[65];
	bool open;
	unsigned band;
	const char *r = "cannot read the file";
	if (fs.read (kImportPath, text, sizeof text))
	{
		r = parse_import (text, ssid, psk, &open, &band);
		if (!r)
		{
			Err e = check_join (ssid, (unsigned) strlen (ssid), open ? "" : psk, open);
			if (e == kOk && open && psk[0]) e = kOpenWithKey;
			r = e == kOk ? 0 : e == kSsid ? "the network name must be 1-32 bytes, no control characters"
				: e == kPsk ? "password must be 8-63 characters (printable ASCII) or 64 hex digits"
				: "an open network takes no password";
		}
	}
	if (!r)
	{
		Conf n = *cur;
		memset (n.ssid, 0, sizeof n.ssid);
		memcpy (n.ssid, ssid, strlen (ssid));
		n.open = open;
		memset (n.psk, 0, sizeof n.psk);
		if (!open)
		{
			if (is_hex64 (psk)) memcpy (n.psk, psk, 64);
			else key (psk, n.ssid, n.psk);		// the key, never the passphrase
		}
		n.on = true;
		n.band = band;
		if (!save (fs, confPath, &n, true)) r = "cannot write /.sidecar/wifi.txt";
		else
		{
			char t[128];
			int nt = format_trial (kTrying, 0, t, sizeof t);
			if (nt <= 0 || !fs.write (tryPath, t, (unsigned) nt)) r = "cannot write the trial marker";
			*cur = n;
		}
		memset (&n, 0, sizeof n);
	}
	memset (text, 0, sizeof text);
	memset (psk, 0, sizeof psk);
	scrub_remove (fs, kImportPath);
	if (r)
	{
		char m[160];
		int n = 0;
		const char *pre = "Sidecar could not use sidecar-wifi.txt: ";
		for (const char *p = pre; *p; p++) m[n++] = *p;
		for (const char *p = r; *p && n < 150; p++) m[n++] = *p;
		m[n++] = '\n';
		fs.write (kImportErrPath, m, (unsigned) n);
		copy (why, nWhy, r, (unsigned) strlen (r));
		return kRejected;
	}
	if (fs.exists (kImportErrPath)) fs.remove (kImportErrPath);
	return kImported;
}

}

