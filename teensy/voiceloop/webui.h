// webui.h - the web console's machine-readable side (web/, docs/webui-protocol.md): user machines (the plugin
// store), the live memory map and the SD card's files, for a page on WebSerial. Included by voiceloop.ino (after the
// input buffer). Everything here is cold (FLASHMEM, strings PSTR): ITCM has ~2 KB of headroom.
//
// Replies are log lines that start with '@': '@' + one JSON object, or '@d ...' for a download's data. They pass
// through log_line, so the plain host log stays plain and a framed link ('L') carries them too (shorter data there).
//
// Console lines (':' + text, at most 63 characters):
//   mem                      the memory map: @{"t":"mem","k":<region>,"size":N,"used":N[,...]} a region, then
//                            @{"t":"mem_end"} (regions: itcm dtcm ocram flash store arena [parena: PSRAM code arena, when fitted] state psram sd)
//   plugins                  the installed packages: @{"t":"plugin",...} each, @{"t":"slot",...} U1..U5, then
//                            @{"t":"plugins",...} (built over loop passes, one store read a pass)
//   plugin=rescan            the store's generation moves on (the Rytm's lists rebuilt), MISSING slots look again
//   plugin=del:ID            one package out of the store (never the whole store; transport stopped)
//   plugin=set:U:ID          U (1..5) plays ID (its installed version) in the current project
//   plugin=clr:U             U empty (explicitly: no fingerprint or inheritance fills it)
//   plugin=put:U:ID:a.b.c[:NAME]  an imported slot list's entry (ID - : explicitly empty); installed or not
//   plugin=empty             "start this project empty": U1..U5 explicitly empty
//   df                       the card's size and free space (free: one FAT/bitmap scan, cached until a write)
// Binary frames 'Y' (not a development frame letter: the host log stays plain):
//   'Y' <n u16 LE> <payload n bytes> <crc32 u32 LE of the payload>     n <= kWebFrameMax
//   payload[0] = op, see web_frame below and docs/webui-protocol.md.
// Card writes, deletes, renames and store installs run only with the transport stopped (as STOP TO INSTALL); reads
// and listings run any time, rate-limited while it runs. One SD or store operation a loop pass, between requests.

#ifndef RYTM_WEBUI
#define RYTM_WEBUI 1
#endif
#if RYTM_WEBUI

// RAM: its buffers (~4.5 KB) are in DTCM (RAM1 has ~87 KB of locals; RAM2 only ~18 KB for malloc).
// (the linker symbols it reads are declared at the top of voiceloop.ino: outside the anonymous namespace)

constexpr int kWebChunk = 1024;                     // upload data per frame
constexpr int kWebFrameMax = 1 + 1 + 4 + kWebChunk;  // op, id, offset, data
constexpr int kWebDownPlain = 768;                  // download data per line: plain log (1024 base64 characters)
constexpr int kWebDownFramed = 168;                 // framed: the line fits an 'L' frame (255)
constexpr uint32_t kWebGapStoppedUs = 300;          // between two card operations, transport stopped
constexpr uint32_t kWebGapRunningUs = 20000;        // and while it runs (reads only): <= 50 a second
constexpr uint32_t kWebIdleMs = 30000;              // an open transfer with no frame this long is aborted

struct WebXfer {
  uint8_t kind;       // 0 none, 'f' a file upload, 'p' a plugin install, 'g' a download
  uint8_t id;
  uint32_t size, crc_want, crc, off, t_last, t0;
  char path[160], part[166];
};
WebXfer wx;
FsFile wx_file;
uint8_t wx_pend[kWebChunk];      // one upload chunk, written from loop() in idle time
int wx_pend_n = -1;
uint32_t wx_pend_off = 0;
uint32_t web_next_us = 0;
// A pending operation (one at a time): a listing (continued over passes), delete, mkdir, rename, df, a download
// chunk, the plugin list, the memory map.
struct WebOp {
  uint8_t op;          // 0 none, 'l' 'd' 'm' 'n' 'f' 'r' 'P' (plugins) 'M' (mem)
  uint8_t phase;
  uint16_t i;
  uint32_t off, n;
  char a[160], b[160];
};
WebOp wop;
FsFile wop_dir;
char web_line[1100];
bool web_df_ok = false;
uint64_t web_df_free = 0, web_df_total = 0;

// ---- output -------------------------------------------------------------------------------------------------------
int web_n = 0;
FLASHMEM void wj(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int room = (int)sizeof web_line - web_n;
  int n = vsniprintf(web_line + web_n, room, fmt, ap);
  va_end(ap);
  web_n += n < 0 ? 0 : n >= room ? room - 1 : n;
}
// A byte string as a JSON string: printable ASCII as is, '"' '\\' and every other byte as \u00XX (the page maps
// the code units back to bytes and decodes UTF-8: the log parser drops bytes >= 0x7f).
FLASHMEM void wj_str(const char* s, int max = 120) {   // (120: the worst case, all escaped, fits web_line)
  wj(PSTR("\""));
  for (int i = 0; s[i] && i < max; ++i) {
    uint8_t c = (uint8_t)s[i];
    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') wj(PSTR("%c"), c);
    else wj(PSTR("\\u%04x"), c);
  }
  wj(PSTR("\""));
}
// A framed link's 'L' carries at most 255 bytes: a longer reply is cut there (the page drops it as unparsable)
// rather than breaking the framing. Only after a development tool switched the host build to framed.
FLASHMEM void web_out() { log_line(web_line, log_framed && web_n > 255 ? 255 : web_n); }
FLASHMEM void wj_begin(const char* type) {
  web_n = 0;
  wj(PSTR("@{\"t\":\"%s\""), type);
}
FLASHMEM void wj_end() {
  wj(PSTR("}"));
  web_out();
}
FLASHMEM void web_reply(const char* type, uint8_t id, const char* st, const char* why) {
  wj_begin(type);
  wj(PSTR(",\"x\":%d,\"st\":\"%s\""), id, st);
  if (why) wj(PSTR(",\"why\":")), wj_str(why);
  wj_end();
}

FLASHMEM uint32_t web_crc(uint32_t c, const uint8_t* p, uint32_t n) {   // CRC-32 (IEEE, as rmp_crc32_update)
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return c;
}

FLASHMEM bool web_running() {
#if RYTM_PLUGINS
  return plug_transport_running();
#else
  return false;
#endif
}
// A path from the page: absolute, no "..", no control bytes, fits.
FLASHMEM bool web_path(const uint8_t* p, int n, char* out, int max) {
  if (n < 1 || n >= max || p[0] != '/') return false;
  for (int i = 0; i < n; ++i)
    if (p[i] < 0x20 || p[i] == '\\') return false;
  for (int i = 0; i + 2 < n + 1; ++i)   // no ".." path element (a loop: libc's strstr would pull memchr into ITCM)
    if (p[i] == '/' && i + 2 < n && p[i + 1] == '.' && p[i + 2] == '.' && (i + 3 == n || p[i + 3] == '/')) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// ---- the memory map -------------------------------------------------------------------------------------------------
FLASHMEM void web_mem_region(const char* k, uint32_t size, uint32_t used) {
  wj_begin(PSTR("mem"));
  wj(PSTR(",\"k\":\"%s\",\"size\":%lu,\"used\":%lu"), k, (unsigned long)size, (unsigned long)used);
}
FLASHMEM void web_mem() {
  const uint32_t itcm = (uint32_t)(uintptr_t)&_itcm_block_count * 32768u;
  web_mem_region(PSTR("itcm"), itcm, (uint32_t)&_etext - (uint32_t)&_stext);
  wj(PSTR(",\"blocks\":%lu}"), (unsigned long)(uintptr_t)&_itcm_block_count);
  web_out();
  uint32_t sp;
  asm volatile("mov %0, sp" : "=r"(sp));
  web_mem_region(PSTR("dtcm"), 524288u - itcm, (uint32_t)&_ebss - (uint32_t)&_sdata);
  wj(PSTR(",\"stack_free\":%lu,\"stack_size\":%lu}"), (unsigned long)(sp - (uint32_t)&_ebss),
     (unsigned long)((uint32_t)&_estack - (uint32_t)&_ebss));
  web_out();
  web_mem_region(PSTR("ocram"), 524288u, (uint32_t)&_heap_start - 0x20200000u + (uint32_t)(__brkval - (char*)&_heap_start));
  wj(PSTR(",\"static\":%lu,\"heap\":%lu}"), (unsigned long)((uint32_t)&_heap_start - 0x20200000u),
     (unsigned long)(__brkval - (char*)&_heap_start));
  web_out();
  web_mem_region(PSTR("flash"), 7936u * 1024u, (uint32_t)&_flashimagelen);
  wj_end();
#if RYTM_PLUGINS
  uint32_t tot = 0, used = 0;
  bool st = rmp_store_space(&tot, &used);   // walks the LittleFS (a few ms): from loop() in idle time
  web_mem_region(PSTR("store"), st ? tot : 0, st ? used : 0);
  wj(PSTR(",\"mounted\":%s,\"pkg_max\":%lu}"), st ? PSTR("true") : PSTR("false"), (unsigned long)RMP_STORE_MAX_PKG);
  web_out();
  web_mem_region(PSTR("arena"), RMP_ARENA_BYTES, rmp_arena_used());
  wj_end();
  if (rmp_arena_psram_size()) {   // the second code arena (PSRAM fitted only)
    web_mem_region(PSTR("parena"), rmp_arena_psram_size(), rmp_arena_psram_used());
    wj_end();
  }
  web_mem_region(PSTR("state"), rmp_state_capacity() > sizeof shared[0] ? rmp_state_capacity() : sizeof shared[0],
                 0);   // (rmp_state_used_*: in ITCM, not linked for this; the packages' "state" says each one's need)
  wj(PSTR(",\"voice\":%lu,\"psram\":%lu}"), (unsigned long)sizeof shared[0], (unsigned long)rmp_state_capacity());
  web_out();
#endif
  web_mem_region(PSTR("psram"), (uint32_t)external_psram_size << 20, 0);
  wj_end();
  if (card_ok) {
    web_df_total = (uint64_t)cards.sd().clusterCount() * cards.sd().bytesPerCluster();
    wj_begin(PSTR("mem"));
    wj(PSTR(",\"k\":\"sd\",\"size\":%lu,\"size_kb\":%lu"), (unsigned long)(web_df_total > 0xFFFFFFFFull ? 0xFFFFFFFFu : web_df_total),
       (unsigned long)(web_df_total >> 10));
    if (web_df_ok) wj(PSTR(",\"free_kb\":%lu,\"used_kb\":%lu"), (unsigned long)(web_df_free >> 10),
                      (unsigned long)((web_df_total - web_df_free) >> 10));
    wj(PSTR(",\"type\":\"%s\"}"), cards.sd().type_name());
    web_out();
  }
  wj_begin(PSTR("mem_end"));
  wj_end();
}

// ---- the card -------------------------------------------------------------------------------------------------------
FLASHMEM void web_df() {
  if (!card_ok) { web_reply(PSTR("df"), 0, PSTR("err"), PSTR("no card")); return; }
  uint32_t t0 = millis();
#if RYTM_PLUGINS
  rmp_guard_feed();
#endif
  int32_t fc = cards.sd().freeClusterCount();   // one scan of the FAT / the exFAT bitmap
  web_df_total = (uint64_t)cards.sd().clusterCount() * cards.sd().bytesPerCluster();
  web_df_ok = fc >= 0;
  web_df_free = web_df_ok ? (uint64_t)fc * cards.sd().bytesPerCluster() : 0;
  wj_begin(PSTR("df"));
  wj(PSTR(",\"st\":\"ok\",\"total_kb\":%lu,\"free_kb\":%lu,\"type\":\"%s\",\"ms\":%lu"), (unsigned long)(web_df_total >> 10),
     (unsigned long)(web_df_free >> 10), cards.sd().type_name(), (unsigned long)(millis() - t0));
  wj_end();
}
FLASHMEM void web_df_spent(uint64_t bytes) {   // a write took room: the cached free space follows
  if (web_df_ok) web_df_free = web_df_free > bytes ? web_df_free - bytes : 0;
}

// One listing step: open, then up to 16 entries a pass (at most 4000 an listing).
FLASHMEM bool web_list_step() {
  if (wop.phase == 0) {
    if (!card_ok || !wop_dir.open(wop.a, O_RDONLY) || !wop_dir.isDir()) {
      wop_dir.close();
      wj_begin(PSTR("ls_end"));
      wj(PSTR(",\"p\":")), wj_str(wop.a), wj(PSTR(",\"st\":\"err\",\"why\":\"%s\""), card_ok ? PSTR("not a folder") : PSTR("no card"));
      wj_end();
      return false;
    }
    wop.phase = 1;
    wop.i = 0;
    return true;
  }
  FsFile e;
  for (int k = 0; k < 16; ++k) {
    if (wop.i >= 4000 || !e.openNext(&wop_dir, O_RDONLY)) {
      wop_dir.close();
      wj_begin(PSTR("ls_end"));
      wj(PSTR(",\"p\":")), wj_str(wop.a), wj(PSTR(",\"st\":\"ok\",\"n\":%u,\"more\":%s"), wop.i, wop.i >= 4000 ? PSTR("true") : PSTR("false"));
      wj_end();
      return false;
    }
    char nm[256];
    e.getName(nm, sizeof nm);
    bool dir = e.isDir();
    uint64_t sz = dir ? 0 : e.fileSize();
    uint16_t d = 0, t = 0;
    e.getModifyDateTime(&d, &t);
    e.close();
    wj_begin(PSTR("ls"));
    wj(PSTR(",\"n\":")), wj_str(nm);
    wj(PSTR(",\"s\":%lu,\"d\":%d,\"m\":\"%04d-%02d-%02d %02d:%02d\"}"), (unsigned long)(sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : sz), dir ? 1 : 0,
       1980 + (d >> 9), (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63);
    web_out();
    ++wop.i;
  }
  return true;
}
FLASHMEM void web_fs_done(char op, bool ok, const char* why) {
  wj_begin(PSTR("fs"));
  wj(PSTR(",\"op\":\"%c\",\"st\":\"%s\",\"p\":"), op, ok ? PSTR("ok") : PSTR("err"));
  wj_str(wop.a);
  if (why) wj(PSTR(",\"why\":")), wj_str(why);
  wj_end();
}
// A path the engine depends on: the card index and the slot maps, the plugin folders.
FLASHMEM bool web_protected(const char* p) {
  return !strcmp(p, PSTR("/")) || !strcmp(p, PSTR("/.rytm")) || !strncmp(p, PSTR("/.rytm/"), 7) || !strcmp(p, PSTR("/Samples"));
}
FLASHMEM void web_fs_op() {
  char op = (char)wop.op;
  if (!card_ok) { web_fs_done(op, false, PSTR("no card")); return; }
  if (web_running()) { web_fs_done(op, false, PSTR("STOP TO WRITE: the transport runs")); return; }
  bool ok = false;
  const char* why = nullptr;
  if (op == 'd') {
    if (web_protected(wop.a)) why = PSTR("protected (the engine's own files)");
    else {
      FsFile f;
      bool dir = f.open(wop.a, O_RDONLY) && f.isDir();
      f.close();
      ok = dir ? cards.sd().rmdir(wop.a) : cards.sd().remove(wop.a);
      if (!ok) why = dir ? PSTR("not empty, or cannot remove") : PSTR("cannot remove");
    }
  } else if (op == 'm') {
    ok = cards.sd().mkdir(wop.a, true);
    if (!ok) why = PSTR("cannot make the folder (it exists?)");
  } else if (op == 'n') {
    if (web_protected(wop.a) || web_protected(wop.b)) why = PSTR("protected (the engine's own files)");
    else if (cards.sd().exists(wop.b)) why = PSTR("the new name exists");
    else ok = cards.sd().rename(wop.a, wop.b);
    if (!ok && !why) why = PSTR("cannot rename");
  }
  web_fs_done(op, ok, why);
}

// ---- transfers ------------------------------------------------------------------------------------------------------
FLASHMEM void web_xfer_close(bool remove_part) {
  if (wx.kind == 'f' || wx.kind == 'g') wx_file.close();
  if (wx.kind == 'f' && remove_part && card_ok) cards.sd().remove(wx.part);
#if RYTM_PLUGINS
  if (wx.kind == 'p' && plug_installing) plug_install_abort("web console abort");
#endif
  wx.kind = 0;
  wx_pend_n = -1;
}
FLASHMEM void web_up_err(const char* why) {
  uint8_t id = wx.id;
  web_xfer_close(true);
  web_reply(PSTR("up"), id, PSTR("err"), why);
}
FLASHMEM uint32_t web_kbs(uint32_t bytes, uint32_t ms) { return ms ? bytes / ms * 1000u / 1024u + (bytes % ms) * 1000u / ms / 1024u : 0; }

#if RYTM_PLUGINS
// A package's fit, from its header and the head of its image (the plugin table, entry first) - the same numbers
// the loader checks: the image and bss in the code arena, the per-voice state in the voice's memory or PSRAM.
FLASHMEM const char* web_pkg_fit(const rmp_info& inf, uint32_t state, uint32_t api) {
  if (api > RMP_API) return PSTR("built for a newer plugin API");
  const uint32_t arena = rmp_arena_psram_size() > RMP_ARENA_BYTES ? rmp_arena_psram_size() : RMP_ARENA_BYTES;
  if (inf.image_size + inf.bss_size > arena) return PSTR("code + data over the code arena");
  uint32_t lim = rmp_state_capacity() > sizeof shared[0] ? rmp_state_capacity() : sizeof shared[0];
  if (state > lim) return PSTR("per-voice state over the voice memory (no PSRAM)");
  return nullptr;
}
#endif

// The chunk waiting in wx_pend: written (card or store), acknowledged.
FLASHMEM void web_write_pending() {
  uint32_t off = wx_pend_off;
  int n = wx_pend_n;
  wx_pend_n = -1;
  if (wx.kind == 'f') {
    if ((int)wx_file.write(wx_pend, n) != n) { web_up_err(PSTR("card write failed (full?)")); return; }
    web_df_spent((uint64_t)n);
  }
#if RYTM_PLUGINS
  else if (wx.kind == 'p') {
    if (off == 0) {   // the header and the plugin table: refused here when it will not fit
      rmp_info inf;
      if (n < (int)RMP_HEADER_BYTES + 16 || rmp_verify_header(wx_pend, wx.size, &inf) != RMP_OK) { web_up_err(PSTR("not a valid .rmp header")); return; }
      uint32_t eo = RMP_HEADER_BYTES + inf.entry_offset;
      uint32_t st = 0, api = 0, magic = 0;
      if (eo + 12 <= (uint32_t)n) { memcpy(&magic, wx_pend + eo, 4); memcpy(&api, wx_pend + eo + 4, 4); memcpy(&st, wx_pend + eo + 8, 4); }
      if (magic && magic != RMP_PLUGIN_MAGIC) { web_up_err(PSTR("no plugin table at the entry")); return; }
      const char* why = web_pkg_fit(inf, st, api);
      if (why) { web_up_err(why); return; }
    }
    if (off == 0) memcpy(wx.part, wx_pend, RMP_HEADER_BYTES);   // the header: the commit checks the store holds it
    plug_install_write(wx_pend, n);
    if (!plug_installing) { web_up_err(PSTR("store write failed")); return; }
  }
#endif
  wx.crc = web_crc(wx.crc, wx_pend, n);
  wx.off = off + n;
  wj_begin(PSTR("up"));
  wj(PSTR(",\"x\":%d,\"st\":\"ack\",\"off\":%lu}"), wx.id, (unsigned long)wx.off);
  web_out();
}
FLASHMEM void web_commit() {
  uint8_t id = wx.id;
  if (wx.off != wx.size) { web_up_err(PSTR("commit before the end")); return; }
  if (~wx.crc != wx.crc_want) { web_up_err(PSTR("CRC mismatch: the file differs, nothing kept")); return; }
  uint32_t ms = millis() - wx.t0;
  if (wx.kind == 'f') {
    bool ok = wx_file.sync();
    wx_file.close();
    if (ok && cards.sd().exists(wx.path) && !cards.sd().remove(wx.path)) ok = false;
    if (ok) ok = cards.sd().rename(wx.part, wx.path);
    if (!ok) { cards.sd().remove(wx.part); wx.kind = 0; web_reply(PSTR("up"), id, PSTR("err"), PSTR("cannot replace the file")); return; }
  }
#if RYTM_PLUGINS
  else if (wx.kind == 'p') {
    plug_install_commit();   // logs "plugins: installed <id>" or the failure; the slots naming it load again
    if (plug_installing) plug_install_abort("commit");
    char pid[33] = {0};
    memcpy(pid, wx.part + 8, 32);   // the header's id; installed = the store's header is this one (its CRC)
    uint8_t h[RMP_HEADER_BYTES];
    uint32_t n = 0;
    bool ok = rmp_store_size(pid, &n) && n == wx.size && rmp_store_read_at(pid, 0, h, sizeof h) && !memcmp(h, wx.part, RMP_HEADER_BYTES);
    rmp_store_release();
    if (!ok) { wx.kind = 0; web_reply(PSTR("up"), id, PSTR("err"), PSTR("the store did not take it (see the log)")); return; }
  }
#endif
  const bool plugin = wx.kind == 'p';
  wx.kind = 0;
  wj_begin(PSTR("up"));
  wj(PSTR(",\"x\":%d,\"st\":\"done\",\"size\":%lu,\"ms\":%lu,\"kbs\":%lu"), id, (unsigned long)wx.size, (unsigned long)ms,
     (unsigned long)web_kbs(wx.size, ms));
  if (plugin) wj(PSTR(",\"plugin\":true"));
  wj_end();
}
// Open an upload: 'f' a card file (<path>.part, renamed when complete) or 'p' a package into the store.
FLASHMEM void web_open_up(uint8_t kind, uint8_t id, uint32_t size, uint32_t crc, const uint8_t* p, int pn) {
  if (wx.kind && wx.kind == kind && wx.id == id && wx.size == size && wx.crc_want == crc) {   // the same one again: resume
    wx.t_last = millis();
    wj_begin(PSTR("up"));
    wj(PSTR(",\"x\":%d,\"st\":\"open\",\"off\":%lu,\"max\":%d,\"resume\":true}"), id, (unsigned long)wx.off, kWebChunk);
    web_out();
    return;
  }
  if (wx.kind) web_xfer_close(true);   // another transfer replaces an old one
  wx.id = id;
  if (web_running()) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("STOP TO WRITE: the transport runs")); return; }
  if (kind == 'f') {
    if (!card_ok) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("no card")); return; }
    if (!web_path(p, pn, wx.path, sizeof wx.path)) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("bad path")); return; }
    if (web_protected(wx.path) || !strncmp(wx.path, PSTR("/.rytm/"), 7)) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("protected (the engine's own files)")); return; }
    if (web_df_ok && size + 65536u > web_df_free) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("not enough room on the card")); return; }
    sniprintf(wx.part, sizeof wx.part, PSTR("%s.part"), wx.path);
    if (!wx_file.open(wx.part, O_WRONLY | O_CREAT | O_TRUNC)) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("cannot create the file (folder missing?)")); return; }
    if (size) wx_file.preAllocate(size);   // contiguous where the card allows (faster, and the sample reader likes it)
  }
#if RYTM_PLUGINS
  else if (kind == 'p') {
    if (size > RMP_STORE_MAX_PKG) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("package over 64 KB (the store's limit)")); return; }
    if (plug_ci.on || plug_installing) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("an install runs")); return; }
    if (plug_info_on()) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("busy (the info report runs)")); return; }
    uint32_t tot = 0, used = 0;
    if (!rmp_store_space(&tot, &used)) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("the plugin store is not mounted")); return; }
    if (size + 65536u * 2u > tot - used) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("not enough room in the plugin store")); return; }
    plug_install_begin(size);
    if (!plug_installing) { web_reply(PSTR("up"), id, PSTR("err"), PSTR("the store refused the install")); return; }
  }
#endif
  else { web_reply(PSTR("up"), id, PSTR("err"), PSTR("unknown kind")); return; }
  wx.kind = kind;
  wx.size = size;
  wx.crc_want = crc;
  wx.crc = 0xFFFFFFFFu;
  wx.off = 0;
  wx.t0 = wx.t_last = millis();
  wj_begin(PSTR("up"));
  wj(PSTR(",\"x\":%d,\"st\":\"open\",\"off\":0,\"max\":%d}"), id, kWebChunk);
  web_out();
}
FLASHMEM void web_open_down(uint8_t id, const uint8_t* p, int pn) {
  if (wx.kind) web_xfer_close(true);
  wx.id = id;
  if (!card_ok) { web_reply(PSTR("dn"), id, PSTR("err"), PSTR("no card")); return; }
  if (!web_path(p, pn, wx.path, sizeof wx.path) || !wx_file.open(wx.path, O_RDONLY) || wx_file.isDir()) {
    wx_file.close();
    web_reply(PSTR("dn"), id, PSTR("err"), PSTR("cannot open"));
    return;
  }
  wx.kind = 'g';
  wx.size = (uint32_t)wx_file.fileSize();
  wx.off = 0;
  wx.t0 = wx.t_last = millis();
  wj_begin(PSTR("dn"));
  wj(PSTR(",\"x\":%d,\"st\":\"open\",\"size\":%lu,\"max\":%d}"), id, (unsigned long)wx.size, log_framed ? kWebDownFramed : kWebDownPlain);
  web_out();
}
// A download request: wop.n bytes from wop.off, one line a pass, each "@d <id> <off> <crc32 hex of its bytes> <base64>";
// true while more of the request is left. An empty line (no data) marks the end of the file.
FLASHMEM bool web_read_chunk() {
  static const char b64[] PROGMEM = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t off = wop.off, n = wop.n;
  const uint32_t lim = log_framed ? kWebDownFramed : kWebDownPlain;
  if (wx.kind != 'g') { web_reply(PSTR("dn"), wx.id, PSTR("err"), PSTR("not open")); return false; }
  if (n > lim) n = lim;
  if (off >= wx.size) n = 0;
  else if (n > wx.size - off) n = wx.size - off;
  uint8_t d[kWebDownPlain];
  if (n && (!wx_file.seekSet(off) || wx_file.read(d, n) != (int)n)) { web_reply(PSTR("dn"), wx.id, PSTR("err"), PSTR("card read failed")); return false; }
  wx.t_last = millis();
  web_n = 0;
  wj(PSTR("@d %d %lu %08lx "), wx.id, (unsigned long)off, (unsigned long)~web_crc(0xFFFFFFFFu, d, n));
  for (uint32_t i = 0; i < n; i += 3) {
    uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    char q[4] = {b64[v >> 18 & 63], b64[v >> 12 & 63], i + 1 < n ? b64[v >> 6 & 63] : '=', i + 2 < n ? b64[v & 63] : '='};
    memcpy(web_line + web_n, q, 4);
    web_n += 4;
  }
  web_out();
  wop.off = off + n;
  wop.n = wop.n > n ? wop.n - n : 0;
  return n && wop.n;
}

// ---- user machines -------------------------------------------------------------------------------------------------
#if RYTM_PLUGINS
struct WebPkg { char id[32]; uint32_t size; };
WebPkg web_pk[32];   // (the Rytm's slot menu lists 32 too; "more" counts the rest)
int web_pk_n = 0, web_pk_over = 0;
FLASHMEM void web_pk_add(const char* id, uint32_t size, void*) {
  if (web_pk_n >= 32) { ++web_pk_over; return; }
  memcpy(web_pk[web_pk_n].id, id, 32);
  web_pk[web_pk_n++].size = size;
}
FLASHMEM void web_ver(uint32_t v) { wj(PSTR("\"%d.%d.%d\""), (int)(v >> 16), (int)((v >> 8) & 255), (int)(v & 255)); }
// One package a pass: its header, the plugin table at the entry and the manifest's name (three store reads).
FLASHMEM void web_pkg_line(const WebPkg& k) {
  uint8_t h[RMP_HEADER_BYTES];
  rmp_info inf;
  bool ok = rmp_store_read_at(k.id, 0, h, sizeof h) && rmp_verify_header(h, k.size, &inf) == RMP_OK;
  uint32_t tab[4] = {0, 0, 0, 0};
  char name[11] = {0};
  if (ok) ok = rmp_store_read_at(k.id, RMP_HEADER_BYTES + inf.entry_offset, (uint8_t*)tab, sizeof tab);
  if (ok && inf.manifest_size >= 10)
    rmp_store_read_at(k.id, RMP_HEADER_BYTES + inf.image_size + 8u * inf.reloc_count, (uint8_t*)name, 10);
  wj_begin(PSTR("plugin"));
  wj(PSTR(",\"id\":")), wj_str(k.id, 32);
  wj(PSTR(",\"size\":%lu"), (unsigned long)k.size);
  if (!ok) {
    wj(PSTR(",\"fit\":false,\"status\":\"bad header\",\"why\":\"the package does not verify\"}"));
    web_out();
    return;
  }
  uint16_t fapi;
  memcpy(&fapi, h + 6, 2);
  wj(PSTR(",\"ver\":")), web_ver(inf.version);
  wj(PSTR(",\"name\":")), wj_str(name, 10);
  wj(PSTR(",\"api\":%lu,\"image\":%lu,\"bss\":%lu,\"code\":%lu,\"state\":%lu,\"cpu\":%lu,\"target\":\"arm32\""), (unsigned long)tab[1],
     (unsigned long)inf.image_size, (unsigned long)inf.bss_size, (unsigned long)(inf.image_size + inf.bss_size), (unsigned long)tab[2],
     (unsigned long)tab[3]);
  wj(PSTR(",\"slots\":["));
  bool first = true;
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (!strncmp(plug_map.slot[s].id, k.id, 32)) { wj(first ? PSTR("%d") : PSTR(",%d"), s + 1); first = false; }
  const char* why = tab[0] != RMP_PLUGIN_MAGIC ? PSTR("no plugin table at the entry") : web_pkg_fit(inf, tab[2], tab[1]);
  wj(PSTR("],\"fit\":%s,\"status\":\"%s\""), why ? PSTR("false") : PSTR("true"), why ? PSTR("no fit") : PSTR("ok"));
  if (why) wj(PSTR(",\"why\":")), wj_str(why);
  wj_end();
}
FLASHMEM bool web_plugins_step() {
  if (wop.phase == 0) {
    web_pk_n = web_pk_over = 0;
    rmp_store_list(web_pk_add, nullptr);
    wop.phase = 1;
    wop.i = 0;
    return true;
  }
  if (wop.i < web_pk_n) {
    web_pkg_line(web_pk[wop.i++]);
    return true;
  }
  rmp_store_release();
  for (int s = 0; s < RMP_SLOTS; ++s) {
    const rmp_slot_entry& e = plug_map.slot[s];
    wj_begin(PSTR("slot"));
    wj(PSTR(",\"slot\":%d,\"id\":"), s + 1), wj_str(e.id, 32);
    wj(PSTR(",\"ver\":")), web_ver(e.version);
    wj(PSTR(",\"state\":\"%s\",\"name\":"), plug_state_name(plug[s].state));
    wj_str(plug_desc_ok[s] ? plug_desc[s].name : e.id, 10);
    const rmp_slot_note& n = plug_notes.slot[s];   // where the assignment came from, what the sounds want (rmp_resolve.h)
    wj(PSTR(",\"src\":\"%s\",\"updated\":%s,\"collision\":%s,\"unknown\":%s,\"mixed\":%s,\"want_fp\":%u,\"want_n\":%u,\"want_id\":"),
       rmp_src_name(n.src), n.flags & RMP_RF_UPDATED ? PSTR("true") : PSTR("false"), n.flags & RMP_RF_COLLISION ? PSTR("true") : PSTR("false"),
       n.flags & RMP_RF_UNKNOWN ? PSTR("true") : PSTR("false"), n.flags & RMP_RF_MIXED ? PSTR("true") : PSTR("false"),
       (unsigned)n.fp, (unsigned)n.nfp);
    wj_str(n.id, 32);
    wj(PSTR(",\"want_ver\":")), web_ver(n.id[0] ? n.version : 0);
    wj(PSTR(",\"want_name\":")), wj_str(n.name, 10);
    wj(PSTR(",\"fp\":%u"), plug[s].state == RMP_SLOT_READY ? (unsigned)plug_fp[s] : 0u);
    if (plug[s].state == RMP_SLOT_READY || plug[s].state == RMP_SLOT_MUTED)   // the image's arena: "internal" or "psram"
      wj(PSTR(",\"place\":\"%s\""), rmp_place_name(plug_place[s]));
    char why[64];
    plug_why_text(s, why, sizeof why);   // why it is not READY, in words ("no room: needs 23288 B, 584 B free")
    if (why[0]) wj(PSTR(",\"why\":")), wj_str(why);
    wj_end();
  }
  uint32_t tot = 0, used = 0;
  bool st = rmp_store_space(&tot, &used);
  wj_begin(PSTR("plugins"));
  wj(PSTR(",\"n\":%d,\"more\":%d,\"prints\":\"%s\",\"fresh\":%s,\"project\":"), web_pk_n, web_pk_over,
     plug_prints_have ? PSTR("rytm") : plug_prints_rytm ? PSTR("waiting") : PSTR("trigs"), plug_fresh ? PSTR("true") : PSTR("false"));
  wj_str(prj().key);
  if (rmp_arena_psram_size())
    wj(PSTR(",\"parena\":[%lu,%lu]"), (unsigned long)rmp_arena_psram_size(), (unsigned long)rmp_arena_psram_used());
  wj(PSTR(",\"arena\":[%lu,%lu],\"state_limit\":%lu,\"store\":[%lu,%lu],\"api\":%d,\"safe_mode\":%s,\"transport\":%s}"),
     (unsigned long)RMP_ARENA_BYTES, (unsigned long)rmp_arena_used(),
     (unsigned long)(rmp_state_capacity() > sizeof shared[0] ? rmp_state_capacity() : sizeof shared[0]), (unsigned long)(st ? tot : 0),
     (unsigned long)(st ? tot - used : 0), (int)RMP_API, plug_off ? PSTR("true") : PSTR("false"), web_running() ? PSTR("true") : PSTR("false"));
  web_out();
  return false;
}
// ':plugin=...'
FLASHMEM void web_plugin_cmd(const char* a) {
  if (!strcmp(a, PSTR("rescan"))) {
    rmp_meta_store(&plug_meta);
    plug_inst_phase = 0;
    for (int s = 0; s < RMP_SLOTS; ++s)
      if (plug[s].state == RMP_SLOT_MISSING) plug_card_todo |= 1u << s;
    wj_begin(PSTR("pl")), wj(PSTR(",\"op\":\"rescan\",\"st\":\"ok\"")), wj_end();
    return;
  }
  if (!strncmp(a, PSTR("del:"), 4)) {
    const char* id = a + 4;
    const char* why = nullptr;
    if (web_running()) why = PSTR("STOP TO INSTALL: the transport runs");
    else if (plug_ci.on || plug_installing || wx.kind == 'p') why = PSTR("an install runs");
    else if (plug_info_on()) why = PSTR("busy (the info report runs)");
    for (int s = 0; !why && s < RMP_SLOTS; ++s)
      if (plug[s].state == RMP_SLOT_LOADING || plug_act_slot == s || (plug_job_on && plug_job_slot == s)) why = PSTR("a slot is loading");
    if (!why) {
      plug_remove((const uint8_t*)id, (int)strlen(id));   // logs; the slots naming it become MISSING
      uint32_t n;
      why = rmp_store_size(id, &n) ? PSTR("cannot remove") : nullptr;
      rmp_store_release();
    }
    wj_begin(PSTR("pl")), wj(PSTR(",\"op\":\"del\",\"id\":")), wj_str(id, 32), wj(PSTR(",\"st\":\"%s\""), why ? PSTR("err") : PSTR("ok"));
    if (why) wj(PSTR(",\"why\":")), wj_str(why);
    wj_end();
    return;
  }
  if ((!strncmp(a, PSTR("set:"), 4) || !strncmp(a, PSTR("clr:"), 4)) && a[4] >= '1' && a[4] <= '5') {
    int u = a[4] - '0';
    const char* id = a[1] == 'e' && a[5] == ':' ? a + 6 : "";
    uint8_t ver[3] = {0, 0, 0};
    const char* why = nullptr;
    if (*id) {   // its installed version
      uint8_t h[RMP_HEADER_BYTES];
      rmp_info inf;
      uint32_t n = 0;
      if (rmp_store_size(id, &n) && rmp_store_read_at(id, 0, h, sizeof h) && rmp_verify_header(h, n, &inf) == RMP_OK) {
        ver[0] = (uint8_t)(inf.version >> 16), ver[1] = (uint8_t)(inf.version >> 8), ver[2] = (uint8_t)inf.version;
      } else {
        why = PSTR("not installed");
      }
      rmp_store_release();
    }
    if (!why && !plug_assign(u, (const uint8_t*)id, (int)strlen(id), ver)) why = PSTR("refused (see the log)");
    wj_begin(PSTR("pl")), wj(PSTR(",\"op\":\"set\",\"slot\":%d,\"id\":"), u), wj_str(id, 32), wj(PSTR(",\"st\":\"%s\""), why ? PSTR("err") : PSTR("ok"));
    if (why) wj(PSTR(",\"why\":")), wj_str(why);
    wj_end();
    return;
  }
  if (!strcmp(a, PSTR("empty"))) {   // "start this project empty"
    bool ok = plug_start_empty();
    wj_begin(PSTR("pl")), wj(PSTR(",\"op\":\"empty\",\"st\":\"%s\""), ok ? PSTR("ok") : PSTR("err")), wj_end();
    return;
  }
  if (!strncmp(a, PSTR("put:"), 4) && a[4] >= '1' && a[4] <= '5' && a[5] == ':') {   // an imported entry: put:U:ID|-:a.b.c[:NAME]
    int u = a[4] - '0';
    char id[32] = "", name[11] = "";
    const char* p = a + 6;
    const char* q = strchr(p, ':');
    unsigned part[3] = {0, 0, 0};
    int k = 0;
    bool ok = q && q > p && q - p < 32;
    if (ok) memcpy(id, p, (size_t)(q - p)), id[q - p] = 0;
    int nd = 0;   // digits in the current part: 1..3
    for (p = ok ? q + 1 : ""; ok && *p && *p != ':'; ++p) {
      if (*p == '.') { if (!nd || ++k > 2) ok = false; nd = 0; continue; }
      if (*p < '0' || *p > '9' || ++nd > 3) ok = false;
      else part[k] = part[k] * 10 + (unsigned)(*p - '0');
    }
    ok = ok && k == 2 && nd && part[0] < 256 && part[1] < 256 && part[2] < 256;
    if (ok && *p == ':') strncpy(name, p + 1, 10);
    if (ok && !strcmp(id, PSTR("-"))) id[0] = 0;
    ok = ok && plug_import(u, id, part[0] << 16 | part[1] << 8 | part[2], name);
    wj_begin(PSTR("pl")), wj(PSTR(",\"op\":\"put\",\"slot\":%d,\"id\":"), u), wj_str(id, 32);
    wj(PSTR(",\"st\":\"%s\""), ok ? PSTR("ok") : PSTR("err"));
    if (ok && id[0]) {   // installed? (the import keeps it either way: MISSING by name until it is)
      uint32_t n;
      bool have = rmp_store_size(id, &n);
      rmp_store_release();
      wj(PSTR(",\"installed\":%s"), have ? PSTR("true") : PSTR("false"));
    }
    wj_end();
    return;
  }
  wj_begin(PSTR("pl")), wj(PSTR(",\"st\":\"err\",\"why\":\"plugin=rescan | del:ID | set:U:ID | clr:U | put:U:ID:a.b.c[:NAME] | empty\"")), wj_end();
}
#endif

// ---- input ----------------------------------------------------------------------------------------------------------
FLASHMEM bool web_op_busy(uint8_t op) {
  if (!wop.op) return false;
  wj_begin(PSTR("busy")), wj(PSTR(",\"op\":\"%c\",\"running\":\"%c\""), op, wop.op), wj_end();
  return true;
}
// A console line for this file: true when it was one.
FLASHMEM bool web_console(const char* p) {
  if (!strcmp(p, PSTR("mem"))) { if (!web_op_busy('M')) wop.op = 'M'; return true; }
  if (!strcmp(p, PSTR("df"))) { if (!web_op_busy('f')) wop.op = 'f'; return true; }
#if RYTM_PLUGINS
  if (!strcmp(p, PSTR("plugins"))) { if (!web_op_busy('P')) wop.op = 'P', wop.phase = 0; return true; }
  if (!strncmp(p, PSTR("plugin="), 7)) { web_plugin_cmd(p + 7); return true; }
#endif
  return false;
}
FLASHMEM uint32_t web_u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
// A 'Y' frame's payload (its CRC checked by the caller).
FLASHMEM void web_frame(const uint8_t* p, int n, bool crc_ok) {
  if (!crc_ok || n < 1) {
    wj_begin(PSTR("frame")), wj(PSTR(",\"st\":\"crc\",\"op\":\"%c\""), n > 0 && p[0] >= 0x20 && p[0] < 0x7f ? p[0] : '?'), wj_end();
    return;
  }
  const uint8_t op = p[0];
  if (op == 'o' && n >= 11) {
    web_open_up(p[1], p[2], web_u32(p + 3), web_u32(p + 7), p + 11, n - 11);
  } else if (op == 'w' && n >= 6) {
    uint8_t id = p[1];
    uint32_t off = web_u32(p + 2);
    int dn = n - 6;
    if (!wx.kind || wx.id != id || wx.kind == 'g') { web_reply(PSTR("up"), id, PSTR("err"), PSTR("not open")); return; }
    wx.t_last = millis();
    if (web_running()) { web_reply(PSTR("up"), id, PSTR("busy"), PSTR("transport")); return; }   // the page resends later
    if (off + dn <= wx.off && wx_pend_n < 0) {   // a resent chunk already written: acknowledged again
      wj_begin(PSTR("up")), wj(PSTR(",\"x\":%d,\"st\":\"ack\",\"off\":%lu"), id, (unsigned long)wx.off), wj_end();
      return;
    }
    if (off != wx.off || wx_pend_n >= 0 || dn > kWebChunk || off + dn > wx.size) {
      wj_begin(PSTR("up")), wj(PSTR(",\"x\":%d,\"st\":\"want\",\"off\":%lu"), id, (unsigned long)wx.off), wj_end();
      return;
    }
    memcpy(wx_pend, p + 6, dn);
    wx_pend_n = dn;
    wx_pend_off = off;
  } else if (op == 'c' && n >= 2) {
    if (wx.kind != 'f' && wx.kind != 'p') { web_reply(PSTR("up"), p[1], PSTR("err"), PSTR("not open")); return; }
    if (wx_pend_n >= 0) { wj_begin(PSTR("up")), wj(PSTR(",\"x\":%d,\"st\":\"want\",\"off\":%lu"), p[1], (unsigned long)wx.off), wj_end(); return; }
    web_commit();
  } else if (op == 'a' && n >= 2) {
    uint8_t id = wx.id;
    bool was = wx.kind != 0;
    web_xfer_close(true);
    web_reply(PSTR("up"), id, PSTR("aborted"), was ? nullptr : PSTR("nothing open"));
  } else if (op == 'g' && n >= 2) {
    web_open_down(p[1], p + 2, n - 2);
  } else if (op == 'r' && n >= 10) {   // id, off u32, n u32 (any length: one line a pass)
    if (web_op_busy('r')) return;
    if (wx.kind != 'g' || wx.id != p[1]) { web_reply(PSTR("dn"), p[1], PSTR("err"), PSTR("not open")); return; }
    wop.op = 'r';
    wop.off = web_u32(p + 2);
    wop.n = web_u32(p + 6);
  } else if ((op == 'l' || op == 'd' || op == 'm') && n >= 2) {
    if (web_op_busy(op)) return;
    if (!web_path(p + 1, n - 1, wop.a, sizeof wop.a)) { wop.a[0] = 0; web_fs_done((char)op, false, PSTR("bad path")); return; }
    wop.op = op;
    wop.phase = 0;
  } else if (op == 'n' && n >= 4) {
    if (web_op_busy(op)) return;
    const uint8_t* z = nullptr;   // (a loop, not memchr: libc's would be linked into ITCM)
    for (int i = 1; i < n && !z; ++i)
      if (!p[i]) z = p + i;
    if (!z || !web_path(p + 1, (int)(z - p - 1), wop.a, sizeof wop.a) ||
        !web_path(z + 1, (int)(p + n - z - 1), wop.b, sizeof wop.b)) { wop.a[0] = 0; web_fs_done('n', false, PSTR("bad path")); return; }
    wop.op = 'n';
  } else {
    wj_begin(PSTR("frame")), wj(PSTR(",\"st\":\"unknown\",\"op\":\"%c\""), op >= 0x20 && op < 0x7f ? op : '?'), wj_end();
  }
}

// From loop(), between requests: at most one card or store operation a pass, rate-limited. true: it did one.
FLASHMEM bool web_pass() {
  if (wx.kind && millis() - wx.t_last > kWebIdleMs) {   // the page went away mid-transfer
    uint8_t id = wx.id;
    const char* t = wx.kind == 'g' ? PSTR("dn") : PSTR("up");
    web_xfer_close(true);
    web_reply(t, id, PSTR("err"), PSTR("timed out"));
  }
  if (wx_pend_n < 0 && !wop.op) return false;
  const uint32_t now = micros();
  if ((int32_t)(now - web_next_us) < 0) return false;
  const bool run = web_running();
  if (wx_pend_n >= 0) {
    if (run) return false;   // held until the transport stops (the frame was taken before it started)
    web_write_pending();
  } else {
    const uint8_t op = wop.op;
    bool more = false;
    if (op == 'l') more = web_list_step();
    else if (op == 'r') more = web_read_chunk();
    else if (op == 'M') web_mem();
    else if (op == 'f') { if (run) return false; web_df(); }
    else if (op == 'd' || op == 'm' || op == 'n') web_fs_op();
#if RYTM_PLUGINS
    else if (op == 'P') more = web_plugins_step();
#endif
    if (!more) wop.op = 0;
  }
  web_next_us = micros() + (run ? kWebGapRunningUs : kWebGapStoppedUs);
  return true;
}
#endif  // RYTM_WEBUI
