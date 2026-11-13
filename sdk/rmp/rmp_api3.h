/* rmp_api3.h - Plugin API 3: user machines and user effects on the Pi (docs/plugin-api-3-plan.md has what this
   engine implements of it and where it differs from the published proposal). API 3 extends rmp_api.h; every API 1
   and API 2 plugin loads and plays as before. The Sidecar Mini (Teensy) stays on API 2.

   Two ways to build:
     dual        rmp_plugin.api = 2 (a literal), reserved[RMP_P_V3] = an rmp3_plugin. An older host loads it as a plain
                 API 2 machine (it never reads reserved[4..7]); an API 3 host calls the rmp3_plugin's render instead of
                 rmp_plugin.render. The default for machines.
     API 3 only  rmp_plugin.api = 3, reserved[RMP_P_V3] = an rmp3_plugin, rmp_plugin.render NULL. An older host refuses
                 the package at the scan. For effects (RMP3_KIND_EFFECT): an older host must not load one as a machine.

   Plain C, fixed-size types, no host symbols. Sizes are given for AArch64 (the Pi, RMPA packages) and asserted there. */
#pragma once
#include "rmp_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define RMP3_MAGIC 0x33504D52u   /* "RMP3" */
#define RMP3_MINOR 0u            /* grows with fields added at the end; size says how many */
#define RMP_API3 3u              /* rmp_plugin.api of an API 3 only table */
#define RMP_H_V3 2               /* rmp_host.reserved[2]: const rmp3_host *, NULL on an older host */
#define RMP_P_V3 4               /* rmp_plugin.reserved[4]: const rmp3_plugin *, NULL = plain API 2 */

enum {                           /* what a host has (rmp3_host.caps), a plugin needs or wants */
  RMP3_CAP_TRANSPORT = 1u << 0,  /* b->transport */
  RMP3_CAP_ARENA     = 1u << 1,  /* the slot arena */
  RMP3_CAP_GUARD     = 1u << 2,  /* guarded setup and control calls */
  RMP3_CAP_STEREO    = 1u << 3,  /* out[1] */
  RMP3_CAP_AUDIO_IN  = 1u << 4,  /* in[], aux[] */
  RMP3_CAP_EFFECT    = 1u << 5,  /* RMP3_KIND_EFFECT */
  RMP3_CAP_BUS       = 1u << 6,  /* the signal bus */
  RMP3_CAP_BLOCK_P   = 1u << 7,  /* b->p, modulation without params() */
  RMP3_CAP_PAGES     = 1u << 8,  /* the manifest's pages */
};

typedef struct rmp3_host {       /* 96 B on AArch64 */
  uint32_t magic;                /* RMP3_MAGIC */
  uint32_t size;                 /* sizeof as the host was built: never read past it */
  uint32_t minor;
  uint32_t caps;                 /* RMP3_CAP_* */
  uint32_t board;                /* 4 or 5; 0 on the Mac's test host */
  uint32_t cpu_mhz;              /* the clock now */
  uint32_t arena_limit;          /* the most one slot's arena may be, bytes */
  uint32_t reserved0;
  void *reserved[8];
} rmp3_host;

static inline const rmp3_host *rmp3_host_of(const rmp_host *h) {
  const rmp3_host *x = h->api >= 2u ? (const rmp3_host *)h->reserved[RMP_H_V3] : (const rmp3_host *)0;
  return x && x->magic == RMP3_MAGIC ? x : (const rmp3_host *)0;
}

enum { RMP3_KIND_MACHINE = 1u << 0, RMP3_KIND_EFFECT = 1u << 1 };
enum {                           /* where a state runs (rmp3_plugin.places, rmp3_voice.place, b->place) */
  RMP3_AT_VOICE  = 1u << 0,      /* a machine on a voice */
  RMP3_AT_INSERT = 1u << 1,      /* a voice's INSERT slot */
  RMP3_AT_BUS    = 1u << 2,      /* a bus's INSERT slot */
  RMP3_AT_SEND   = 1u << 3,      /* a send FX slot */
  RMP3_AT_MASTER = 1u << 4,      /* the master chain */
};

/* ---- the block: everything one render call needs, as data */
enum { RMP3_SIG_CONTROL = 0, RMP3_SIG_AUDIO = 1 };
enum { RMP3_U_AUDIO = 0, RMP3_U_BIPOLAR = 1, RMP3_U_UNIPOLAR = 2, RMP3_U_GATE = 3 };
typedef struct rmp3_sig_decl {   /* 12 B */
  char name[8];                  /* "ENV", NUL-padded: shown in the routing UI */
  uint8_t rate;                  /* RMP3_SIG_* */
  uint8_t unit;                  /* RMP3_U_* */
  uint16_t reserved;
} rmp3_sig_decl;
typedef struct rmp3_sig_in {     /* 16 B */
  const float *audio;            /* n frames of the block before, or zeros; never NULL */
  float value;                   /* the source's value at the end of the block before, or 0 */
  uint32_t flags;                /* RMP3_SI_ROUTED: a live source is behind it */
} rmp3_sig_in;
enum { RMP3_SI_ROUTED = 1u << 0 };

enum {
  RMP3_T_RUNNING = 1u << 0,      /* the Rytm's sequencer plays */
  RMP3_T_CLOCKED = 1u << 1,      /* a MIDI clock tick came in the last 250 ms */
  RMP3_T_LOCKED  = 1u << 2,      /* song_tick follows the Rytm's song position */
  RMP3_T_START   = 1u << 3,      /* Start or Continue in this block */
  RMP3_T_STOP    = 1u << 4,      /* Stop in this block */
  RMP3_T_JUMP    = 1u << 5,      /* song_tick moved by more than the block's time */
};
typedef struct rmp3_transport {  /* 32 B */
  uint32_t flags;                /* RMP3_T_* */
  float bpm;                     /* 60e6 / (24 x tick_us); 120 until measured */
  float tick_us;                 /* one 24 ppqn tick, smoothed */
  float free_tick;               /* 0..2304, wraps; runs on at the last tempo when stopped (the LFOs' clock) */
  int32_t song_tick;             /* ticks from the song's start, SPP x 6 + ticks; RMP3_T_LOCKED only, else -1 */
  float song_frac;               /* 0..1 inside song_tick */
  uint32_t step16;               /* song_tick / 6 (0 while not locked) */
  uint32_t lead_frames;          /* frames from this block's render to the Rytm's outputs (the estimate) */
} rmp3_transport;

enum {
  RMP3_B_MONO_OUT = 1u << 0,     /* out[1] is NULL: a mono route */
  RMP3_B_WET_ONLY = 1u << 1,     /* a send slot: write only the wet signal */
};
typedef struct rmp3_block {      /* 112 B on AArch64 */
  uint32_t size;                 /* sizeof as the host was built */
  int32_t n;                     /* frames, 1..16; 12 on a voice */
  uint32_t flags;                /* RMP3_B_* */
  uint32_t gen;                  /* the block's number, wraps */
  float *out[2];                 /* written, not added; nominal +-1 */
  const float *in[2];            /* an effect's input; in[1] NULL when mono; NULL on a machine */
  const float *aux[2];           /* the side input the user chose, or zeros; NULL if not declared */
  const float *p;                /* P1..P16 as they play now (RMP3_CAP_BLOCK_P), else NULL */
  uint32_t p_changed;            /* bit k: P(k+1) differs from the last block */
  uint32_t place;                /* RMP3_AT_* */
  const rmp3_transport *transport;   /* the block's snapshot, never NULL */
  const rmp3_sig_in *sig_in;     /* sig_in_n entries */
  float *const *sig_audio;       /* sig_out_n pointers: n frames to write, NULL for a control signal */
  float *sig_value;              /* sig_out_n floats: written whenever it changes, kept between blocks */
} rmp3_block;

/* ---- setup and the states */
typedef struct rmp3_setup {      /* 40 B on AArch64 */
  const rmp_host *host;
  const rmp3_host *host3;
  void *shared;                  /* the slot's shared part, zeroed; NULL if 0 bytes */
  uint32_t shared_bytes;         /* what was granted: want, else min */
  uint32_t voice_bytes;          /* the same, per state */
  uint32_t states;               /* states reserved */
  int32_t slot;                  /* U1..U5 = 1..5, UFX1..UFX4 = 64..67, the TYPE ids */
} rmp3_setup;
typedef struct rmp3_voice {      /* 40 B on AArch64 */
  void *shared;
  void *arena;                   /* this state's part; not zeroed again after the load; NULL if 0 bytes */
  uint32_t shared_bytes;
  uint32_t arena_bytes;
  int32_t slot;
  int32_t voice;                 /* 0..7, -1 on a bus, send or master position */
  int32_t instance;              /* 0.. on a poly track, -1 for a voice's own state */
  uint32_t place;                /* RMP3_AT_* */
} rmp3_voice;

/* ---- a page's live data (declared pages: not implemented by this engine yet) */
enum { RMP3_VIEW_PEAKS = 1, RMP3_VIEW_TEXT = 2 };
typedef struct rmp3_view {
  uint8_t kind;                  /* RMP3_VIEW_* */
  uint8_t start, end;            /* the played region, 0..127 */
  uint8_t nslices;
  uint16_t columns;              /* peaks: columns used, 1..128 */
  uint8_t lines;                 /* text: lines used, 1..8 */
  uint8_t selected;              /* text: the line shown inverted, 255 none */
  int8_t peaks[256];             /* min, max per column, -127..127 */
  uint8_t slices[16];            /* 0..127 */
  char text[8][33];              /* NUL-terminated, 32 characters at most */
} rmp3_view;

/* ---- the plugin's table, read once at load */
typedef struct rmp3_plugin {     /* 136 B on AArch64 */
  uint32_t magic;                /* RMP3_MAGIC */
  uint32_t size;                 /* sizeof as the plugin was built */
  uint32_t minor;                /* RMP3_MINOR */
  uint32_t kind;                 /* RMP3_KIND_* */
  uint32_t needs;                /* RMP3_CAP_*: without one the slot is BAD, "needs ..." */
  uint32_t wants;                /* used when the host has it */
  uint32_t shared_min, shared_want;   /* arena: one part per slot */
  uint32_t voice_min, voice_want;     /* arena: one part per state */
  uint16_t states;               /* states to reserve, 0 = the host's default */
  uint16_t latency_frames;       /* an effect's own delay, 0..4096 */
  uint8_t out_channels;          /* 1 or 2 */
  uint8_t in_channels;           /* an effect: 1 or 2; a machine: 0 */
  uint8_t aux_channels;          /* 0, 1 or 2 */
  uint8_t places;                /* RMP3_AT_* */
  uint8_t sig_out_n, sig_in_n;   /* 0..4 each */
  uint8_t pad[6];
  const rmp3_sig_decl *sig_out;
  const rmp3_sig_decl *sig_in;
  int32_t (*setup)(const rmp3_setup *s);                  /* after init, per slot: 0 done, RMP3_SETUP_MORE call
                                                             again (next idle step), else refuse */
  void (*voice_attach)(void *state, const rmp3_voice *v); /* after voice_init */
  void (*render)(void *state, const rmp3_block *b);       /* replaces rmp_plugin.render */
  int32_t (*view)(void *state, int32_t id, rmp3_view *v); /* a page's live data */
  void *reserved[4];
} rmp3_plugin;

/* ---- modulation hints and declared pages (the manifest's RX31 block: not implemented by this engine yet) */
enum { RMP3_MOD_CONT = 0, RMP3_MOD_STEP = 1, RMP3_MOD_TRIG = 2, RMP3_MOD_OFF = 3 };
typedef struct rmp3_knob_hint {  /* 6 B, packed from the manifest */
  uint8_t p;                     /* 1..16 */
  uint8_t mode;                  /* RMP3_MOD_* */
  uint8_t lo, hi;                /* the modulated value stays in lo..hi */
  uint16_t smooth_us;            /* 0 = the host's 3 ms */
} rmp3_knob_hint;
enum {
  RMP3_W_KNOB, RMP3_W_KNOBBI, RMP3_W_BAR, RMP3_W_PAN, RMP3_W_ENUM, RMP3_W_FILTER, RMP3_W_ENV, RMP3_W_WAVE, RMP3_W_LFO,
  RMP3_W_WAVEFORM, RMP3_W_TEXT, RMP3_W_METER, RMP3_W_LIST
};
typedef struct rmp3_cell {       /* 16 B */
  uint8_t kind;                  /* RMP3_W_* */
  uint8_t fallback;              /* the kind an older host draws instead */
  uint8_t col, row, cols, rows;
  uint8_t in[4];                 /* P numbers 0..15, 255 unused */
  uint8_t arg;                   /* per kind, as core/ui/page.h */
  uint8_t sig;                   /* meter: signal 0..3 of this state, 255 none */
  uint8_t view;                  /* waveform, text, list: view id, 255 none */
  uint8_t flags;
  uint16_t names;                /* offset of '\0'-separated names in the block, 0 none */
} rmp3_cell;
typedef struct rmp3_page {       /* 64 B, then nrytm + ntouch cells */
  char title[12];
  uint8_t nrytm, ntouch;
  uint8_t flags;
  uint8_t reserved;
  uint8_t enc[8];                /* encoder A..H -> P number 0..15, 255 none */
  char label[8][5];
} rmp3_page;

typedef void (*rmp3_render_fn)(void *state, const rmp3_block *b);

/* setup()'s "not done yet": the host calls it again from its next idle step, with the engine playing in between. init()
   and each setup() call run on the control core between blocks: keep each under 1 ms (the host refuses the package
   past it), and do heavy work (tables, a big arena's layout) in pieces. */
#define RMP3_SETUP_MORE 0x45524F4D   /* "MORE" */
/* A dual package (an API 2 table with an rmp3_plugin in reserved[RMP_P_V3]) is held to these limits by an API 3 host
   from its init() on (the 1 ms, the three late control calls in a row, one control call over 2 ms), though an API 2 host, which never reads the
   rmp3_plugin, holds it to none: adding the table to an API 2 machine means its init() too must fit in 1 ms (move
   table building into setup(), in pieces). */
/* Host rule (the guarded calls): a host service a plugin can call reads or writes the plugin's memory before it takes
   any lock, so a bad pointer faults with no host lock held. */

#ifdef __aarch64__
#ifdef __cplusplus
#define RMP3_ASSERT(c, m) static_assert(c, m)
#else
#define RMP3_ASSERT(c, m) _Static_assert(c, m)
#endif
RMP3_ASSERT(sizeof(rmp3_host) == 96, "rmp3_host is 96 B");
RMP3_ASSERT(sizeof(rmp3_transport) == 32, "rmp3_transport is 32 B");
RMP3_ASSERT(sizeof(rmp3_block) == 112, "rmp3_block is 112 B");
RMP3_ASSERT(sizeof(rmp3_setup) == 40, "rmp3_setup is 40 B");
RMP3_ASSERT(sizeof(rmp3_voice) == 40, "rmp3_voice is 40 B");
RMP3_ASSERT(sizeof(rmp3_plugin) == 136, "rmp3_plugin is 136 B");
RMP3_ASSERT(sizeof(rmp3_sig_decl) == 12 && sizeof(rmp3_sig_in) == 16, "signal declarations");
RMP3_ASSERT(sizeof(rmp3_cell) == 16 && sizeof(rmp3_page) == 64 && sizeof(rmp3_knob_hint) == 6, "page and hint records");
#undef RMP3_ASSERT
#endif
#define RMP3_PLUGIN_BYTES_A64 136u   /* the smallest rmp3_plugin.size a host accepts (the minor 0 table) */

#ifdef __cplusplus
}
#endif
