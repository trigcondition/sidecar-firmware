// core/tests/rui_png.h - a Rytm-screen frame as a PNG for the docs and reviews (sim/rui_render.py's colours, scale 4,
// the pixel grid), as test_samp_view.cpp / test_seq.cpp / test_pool_page.cpp write theirs: the host's render only
// (ui::preview / ui::preview_sent: the Rytm's text drawn in the host's font), no firmware data.
//   rui_png_dir("RUI_PAGES", "build/rui_pages")   the directory ($env or the default), made
//   rui_png(dir, "keys_map.png", fb)                one frame
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "ui/ui.h"

namespace rui_png_detail {
inline uint32_t crc32_of(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
inline std::vector<uint8_t> zstore(const std::vector<uint8_t>& raw) {   // a zlib stream of stored deflate blocks
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t at = 0; at < raw.size();) {
    size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8)), z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
  }
  for (uint8_t x : raw) a = (a + x) % 65521, b = (b + a) % 65521;
  uint32_t ad = b << 16 | a;
  for (int i = 3; i >= 0; --i) z.push_back((uint8_t)(ad >> (8 * i)));
  return z;
}
inline void put_be32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
inline void chunk(std::vector<uint8_t>& f, const char* t, const std::vector<uint8_t>& d) {
  put_be32(f, (uint32_t)d.size());
  std::vector<uint8_t> td(t, t + 4);
  td.insert(td.end(), d.begin(), d.end());
  f.insert(f.end(), td.begin(), td.end());
  put_be32(f, crc32_of(td.data(), td.size()));
}
}  // namespace rui_png_detail

inline std::string rui_png_dir(const char* env, const char* def) {
  const char* e = getenv(env);
  std::string d = e && *e ? e : def;
  mkdir("build", 0755);
  mkdir(d.c_str(), 0755);
  return d;
}

// fb: the Rytm's framebuffer layout (glass pixel x, y = byte (7 - y / 8) + 8 x, bit y % 8)
inline bool rui_png(const std::string& dir, const char* name, const uint8_t* fb) {
  using namespace rui_png_detail;
  const int s = 4, w = 128 * s, h = 64 * s;
  std::vector<uint8_t> raw;
  raw.reserve((size_t)h * (w * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      int X = x / s, Y = y / s;
      bool on = fb[(7 - Y / 8) + 8 * X] >> (Y % 8) & 1;
      int c[3] = {on ? 232 : 12, on ? 238 : 14, on ? 255 : 22};
      bool grid = x % s == s - 1 || y % s == s - 1;
      for (int k = 0; k < 3; ++k) raw.push_back((uint8_t)(grid ? c[k] * 8 / 10 : c[k]));
    }
  }
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, ih;
  put_be32(ih, w), put_be32(ih, h);
  ih.insert(ih.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ih);
  chunk(f, "IDAT", zstore(raw));
  chunk(f, "IEND", {});
  std::string path = dir + "/" + name;
  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) return false;
  fwrite(f.data(), 1, f.size(), fp);
  fclose(fp);
  printf("  %s\n", path.c_str());
  return true;
}
