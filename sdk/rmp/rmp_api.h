// rmp_api.h - the ABI between the Teensy's voiceloop (the host) and a machine
// plugin (.rmp). Version RMP_API 2 (API 1 plugins still load: every API 2
// addition sits in a reserved slot, sizes unchanged). Shared by the Teensy, the
// Mac harness and plugins. Everything a plugin may call is in rmp_host;
// everything the host calls is in rmp_plugin. Plain C, fixed-size types, no
// host symbols.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RMP_API 2u
#define RMP_API_MIN 1u                 /* the oldest plugin API the host loads */
#define RMP_API_RMPA 3u                /* the newest API an RMPA (AArch64, the Pi) package may have: API 3 (rmp_api3.h);
                                          a plugin built against this header still writes .api = 2u (RMP_API) */
#define RMP_PLUGIN_MAGIC 0x504D5252u   /* "RRMP" little-endian */
#define RMP_PARAMS 16                  /* P1..P16: SRC page P1..P8, SMPL page P9..P16 */

/* API 2: a pool sample, as pool_info reports it. */
typedef struct rmp_pool_info {
  uint32_t frames;       /* frames in the sample */
  uint32_t channels;     /* stored channels pool_read delivers interleaved (1 or 2) */
  float rate;            /* its sample rate */
  uint32_t where;        /* RMP_POOL_PSRAM (all of it, random access) or RMP_POOL_HEAD */
  uint32_t head_frames;  /* RMP_POOL_HEAD: frames 0..head_frames-1 are random access, the rest
                            streams in order (a jump outside head + ring restarts the stream) */
} rmp_pool_info;
enum { RMP_POOL_PSRAM = 1, RMP_POOL_HEAD = 2 };
/* pool_info's result */
enum { RMP_POOL_OK = 0, RMP_POOL_NOT_READY = 1, RMP_POOL_EMPTY = 2 };

typedef struct rmp_host {
  uint32_t api;                 /* RMP_API, or 1 for an API-1 plugin (which never sees reserved[]) */
  float sample_rate;            /* 48000 */
  int32_t max_block;            /* the largest n render() is called with (12 today) */
  void (*log)(const char *msg); /* a line in the Teensy's log, not from render() */
  uint32_t (*random)(uint32_t *state);   /* xorshift32 on the caller's state */
  /* API 2: [RMP_H_POOL_INFO] int32_t pool_info(int32_t slot, rmp_pool_info *info):
       pool slot 1..127 of the project; RMP_POOL_OK with *info, RMP_POOL_NOT_READY while the host
       prepares it (asked for a slot this voice does not stream: a job between render requests),
       RMP_POOL_EMPTY when there is no such sample. Call it about once a block (render, note_on), not
       per sample: the Teensy runs it from flash. Asked in voice_init, its prepare starts once the voice
       plays the slot; in voice_leave it answers RMP_POOL_EMPTY.
     [RMP_H_POOL_READ] int32_t pool_read(int32_t slot, uint32_t pos, float *buf, int32_t n):
       n frames from frame pos, the stored channels interleaved, -1..1; returns the frames
       delivered (fewer when not there yet: play silence for the rest). For the voice the host
       is calling. Callable from render().
     [2] API 3 (rmp_api3.h RMP_H_V3): const rmp3_host *, NULL on an API 2 host.
     [3..10] later APIs, NULL. */
  void *reserved[11];
} rmp_host;
#define RMP_H_POOL_INFO 0
#define RMP_H_POOL_READ 1

typedef struct rmp_plugin {
  uint32_t magic;               /* RMP_PLUGIN_MAGIC */
  uint32_t api;                 /* RMP_API the plugin was built for (1 or 2) */
  uint32_t state_bytes;         /* per voice, 8-byte aligned by the host, zeroed before voice_init */
  uint32_t cpu_permille;        /* declared CPU per voice in 1/1000 of the Teensy (guard budget) */
  /* once after loading; nonzero = refuse (the plugin is unloaded) */
  int32_t (*init)(const rmp_host *host);
  /* a voice starts using this machine (state zeroed) */
  void (*voice_init)(void *state);
  /* a trig: note in semitones (60 = C4), velocity 0..1 */
  void (*note_on)(void *state, float note, float velocity);
  /* the voice's gate closed (Rytm 0x11) */
  void (*note_off)(void *state);
  /* P1..P16, each 0..127 with the 7-bit fraction (value + fraction/128) */
  void (*params)(void *state, const float *p);
  /* n samples (1..max_block), mono, written (not added) to out, nominal level +-1 */
  void (*render)(void *state, float *out, int32_t n);
  /* API 2 (an API-1 plugin's are NULL and never read; a NULL one turns the feature off):
     [RMP_P_PARSE]       banks: later, NULL now
     [RMP_P_LOAD_ENTRY]  int32_t load_entry(void *state, const uint8_t *bytes, int32_t len, int16_t values[16]):
                         a native preset file: set this voice's hidden state and fill values[k] (P(k+1)'s
                         0..127) for the preset-role knobs, -1 = untouched; between render calls, guarded
     [RMP_P_VOICE_ENTER] void voice_enter(void *state): after voice_init, each time a voice starts this slot
     [RMP_P_VOICE_LEAVE] void voice_leave(void *state): before the voice leaves this slot or it unloads
     [RMP_P_V3 = 4] API 3 (rmp_api3.h): const rmp3_plugin *, NULL = a plain API 2 plugin
     [5..7] later APIs, NULL */
  void *reserved[8];
} rmp_plugin;
#define RMP_P_PARSE 0
#define RMP_P_LOAD_ENTRY 1
#define RMP_P_VOICE_ENTER 2
#define RMP_P_VOICE_LEAVE 3

/* A function in a reserved slot of a table: RMP_FN(load_entry). */
#define RMP_FN(f) ((void *)(f))
typedef int32_t (*rmp_load_entry_fn)(void *state, const uint8_t *bytes, int32_t len, int16_t values[16]);
typedef void (*rmp_voice_fn)(void *state);
typedef int32_t (*rmp_pool_info_fn)(int32_t slot, rmp_pool_info *info);
typedef int32_t (*rmp_pool_read_fn)(int32_t slot, uint32_t pos, float *buf, int32_t n);
/* The host reads a plugin's API-2 functions only through these (NULL for an API-1 plugin). */
static inline rmp_load_entry_fn rmp_load_entry_of(const rmp_plugin *p) {
  return p->api >= 2u ? (rmp_load_entry_fn)p->reserved[RMP_P_LOAD_ENTRY] : (rmp_load_entry_fn)0;
}
static inline rmp_voice_fn rmp_voice_enter_of(const rmp_plugin *p) {
  return p->api >= 2u ? (rmp_voice_fn)p->reserved[RMP_P_VOICE_ENTER] : (rmp_voice_fn)0;
}
static inline rmp_voice_fn rmp_voice_leave_of(const rmp_plugin *p) {
  return p->api >= 2u ? (rmp_voice_fn)p->reserved[RMP_P_VOICE_LEAVE] : (rmp_voice_fn)0;
}
/* A plugin reads the host's API-2 functions only through these (NULL from an API-1 host table). */
static inline rmp_pool_info_fn rmp_pool_info_of(const rmp_host *h) {
  return h->api >= 2u ? (rmp_pool_info_fn)h->reserved[RMP_H_POOL_INFO] : (rmp_pool_info_fn)0;
}
static inline rmp_pool_read_fn rmp_pool_read_of(const rmp_host *h) {
  return h->api >= 2u ? (rmp_pool_read_fn)h->reserved[RMP_H_POOL_READ] : (rmp_pool_read_fn)0;
}

/* sizeof(rmp_plugin) on the 32-bit Teensy: 4 u32 + 6 function pointers + 8 reserved pointers. The loader
   and tools/rmp_pack.py use this fixed number (a 64-bit Mac build has bigger pointers), so the ARM
   build proves it. */
#define RMP_PLUGIN_BYTES 72u
/* sizeof(rmp_plugin) built for AArch64 (the Pi, RMPA packages; also the arm64 Mac): 4 u32 + 14 pointers. */
#define RMP_PLUGIN_BYTES_A64 128u
#ifdef __arm__
#ifdef __cplusplus
static_assert(sizeof(rmp_plugin) == RMP_PLUGIN_BYTES, "rmp_plugin size (RMP_PLUGIN_BYTES, tools/rmp_pack.py)");
#else
_Static_assert(sizeof(rmp_plugin) == RMP_PLUGIN_BYTES, "rmp_plugin size (RMP_PLUGIN_BYTES, tools/rmp_pack.py)");
#endif
#endif
#ifdef __aarch64__
#ifdef __cplusplus
static_assert(sizeof(rmp_plugin) == RMP_PLUGIN_BYTES_A64, "rmp_plugin size on AArch64 (RMP_PLUGIN_BYTES_A64)");
#else
_Static_assert(sizeof(rmp_plugin) == RMP_PLUGIN_BYTES_A64, "rmp_plugin size on AArch64 (RMP_PLUGIN_BYTES_A64)");
#endif
#endif

/* A plugin exports exactly one of these, named rmp_entry. */
#ifdef __cplusplus
#define RMP_LINKAGE extern "C"   /* so a .cpp plugin's const rmp_entry is a global, unmangled symbol */
#else
#define RMP_LINKAGE
#endif
#ifdef RMP_NATIVE   /* the native build (a .dylib/.so): no ELF section, just exported */
#define RMP_ENTRY RMP_LINKAGE __attribute__((used, visibility("default")))
#else
#define RMP_ENTRY RMP_LINKAGE __attribute__((used, section(".rmp_entry")))
#endif

#ifdef __cplusplus
}
#endif
