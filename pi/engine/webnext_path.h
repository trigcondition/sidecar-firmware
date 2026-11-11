// pi/engine/webnext_path.h - the next web UI (web-next/, docs/web-ws.md "Serving the new app from the Pi") is served from
// the card, not built into the kernel: GET /next/<file> reads SD:/.rytm/www-next/<file>. This is the URL -> card path
// mapping (nothing outside that folder: no "..", no hidden names, a short safe alphabet) and the answer's headers.
// /fs cannot reach /.rytm (fsweb.h), so nothing can be put there over the network; the folder is written by copying
// web-next's build onto the card. Portable, header only (tests/test_ws.cpp).
#ifndef _webnext_path_h
#define _webnext_path_h

#include <stddef.h>
#include <string.h>

namespace webnext {

static const char kRoot[] = "SD:/.rytm/www-next";

// true and the card path into pOut (max bytes) for "/next", "/next/" (index.html) and "/next/<a/b.c>"; false for anything
// else or anything unsafe
inline bool path (const char *url, char *pOut, size_t max)
{
	if (strncmp (url, "/next", 5) != 0 || (url[5] != 0 && url[5] != '/')) return false;
	const char *rel = url[5] == 0 || url[6] == 0 ? "index.html" : url + 6;
	size_t n = strlen (rel);
	if (n == 0 || n > 120) return false;
	bool segStart = true;
	for (size_t i = 0; i < n; i++)
	{
		char c = rel[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/';
		if (!ok) return false;
		if (segStart && (c == '.' || c == '/')) return false;	// no "..", no hidden names, no "//", no leading "/"
		segStart = c == '/';
	}
	if (rel[n - 1] == '/') return false;
	if (strlen (kRoot) + 1 + n + 1 > max) return false;
	strcpy (pOut, kRoot);
	strcat (pOut, "/");
	strcat (pOut, rel);
	return true;
}

// the Content-Type for a file name
inline const char *type (const char *name)
{
	const char *e = strrchr (name, '.');
	if (e == 0) return "application/octet-stream";
	static const char *const k[][2] = {
		{".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
		{".woff2", "font/woff2"}, {".svg", "image/svg+xml"}, {".png", "image/png"}, {".txt", "text/plain; charset=utf-8"},
		{".json", "application/json"}, {".ico", "image/x-icon"},
	};
	for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (strcmp (e, k[i][0]) == 0) return k[i][1];
	return "application/octet-stream";
}

// The Content-Type value with the other headers riding in it (Circle's daemon writes only Content-Type; web.cpp's page
// does the same): gz the file is the .gz sibling; immutable below. Static strings (the daemon sends them after
// GetContent returns).
#define WEBNEXT_H(t) t "\r\nCache-Control: no-cache", t "\r\nCache-Control: public, max-age=31536000, immutable", \
	t "\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nCache-Control: no-cache", \
	t "\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nCache-Control: public, max-age=31536000, immutable"
inline const char *headers (const char *name, bool gz, bool imm)
{
	static const char *const k[][5] = {
		{".html", WEBNEXT_H ("text/html; charset=utf-8")}, {".js", WEBNEXT_H ("text/javascript; charset=utf-8")},
		{".css", WEBNEXT_H ("text/css; charset=utf-8")}, {".woff2", WEBNEXT_H ("font/woff2")},
		{".svg", WEBNEXT_H ("image/svg+xml")}, {".png", WEBNEXT_H ("image/png")},
		{".txt", WEBNEXT_H ("text/plain; charset=utf-8")}, {".json", WEBNEXT_H ("application/json")},
		{".ico", WEBNEXT_H ("image/x-icon")}, {"", WEBNEXT_H ("application/octet-stream")},
	};
	const size_t n = sizeof k / sizeof k[0];
	const char *e = strrchr (name, '.');
	size_t i = 0;
	if (e != 0) while (i < n - 1 && strcmp (e, k[i][0]) != 0) i++;
	else i = n - 1;
	return k[i][1 + (gz ? 2 : 0) + (imm ? 1 : 0)];
}
#undef WEBNEXT_H

// Vite's hashed assets (/next/assets/...) never change under one name: cached for good; everything else (index.html,
// the favicon, the licences) asked again each time, so a new build shows at once
inline bool immutable (const char *url) { return strncmp (url, "/next/assets/", 13) == 0; }

}	// namespace webnext

#endif
