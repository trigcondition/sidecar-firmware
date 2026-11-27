// teensy/mirror_relay/relay.cpp - the Teensy side of usb=mirror through a Teensy (pi/engine/relay_proto.h,
// docs/teensy-mirror.md): USB1 is the link to the Pi, USB2 shows the box the Pi mirrors to the computer.
//
// All of it runs in the two ports' interrupts (same priority: never one inside the other). loop() only blinks.
#include "relay.h"
#include "ci_dev.h"
#include "imxrt.h"
#include "core_pins.h"
#include <relay_proto.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

using namespace relay;

static const uint8_t kAudioInEp = 0x83, kAudioOutEp = 0x03;	// ob::kInEp / ob::kOutEp (ob_block.h)

// ---------------------------------------------------------------- state (DTCM: plain globals)

static ci_qh qh1[16] __attribute__ ((aligned (4096)));
static ci_qh qh2[16] __attribute__ ((aligned (4096)));
static ci_dev L;		// USB1: the link to the Pi
static ci_dev P;		// USB2: the computer

static volatile Stats st;
volatile uint32_t relay_stage;
static volatile uint32_t reboot_at;		// kReqBootloader: when loop() reboots into the bootloader (0: never)

// the box's descriptors as the Pi uploaded them
static uint8_t dev_desc[18] __attribute__ ((aligned (32)));
static bool have_dev;
static uint8_t cfg_desc[mirror::kMaxConfig] __attribute__ ((aligned (32)));
static unsigned cfg_len;
struct Str { uint8_t index, len; uint8_t data[254]; };
static Str strs[mirror::kMaxStrings];
static unsigned n_strs;
static uint8_t bos_desc[256];
static unsigned bos_len;
static uint8_t msos_set[512];
static unsigned msos_len;
static uint8_t msos_code;
static Map map;
static bool shown;
static volatile bool connect_due;		// kReqConnect(1) taken: loop() raises the pull-up once 300 ms have passed since it dropped
static volatile uint32_t pc_off_at;		// millis() when USB2 left the bus (0: never on it)
static uint8_t pc_config;
static uint8_t pc_alt[mirror::kMaxIfaces];

// EP0 buffers
static uint8_t l_ep0[1024] __attribute__ ((aligned (32)));	// the link's vendor requests (both directions)
static uint8_t l_setup[8];
static uint8_t p_ep0[512] __attribute__ ((aligned (32)));	// the computer's: answers and OUT data stages
static uint8_t p_ctl_data[512];
static uint16_t p_ctl_data_len;
static CtlSlot ctl;

// events
static const unsigned kEvRing = 64;
static Event ev_ring[kEvRing];
static unsigned ev_head, ev_tail;
static ci_td ev_td[2];
static uint8_t ev_buf[2][kEvMps] __attribute__ ((aligned (32)));
static uint8_t ev_busy[2];

// audio: link -> computer (blocks for 0x83) and computer -> link (blocks from 0x03)
// T337: the Engine's reads of the A4's IN came late enough that a 16-block queue (2.3 ms) overflowed (69 blocks in 60 s):
// 48 blocks (7 ms) waiting, 4 primed at once toward each side
static const unsigned kAQ = 48, kALinkTds = 4, kAPcTds = 4;
struct Block { uint8_t d[kLinkMps]; uint16_t len; uint32_t t; };
static Block aq_pc[kAQ];			// waiting for the computer
static unsigned aq_pc_head, aq_pc_tail;
static Block aq_pi[kAQ];			// waiting for the Pi
static unsigned aq_pi_head, aq_pi_tail;
static ci_td a_from_pi_td[kALinkTds];
static uint8_t a_from_pi_buf[kALinkTds][kLinkMps] __attribute__ ((aligned (32)));
static ci_td a_to_pc_td[kAPcTds];
static uint8_t a_to_pc_buf[kAPcTds][kLinkMps] __attribute__ ((aligned (32)));
static uint32_t a_to_pc_t[kAPcTds];
static uint8_t a_to_pc_busy[kAPcTds];
static ci_td a_from_pc_td[kALinkTds];
static uint8_t a_from_pc_buf[kALinkTds][kLinkMps] __attribute__ ((aligned (32)));
static ci_td a_to_pi_td[kAPcTds];
static uint8_t a_to_pi_buf[kAPcTds][kLinkMps] __attribute__ ((aligned (32)));
static uint32_t a_to_pi_t[kAPcTds];
static uint8_t a_to_pi_busy[kAPcTds];
static uint16_t a_out_mps;			// the mirrored audio OUT endpoint's size: a dTD of it ends on each packet

// channels: per channel and direction kChanDepth buffers, each with a dTD for each port (zero-copy hand-over)
enum BufState : uint8_t { kFree, kOnLink, kOnPc };
struct ChanBuf { uint8_t d[kLinkMps]; ci_td tl, tp; uint8_t state; uint16_t len; } __attribute__ ((aligned (32)));
static ChanBuf ch_to_pc[kChannels][kChanDepth] __attribute__ ((aligned (32)));	// link OUT -> mirrored IN
static ChanBuf ch_to_pi[kChannels][kChanDepth] __attribute__ ((aligned (32)));	// mirrored OUT -> link IN
static uint8_t credits[kChannels];

// log
static char log_ring[2048];
static unsigned log_head, log_tail;

static inline uint32_t cyc_us (uint32_t cyc) { return cyc / (F_CPU_ACTUAL / 1000000u); }

void rlog (const char *fmt, ...)
{
	char b[120];
	va_list ap;
	va_start (ap, fmt);
	int n = vsnprintf (b, sizeof b - 1, fmt, ap);
	va_end (ap);
	if (n < 0) return;
	if (n > (int) sizeof b - 2) n = sizeof b - 2;
	b[n++] = '\n';
	__disable_irq ();
	for (int i = 0; i < n; i++)
	{
		if (log_head - log_tail >= sizeof log_ring) log_tail++;
		log_ring[log_head++ % sizeof log_ring] = b[i];
	}
	__enable_irq ();
}

// ---------------------------------------------------------------- events to the Pi

static void ev_pump (void)
{
	if (!L.ep[kEvEp & 7][1].open) return;
	for (unsigned k = 0; k < 2 && ev_head != ev_tail; k++)
	{
		if (ev_busy[k]) continue;
		memcpy (ev_buf[k], &ev_ring[ev_tail++ % kEvRing], sizeof (Event));
		ev_busy[k] = 1;
		ev_td[k].param = k;
		ci_queue (&L, kEvEp, &ev_td[k], ev_buf[k], sizeof (Event));
	}
}

static void ev_post (uint8_t type, uint8_t a, uint8_t b = 0, const uint8_t *data = 0)
{
	if (!L.ep[kEvEp & 7][1].open) return;
	if (ev_head - ev_tail >= kEvRing) { st.events_drop++; return; }
	Event &e = ev_ring[ev_head % kEvRing];
	memset (&e, 0, sizeof e);
	e.type = type, e.a = a, e.b = b;
	e.t_us = micros ();
	if (data) memcpy (e.data, data, 8);
	ev_head++;
	st.events++;
	ev_pump ();
}

static void ev_done (ci_dev *, uint8_t, ci_td *t, uint32_t, uint32_t)
{
	ev_busy[t->param] = 0;
	ev_pump ();
}

// ---------------------------------------------------------------- audio

// The computer idle on the box's IN (T339/T340: the Overbridge Engine stops reading after its latency check and starts
// again with the app): blocks that arrive meanwhile are not kept, or the computer starts on a full queue of stale blocks
// and, reading in step with its own OUT, never drains it (every later jitter then overflows it: breaks). Idle: nothing
// primed, the newest block kept; the computer's next poll (its NAK, loop()) primes from the newest.
static const uint32_t kPcIdleUs = 20000;
static volatile bool pc_in_idle = true;
static uint32_t pc_in_last_take;

// Paced IN (T341: the returned sine slipped 1-2 samples every ~2.6 s through the relay, not on the A4 alone): blocks reach
// the Teensy in bunches (the link, the Pi's IRQ), while the A4 itself has one ready every 7/48000 s. The computer's
// Overbridge Engine follows the box's clock from when its blocks come; so they are handed out one per microframe at the
// box's mean rate (48000 / 7 / 8000 a microframe, a phase accumulator at each SOF), kept ~kPaceTarget blocks behind the
// newest, the rate trimmed slowly toward that level (at most +-2 %).
static const bool kPace = true;
static const uint32_t kPaceBase = 56174;		// 65536 * 48000 / 7 / 8000
static const int32_t kPaceTarget = 4, kPaceGain = 280, kPaceClamp = 1123;	// blocks; Q16 per block of level error; 2 %
static uint32_t pace_acc;
static int32_t pace_lvl;				// the level, smoothed (Q8)

static void a_to_pc_prime_one (void)
{
	for (unsigned k = 0; k < kAPcTds && aq_pc_head != aq_pc_tail; k++)
	{
		if (a_to_pc_busy[k]) continue;
		Block &b = aq_pc[aq_pc_tail++ % kAQ];
		memcpy (a_to_pc_buf[k], b.d, b.len);
		a_to_pc_t[k] = b.t;
		a_to_pc_busy[k] = 1;
		a_to_pc_td[k].param = k;
		ci_queue (&P, kAudioInEp, &a_to_pc_td[k], a_to_pc_buf[k], b.len);
		st.pace_primed++;
		return;
	}
}

static void p_sof (ci_dev *)
{
	if (!kPace || !P.ep[kAudioInEp & 7][1].open || pc_in_idle) return;
	unsigned busy = 0;
	for (unsigned k = 0; k < kAPcTds; k++) busy += a_to_pc_busy[k];
	int32_t level = (int32_t) (aq_pc_head - aq_pc_tail) + (int32_t) busy;
	pace_lvl += (level * 256 - pace_lvl) / 64;		// ~8 ms
	int32_t trim = (pace_lvl - kPaceTarget * 256) * kPaceGain / 256;
	if (trim > kPaceClamp) trim = kPaceClamp;
	if (trim < -kPaceClamp) trim = -kPaceClamp;
	pace_acc += (uint32_t) ((int32_t) kPaceBase + trim);
	if (pace_acc < 65536) return;
	pace_acc -= 65536;
	if (pace_acc > 65536) pace_acc = 65536;		// none queued for a while: no burst after
	if (busy >= 2) { st.pace_held++; return; }	// the computer has not taken the last two yet
	if (aq_pc_head == aq_pc_tail) { st.pace_empty++; return; }
	a_to_pc_prime_one ();
}

static void a_to_pc_pump (void)
{
	if (!P.ep[kAudioInEp & 7][1].open || pc_in_idle || kPace) return;
	for (unsigned k = 0; k < kAPcTds && aq_pc_head != aq_pc_tail; k++)
	{
		if (a_to_pc_busy[k]) continue;
		Block &b = aq_pc[aq_pc_tail++ % kAQ];
		memcpy (a_to_pc_buf[k], b.d, b.len);
		a_to_pc_t[k] = b.t;
		a_to_pc_busy[k] = 1;
		a_to_pc_td[k].param = k;
		ci_queue (&P, kAudioInEp, &a_to_pc_td[k], a_to_pc_buf[k], b.len);
	}
}

static uint32_t pc_in_last, pc_out_last;
static bool pc_in_seen, pc_out_seen;

static void gap_note (uint32_t *last, bool *seen, volatile uint32_t *hist, volatile uint32_t *maxv)
{
	uint32_t now = ARM_DWT_CYCCNT;
	if (*seen)
	{
		uint32_t us = cyc_us (now - *last);
		if (us < 1000000u)
		{
			hist[gap_bin (us)]++;
			if (us > *maxv) *maxv = us;
		}
	}
	*seen = true, *last = now;
}

static void a_to_pc_done (ci_dev *, uint8_t, ci_td *t, uint32_t, uint32_t err)
{
	unsigned k = t->param;
	a_to_pc_busy[k] = 0;
	if (err) st.pc_audio_err++;
	else
	{
		gap_note (&pc_in_last, &pc_in_seen, st.pc_in_gap, &st.pc_in_gap_max_us);
		pc_in_last_take = ARM_DWT_CYCCNT;
	}
	if (!err)
	{
		st.audio_to_pc++;
		uint32_t us = cyc_us (ARM_DWT_CYCCNT - a_to_pc_t[k]);
		st.hist_to_pc[hist_bin (us)]++;
		if (us > st.worst_to_pc_us) st.worst_to_pc_us = us;
	}
	bool idle = aq_pc_head == aq_pc_tail;
	for (unsigned i = 0; i < kAPcTds; i++) idle = idle && !a_to_pc_busy[i];
	if (idle) st.audio_to_pc_empty++;
	a_to_pc_pump ();
}

static uint32_t link_last_cyc;
static bool link_seen;

static void a_from_pi_done (ci_dev *, uint8_t, ci_td *t, uint32_t len, uint32_t err)
{
	unsigned k = t->param;
	uint32_t now = ARM_DWT_CYCCNT;
	if (link_seen)
	{
		uint32_t us = cyc_us (now - link_last_cyc);
		if (us < 1000000u)
		{
			st.link_gap_hist[hist_bin (us)]++;
			if (us > st.link_gap_max_us) st.link_gap_max_us = us;
		}
	}
	link_seen = true, link_last_cyc = now;
	if (ci_queued (&L, kAudioFromPiEp) == 0) st.link_dry++;	// this was the last armed one (re-armed below)
	if (!err && P.ep[kAudioInEp & 7][1].open)
	{
		if (!pc_in_idle && cyc_us (now - pc_in_last_take) > kPcIdleUs)	// the computer stopped taking blocks
		{
			pc_in_idle = true;
			ci_ep_flush (&P, kAudioInEp);
			ci_nak_take (&P, kAudioInEp);		// a NAK from before the flush is not the computer coming back
			memset (a_to_pc_busy, 0, sizeof a_to_pc_busy);
			st.pc_idle_enter++;
		}
		if (pc_in_idle) aq_pc_tail = aq_pc_head;		// keep only the block below, the newest
		else if (aq_pc_head - aq_pc_tail >= kAQ) { aq_pc_tail++; st.audio_to_pc_drop++; }
		Block &b = aq_pc[aq_pc_head++ % kAQ];
		memcpy (b.d, a_from_pi_buf[k], len);
		b.len = (uint16_t) len;
		b.t = ARM_DWT_CYCCNT;
		unsigned q = aq_pc_head - aq_pc_tail;
		if (q > st.q_to_pc_max) st.q_to_pc_max = q;
		a_to_pc_pump ();
	}
	ci_queue (&L, kAudioFromPiEp, t, a_from_pi_buf[k], kLinkMps);
}

static void a_to_pi_pump (void)
{
	if (!L.ep[kAudioToPiEp & 7][1].open) return;
	for (unsigned k = 0; k < kAPcTds && aq_pi_head != aq_pi_tail; k++)
	{
		if (a_to_pi_busy[k]) continue;
		Block &b = aq_pi[aq_pi_tail++ % kAQ];
		memcpy (a_to_pi_buf[k], b.d, b.len);
		a_to_pi_t[k] = b.t;
		a_to_pi_busy[k] = 1;
		a_to_pi_td[k].param = k;
		ci_queue (&L, kAudioToPiEp, &a_to_pi_td[k], a_to_pi_buf[k], b.len);
	}
}

static void a_to_pi_done (ci_dev *, uint8_t, ci_td *t, uint32_t, uint32_t err)
{
	unsigned k = t->param;
	a_to_pi_busy[k] = 0;
	if (!err)
	{
		st.audio_to_pi++;
		uint32_t us = cyc_us (ARM_DWT_CYCCNT - a_to_pi_t[k]);
		st.hist_to_pi[hist_bin (us)]++;
		if (us > st.worst_to_pi_us) st.worst_to_pi_us = us;
	}
	a_to_pi_pump ();
}

static void a_from_pc_done (ci_dev *, uint8_t, ci_td *t, uint32_t len, uint32_t err)
{
	unsigned k = t->param;
	if (!err)
	{
		gap_note (&pc_out_last, &pc_out_seen, st.pc_out_gap, &st.pc_out_gap_max_us);
		if (aq_pi_head - aq_pi_tail >= kAQ) { aq_pi_tail++; st.audio_to_pi_drop++; }
		Block &b = aq_pi[aq_pi_head++ % kAQ];
		memcpy (b.d, a_from_pc_buf[k], len);
		b.len = (uint16_t) len;
		b.t = ARM_DWT_CYCCNT;
		unsigned q = aq_pi_head - aq_pi_tail;
		if (q > st.q_to_pi_max) st.q_to_pi_max = q;
		a_to_pi_pump ();
	}
	if (P.ep[kAudioOutEp & 7][0].open) ci_queue (&P, kAudioOutEp, t, a_from_pc_buf[k], a_out_mps);
}

// ---------------------------------------------------------------- channels

static int chan_of_td (ChanBuf (*tab)[kChanDepth], ci_td *t, unsigned *pk)
{
	for (unsigned c = 0; c < kChannels; c++)
		for (unsigned k = 0; k < kChanDepth; k++)
			if (&tab[c][k].tl == t || &tab[c][k].tp == t) { *pk = k; return (int) c; }
	return -1;
}

static uint8_t mirrored_in_ep (unsigned c)  { return map.ep_of_in[c] >= 0 ? map.l.ep[map.ep_of_in[c]].addr : 0; }
static uint8_t mirrored_out_ep (unsigned c) { return map.ep_of_out[c] >= 0 ? map.l.ep[map.ep_of_out[c]].addr : 0; }

// box -> computer: the link's OUT channel gave a packet; it goes to the mirrored IN endpoint as it is
static void ch_link_out_done (ci_dev *, uint8_t, ci_td *t, uint32_t len, uint32_t err)
{
	unsigned k;
	int c = chan_of_td (ch_to_pc, t, &k);
	if (c < 0) return;
	ChanBuf &b = ch_to_pc[c][k];
	uint8_t ep = mirrored_in_ep ((unsigned) c);
	if (err || ep == 0 || !P.ep[ep & 7][1].open)
	{
		b.state = kFree;
		if (!err && ep) ev_post (kEvInTaken, ep, 1);	// the computer is not there: drained, as the DWC2 mirror parks
		b.state = kOnLink;
		ci_queue (&L, chan_out_ep ((unsigned) c), &b.tl, b.d, kLinkMps);
		return;
	}
	b.state = kOnPc;
	b.len = (uint16_t) len;
	ci_queue (&P, ep, &b.tp, b.d, len);
}

static void ch_pc_in_done (ci_dev *, uint8_t addr, ci_td *t, uint32_t, uint32_t err)
{
	unsigned k;
	int c = chan_of_td (ch_to_pc, t, &k);
	if (c < 0) return;
	ChanBuf &b = ch_to_pc[c][k];
	if (!err) { st.chan_to_pc[c]++; ev_post (kEvInTaken, addr, 1); }
	b.state = kOnLink;
	ci_queue (&L, chan_out_ep ((unsigned) c), &b.tl, b.d, kLinkMps);
}

// computer -> box: the mirrored OUT endpoint is primed only with a credit from the Pi
static void ch_pc_out_prime (unsigned c)
{
	uint8_t ep = mirrored_out_ep (c);
	if (ep == 0 || !P.ep[ep & 7][0].open) return;
	for (unsigned k = 0; k < kChanDepth && credits[c] > 0; k++)
	{
		ChanBuf &b = ch_to_pi[c][k];
		if (b.state != kFree) continue;
		credits[c]--;
		b.state = kOnPc;
		ci_queue (&P, ep, &b.tp, b.d, map.l.ep[map.ep_of_out[c]].mps);
	}
}

static void ch_pc_out_done (ci_dev *, uint8_t, ci_td *t, uint32_t len, uint32_t err)
{
	unsigned k;
	int c = chan_of_td (ch_to_pi, t, &k);
	if (c < 0) return;
	ChanBuf &b = ch_to_pi[c][k];
	if (err || !L.ep[chan_in_ep ((unsigned) c) & 7][1].open) { b.state = kFree; credits[c]++; ch_pc_out_prime ((unsigned) c); return; }
	b.state = kOnLink;
	b.len = (uint16_t) len;
	ci_queue (&L, chan_in_ep ((unsigned) c), &b.tl, b.d, len);
}

static void ch_link_in_done (ci_dev *, uint8_t, ci_td *t, uint32_t, uint32_t err)
{
	unsigned k;
	int c = chan_of_td (ch_to_pi, t, &k);
	if (c < 0) return;
	if (!err) st.chan_to_pi[c]++;
	ch_to_pi[c][k].state = kFree;
	ch_pc_out_prime ((unsigned) c);
}

// ---------------------------------------------------------------- the computer's side (USB2)

static void pc_eps_close (void)
{
	ci_ep_close_all (&P);
	memset (a_to_pc_busy, 0, sizeof a_to_pc_busy);
	aq_pc_head = aq_pc_tail = 0;
	pc_in_idle = true;
	for (unsigned c = 0; c < kChannels; c++)
	{
		credits[c] = 0;
		for (unsigned k = 0; k < kChanDepth; k++)
		{
			ChanBuf &bo = ch_to_pi[c][k];
			if (bo.state == kOnPc) bo.state = kFree;	// its dTD went with the flush
			ChanBuf &bi = ch_to_pc[c][k];
			if (bi.state == kOnPc && L.ep[chan_out_ep (c)][0].open)	// lost with the computer: back to the link
			{
				bi.state = kOnLink;
				ci_queue (&L, chan_out_ep (c), &bi.tl, bi.d, kLinkMps);
			}
		}
	}
}

static void pc_eps_open (void)
{
	for (unsigned i = 0; i < map.l.n; i++)
	{
		const mirror::Ep &e = map.l.ep[i];
		uint8_t type = e.attr & 3;
		if (e.role == mirror::kAudioIn) ci_ep_open (&P, e.addr, type, e.mps, a_to_pc_done);
		else if (e.role == mirror::kAudioOut)
		{
			ci_ep_open (&P, e.addr, type, e.mps, a_from_pc_done);
			a_out_mps = e.mps;
			for (unsigned k = 0; k < kALinkTds; k++)
			{
				a_from_pc_td[k].param = k;
				ci_queue (&P, e.addr, &a_from_pc_td[k], a_from_pc_buf[k], a_out_mps);
			}
		}
		else if (e.addr & 0x80) ci_ep_open (&P, e.addr, type, e.mps, ch_pc_in_done);
		else ci_ep_open (&P, e.addr, type, e.mps, ch_pc_out_done);
	}
}

static bool p_desc (uint16_t wValue, const uint8_t **p, unsigned *n)
{
	uint8_t i = wValue & 0xFF;
	switch (wValue >> 8)
	{
	case 1: if (i) return false; *p = dev_desc, *n = 18; return true;
	case 2: if (i) return false; *p = cfg_desc, *n = cfg_len; return true;
	case 3:
		for (unsigned k = 0; k < n_strs; k++)
			if (strs[k].index == i) { *p = strs[k].data, *n = strs[k].len; return true; }
		if (i == 0) { static const uint8_t Lang[4] = {4, 3, 0x09, 0x04}; *p = Lang, *n = 4; return true; }
		return false;
	case 15: if (!bos_len) return false; *p = bos_desc, *n = bos_len; return true;
	default: return false;	// no qualifier, no other-speed: as the DWC2 mirror (sidecar_gadget.cpp GetDescriptor)
	}
}

static void p_send (const void *data, unsigned n, uint16_t wLength)
{
	if (n > wLength) n = wLength;
	if (n > sizeof p_ep0) n = sizeof p_ep0;
	memcpy (p_ep0, data, n);
	ci_ep0_reply (&P, p_ep0, n, wLength);
}

static void p_ctl_out_arrived (ci_dev *, uint32_t len)
{
	p_ctl_data_len = (uint16_t) len;
	memcpy (p_ctl_data, p_ep0, len);
	st.ctl_forwarded++;
	ev_post (kEvSetup, ctl.seq, 0, ctl.setup);
}

static void p_setup (ci_dev *d, const uint8_t *s)
{
	uint8_t type = s[0], req = s[1];
	uint16_t wValue = s[2] | s[3] << 8, wIndex = s[4] | s[5] << 8, wLength = s[6] | s[7] << 8;
	st.setups++;
	if (ctl.abort ()) { st.ctl_aborted++; ev_post (kEvAbort, ctl.seq); }	// the computer moved on
	if ((type & 0x60) != 0)			// class or vendor
	{
		if (msos_len && type == 0xC0 && req == msos_code && wIndex == 7) { p_send (msos_set, msos_len, wLength); return; }
		if (wLength > sizeof p_ctl_data) { ci_ep0_stall (d); return; }
		ctl.begin (s);
		p_ctl_data_len = 0;
		if (!(type & 0x80) && wLength) { ci_ep0_recv (d, p_ep0, wLength, p_ctl_out_arrived); return; }
		st.ctl_forwarded++;
		ev_post (kEvSetup, ctl.seq, 0, s);	// EP0 NAKs (nothing armed) until the Pi answers
		return;
	}
	const uint8_t *p;
	unsigned n;
	switch ((uint16_t) (req << 8 | type))
	{
	case 0x0500:
		ci_ep0_status_in (d);
		ci_set_address (d, (uint8_t) wValue);
		return;
	case 0x0680: case 0x0681:
		if (!p_desc (wValue, &p, &n)) break;
		p_send (p, n, wLength);
		return;
	case 0x0900:
		pc_eps_close ();
		pc_config = (uint8_t) wValue;
		memset (pc_alt, 0, sizeof pc_alt);
		if (pc_config) pc_eps_open ();
		st.pc_configured = pc_config ? 1 : 0;
		ci_ep0_status_in (d);
		ev_post (kEvConfig, pc_config);
		return;
	case 0x0880: p_ep0[0] = pc_config; ci_ep0_reply (d, p_ep0, 1, wLength); return;
	case 0x0080: p_ep0[0] = (cfg_len > 7 && (cfg_desc[7] & 0x40)) ? 1 : 0; p_ep0[1] = 0; ci_ep0_reply (d, p_ep0, 2, wLength); return;
	case 0x0081: p_ep0[0] = 0; p_ep0[1] = 0; ci_ep0_reply (d, p_ep0, 2, wLength); return;
	case 0x0082: p_ep0[0] = ci_stalled (d, (uint8_t) wIndex) ? 1 : 0; p_ep0[1] = 0; ci_ep0_reply (d, p_ep0, 2, wLength); return;
	case 0x0102: if (wIndex & 0x0F) ci_stall (d, (uint8_t) wIndex, false); ci_ep0_status_in (d); return;	// also resets the toggle
	case 0x0302: if (wIndex & 0x0F) ci_stall (d, (uint8_t) wIndex, true); ci_ep0_status_in (d); return;
	case 0x0100: case 0x0300: ci_ep0_status_in (d); return;	// remote wakeup / test mode: taken, nothing to do
	case 0x0B01:
		// the computer's alternate, kept here as the DWC2 mirror keeps it; the box stays at the Pi's alt 3
		if (wIndex >= mirror::kMaxIfaces || !mirror::has_alt (cfg_desc, cfg_len, (uint8_t) wIndex, (uint8_t) wValue)) break;
		pc_alt[wIndex] = (uint8_t) wValue;
		for (unsigned i = 0; i < map.l.n; i++) if (map.l.ep[i].iface == wIndex) ci_reset_toggle (d, map.l.ep[i].addr);
		ci_ep0_status_in (d);
		ev_post (kEvAlt, (uint8_t) wIndex, (uint8_t) wValue);
		return;
	case 0x0A81:
		if (wIndex >= mirror::kMaxIfaces) break;
		p_ep0[0] = pc_alt[wIndex];
		ci_ep0_reply (d, p_ep0, 1, wLength);
		return;
	}
	ci_ep0_stall (d);
}

static void p_reset (ci_dev *)
{
	pc_eps_close ();
	pc_config = 0;
	st.pc_configured = 0;
	st.pc_resets++;
	if (ctl.abort ()) { st.ctl_aborted++; ev_post (kEvAbort, ctl.seq); }
	ev_post (kEvBus, kBusReset, ci_high_speed (&P));
}

static void p_port (ci_dev *) { st.pc_hs = ci_high_speed (&P); }
static void p_suspend (ci_dev *) { st.pc_suspends++; ev_post (kEvBus, kBusSuspend, ci_high_speed (&P)); }

static void pc_show (bool on)
{
	if (!on)
	{
		connect_due = false;
		if (shown) pc_off_at = millis () | 1;
		ci_run (&P, false);
		pc_eps_close ();
		pc_config = 0;
		st.pc_configured = 0;
		if (ctl.abort ()) { st.ctl_aborted++; ev_post (kEvAbort, ctl.seq); }
		shown = false;
		st.shown = 0;
		return;
	}
	shown = true;
	st.shown = 1;
	ci_run (&P, true);
}

static void forget_box (void)
{
	pc_show (false);
	have_dev = false;
	cfg_len = 0;
	n_strs = 0;
	bos_len = msos_len = 0;
	memset (&map, 0, sizeof map);
}

// ---------------------------------------------------------------- the link's side (USB1)

static const uint8_t l_dev_desc[18] = {
	18, 1, 0x10, 0x02, 0x00, 0x00, 0x00, 64,
	kVid & 0xFF, kVid >> 8, kPid & 0xFF, kPid >> 8,
	kVersion & 0xFF, kVersion >> 8, 1, 2, 3, 1
};

static unsigned l_config (uint8_t *b, bool hs, uint8_t type)
{
	unsigned n = 0;
	auto put = [&] (std::initializer_list<int> x) { for (int v : x) b[n++] = (uint8_t) v; };
	unsigned big = hs ? kLinkMps : 64;
	put ({9, type, 0, 0, 1, 1, 0, 0x80, 50});
	put ({9, 4, 0, 0, 0, kIfaceClass, kIfaceSub, kIfaceProto, 0});
	put ({7, 5, kEvEp, 2, (int) (big & 0xFF), (int) (big >> 8), 0});	// bulk since 1.02 (T411: the 64 B iv1 event pipe went unserved beside the Rytm)
	put ({7, 5, kAudioToPiEp, 2, (int) (big & 0xFF), (int) (big >> 8), 0});	// bulk since 1.01 (T406/T407: the 512 B interrupt pair was never polled beside the Rytm)
	put ({7, 5, kAudioFromPiEp, 2, (int) (big & 0xFF), (int) (big >> 8), 0});
	for (unsigned c = 0; c < kChannels; c++)
	{
		put ({7, 5, chan_in_ep (c), 2, (int) (big & 0xFF), (int) (big >> 8), 0});
		put ({7, 5, chan_out_ep (c), 2, (int) (big & 0xFF), (int) (big >> 8), 0});
	}
	b[13] = (uint8_t) (3 + 2 * kChannels);		// bNumEndpoints
	b[2] = (uint8_t) n, b[3] = (uint8_t) (n >> 8);
	return n;
}

// BOS + MS OS 2.0 so that a computer (the bench) binds WinUSB to the link without an INF; the Pi ignores both
static const uint8_t kLinkMsosCode = 0x21;
static uint8_t l_msos[170];
static unsigned l_msos_len;
static uint8_t l_bos[33];

static unsigned put_utf16 (uint8_t *p, const char *s, int nuls)
{
	unsigned n = 0;
	while (*s) { p[n++] = (uint8_t) *s++; p[n++] = 0; }
	for (int i = 0; i < nuls; i++) { p[n++] = 0; p[n++] = 0; }
	return n;
}

static void l_build_msos (void)
{
	uint8_t *p = l_msos;
	unsigned n = 0;
	p[n++] = 10; p[n++] = 0; p[n++] = 0; p[n++] = 0;
	p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x03; p[n++] = 0x06;
	unsigned total_at = n; n += 2;
	p[n++] = 20; p[n++] = 0; p[n++] = 3; p[n++] = 0;
	memcpy (p + n, "WINUSB\0\0", 8); n += 8;
	memset (p + n, 0, 8); n += 8;
	unsigned start = n; n += 2;
	p[n++] = 4; p[n++] = 0; p[n++] = 7; p[n++] = 0;
	unsigned nl_at = n; n += 2;
	unsigned nl = put_utf16 (p + n, "DeviceInterfaceGUIDs", 1); n += nl;
	p[nl_at] = (uint8_t) nl; p[nl_at + 1] = 0;
	unsigned dl_at = n; n += 2;
	unsigned dl = put_utf16 (p + n, "{5B1F0C2E-7A43-4D9B-8E61-2C4A9D0F7B35}", 2); n += dl;
	p[dl_at] = (uint8_t) dl; p[dl_at + 1] = 0;
	p[start] = (uint8_t) (n - start); p[start + 1] = (uint8_t) ((n - start) >> 8);
	p[total_at] = (uint8_t) n; p[total_at + 1] = (uint8_t) (n >> 8);
	l_msos_len = n;
	const uint8_t b[33] = {5, 0x0F, 33, 0, 1, 28, 0x10, 5, 0,
		0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
		0x00, 0x00, 0x03, 0x06, (uint8_t) n, (uint8_t) (n >> 8), kLinkMsosCode, 0};
	memcpy (l_bos, b, sizeof b);
}

static unsigned l_string (uint8_t *b, uint8_t i)
{
	if (i == 0) { b[0] = 4; b[1] = 3; b[2] = 0x09; b[3] = 0x04; return 4; }
	const char *s = i == 1 ? "Sidecar" : i == 2 ? "Sidecar mirror relay" : i == 3 ? "SMR1" : 0;
	if (!s) return 0;
	unsigned n = put_utf16 (b + 2, s, 0) + 2;
	b[0] = (uint8_t) n; b[1] = 3;
	return n;
}

static void l_eps_open (void)
{
	ci_ep_open (&L, kEvEp, 2, kLinkMps, ev_done);		// bulk (1.02); an event is one 16 B short packet
	memset (ev_busy, 0, sizeof ev_busy);
	ci_ep_open (&L, kAudioToPiEp, 2, kLinkMps, a_to_pi_done);
	memset (a_to_pi_busy, 0, sizeof a_to_pi_busy);
	aq_pi_head = aq_pi_tail = 0;
	ci_ep_open (&L, kAudioFromPiEp, 2, kLinkMps, a_from_pi_done);
	for (unsigned k = 0; k < kALinkTds; k++)
	{
		a_from_pi_td[k].param = k;
		ci_queue (&L, kAudioFromPiEp, &a_from_pi_td[k], a_from_pi_buf[k], kLinkMps);
	}
	for (unsigned c = 0; c < kChannels; c++)
	{
		ci_ep_open (&L, chan_in_ep (c), 2, kLinkMps, ch_link_in_done);
		ci_ep_open (&L, chan_out_ep (c), 2, kLinkMps, ch_link_out_done);
		for (unsigned k = 0; k < kChanDepth; k++)
		{
			ch_to_pi[c][k].state = kFree;
			ChanBuf &b = ch_to_pc[c][k];
			b.state = kOnLink;
			ci_queue (&L, chan_out_ep (c), &b.tl, b.d, kLinkMps);
		}
	}
}

static uint16_t l_wvalue, l_windex;

static void l_vendor_out_arrived (ci_dev *d, uint32_t len)
{
	uint8_t req = l_setup[1];
	bool ok = true;
	switch (req)
	{
	case kReqSetDevice:
		if (len != 18 || l_ep0[0] != 18 || l_ep0[1] != 1 || l_ep0[7] != 64) { ok = false; break; }
		if (shown) pc_show (false);
		memcpy (dev_desc, l_ep0, 18);
		have_dev = true;
		break;
	case kReqSetConfig:
		if (len < 9 || len > sizeof cfg_desc) { ok = false; break; }
		if (shown) pc_show (false);
		memcpy (cfg_desc, l_ep0, len);
		cfg_len = len;
		n_strs = 0;
		break;
	case kReqSetString:
	{
		if (len < 2 || len > sizeof strs[0].data || l_ep0[1] != 3) { ok = false; break; }
		unsigned k = 0;
		while (k < n_strs && strs[k].index != (uint8_t) l_wvalue) k++;
		if (k == n_strs) { if (n_strs == mirror::kMaxStrings) { ok = false; break; } n_strs++; }
		strs[k].index = (uint8_t) l_wvalue, strs[k].len = (uint8_t) len;
		memcpy (strs[k].data, l_ep0, len);
		break;
	}
	case kReqSetExtra:
		if (l_wvalue == 15 && len <= sizeof bos_desc) { memcpy (bos_desc, l_ep0, len); bos_len = len; }
		else if (l_wvalue == 0xEE && len <= sizeof msos_set) { memcpy (msos_set, l_ep0, len); msos_len = len; msos_code = (uint8_t) l_windex; }
		else ok = false;
		break;
	case kReqCtlAnswer:
		if (!ctl.answer ((uint8_t) l_wvalue)) { st.ctl_late++; break; }	// for nobody: the computer moved on
		st.ctl_answered++;
		if (l_windex & kAnsStall) { st.ctl_stalled++; ci_ep0_stall (&P); break; }
		if (ctl.setup[0] & 0x80)
		{
			uint16_t wl = ctl.setup[6] | ctl.setup[7] << 8;
			p_send (l_ep0, len, wl);
		}
		else ci_ep0_status_in (&P);
		break;
	default:
		ok = false;
	}
	if (ok) ci_ep0_status_in (d);
	else ci_ep0_stall (d);
}

static void l_setup_fn (ci_dev *d, const uint8_t *s)
{
	memcpy (l_setup, s, 8);
	uint8_t type = s[0], req = s[1];
	uint16_t wValue = s[2] | s[3] << 8, wIndex = s[4] | s[5] << 8, wLength = s[6] | s[7] << 8;
	l_wvalue = wValue, l_windex = wIndex;
	unsigned n;
	if (type == 0x40)
	{
		switch (req)
		{
		case kReqSetDevice: case kReqSetConfig: case kReqSetString: case kReqSetExtra: case kReqCtlAnswer:
			if (wLength > sizeof l_ep0) break;
			if (wLength == 0) { l_vendor_out_arrived (d, 0); return; }
			ci_ep0_recv (d, l_ep0, wLength, l_vendor_out_arrived);
			return;
		case kReqConnect:
			if (wValue == 0) { pc_show (false); ci_ep0_status_in (d); return; }
			if (!have_dev || !cfg_len || !make_map (cfg_desc, cfg_len, kAudioInEp, kAudioOutEp, &map))
			{
				rlog ("connect refused: dev %d cfg %u map no", have_dev, cfg_len);
				break;
			}
			if (shown) pc_show (false);
			rlog ("connect: %04x:%04x, %u B cfg, %u eps, chans in %u out %u", dev_desc[8] | dev_desc[9] << 8,
				dev_desc[10] | dev_desc[11] << 8, cfg_len, map.l.n, map.n_in, map.n_out);
			ci_ep0_status_in (d);
			connect_due = true;		// relay_poll: on the bus 300 ms after it left (a replug the computer sees, as the gadget's)
			return;
		case kReqCredit:
			for (unsigned c = 0; c < kChannels; c++)
				if (mirrored_out_ep (c) == (uint8_t) wValue)
				{
					unsigned v = credits[c] + wIndex;
					credits[c] = (uint8_t) (v > kMaxCredits ? kMaxCredits : v);
					ch_pc_out_prime (c);
					ci_ep0_status_in (d);
					return;
				}
			break;
		case kReqStatsReset:
		{
			uint32_t keep[10] = {st.link_hs, st.link_configured, st.link_resets, st.pc_hs, st.pc_configured, st.pc_resets,
				st.pc_suspends, st.shown, 0, 0};
			memset ((void *) &st, 0, sizeof st);
			st.link_hs = keep[0], st.link_configured = keep[1], st.link_resets = keep[2], st.pc_hs = keep[3];
			st.pc_configured = keep[4], st.pc_resets = keep[5], st.pc_suspends = keep[6], st.shown = keep[7];
			ci_ep0_status_in (d);
			return;
		}
		case kReqBootloader:
			if (wValue != 0x4C42) break;		// "BL": not by accident
			ci_ep0_status_in (d);
			reboot_at = millis () + 50;	// from loop(): a bkpt inside an interrupt is a fault, not the bootloader
			return;
		case kReqHalt:
			if ((wValue & 0x0F) == 0) break;
			ci_stall (&P, (uint8_t) wValue, wIndex != 0);
			ci_ep0_status_in (d);
			return;
		}
		ci_ep0_stall (d);
		return;
	}
	if (type == 0xC0)
	{
		switch (req)
		{
		case kReqCtlData:
			if ((uint8_t) wValue != ctl.seq || !ctl.pending) break;
			n = p_ctl_data_len < wLength ? p_ctl_data_len : wLength;
			memcpy (l_ep0, p_ctl_data, n);
			ci_ep0_reply (d, l_ep0, n, wLength);
			return;
		case kReqStats:
		{
			Stats s;
			memcpy (&s, (const void *) &st, sizeof s);
			s.magic = kStatsMagic, s.version = kVersion, s.uptime_ms = millis ();
			s.isr1 = L.isr_count, s.isr2 = P.isr_count;
			n = sizeof s < wLength ? sizeof s : wLength;
			memcpy (l_ep0, &s, n);
			ci_ep0_reply (d, l_ep0, n, wLength);
			return;
		}
		case kReqLog:
			n = 0;
			while (log_tail != log_head && n < wLength && n < sizeof l_ep0) l_ep0[n++] = (uint8_t) log_ring[log_tail++ % sizeof log_ring];
			ci_ep0_reply (d, l_ep0, n, wLength);
			return;
		case kLinkMsosCode:
			if (wIndex != 7) break;
			n = l_msos_len < wLength ? l_msos_len : wLength;
			memcpy (l_ep0, l_msos, n);
			ci_ep0_reply (d, l_ep0, n, wLength);
			return;
		}
		ci_ep0_stall (d);
		return;
	}
	// standard requests
	switch ((uint16_t) (req << 8 | type))
	{
	case 0x0500: ci_ep0_status_in (d); ci_set_address (d, (uint8_t) wValue); return;
	case 0x0680:
		n = 0;
		switch (wValue >> 8)
		{
		case 1: memcpy (l_ep0, l_dev_desc, 18); n = 18; break;
		case 2: n = l_config (l_ep0, ci_high_speed (d), 2); break;
		case 7: n = l_config (l_ep0, !ci_high_speed (d), 7); break;
		case 6: { const uint8_t q[10] = {10, 6, 0x10, 0x02, 0, 0, 0, 64, 1, 0}; memcpy (l_ep0, q, 10); n = 10; break; }
		case 3: n = l_string (l_ep0, wValue & 0xFF); break;
		case 15: memcpy (l_ep0, l_bos, sizeof l_bos); n = sizeof l_bos; break;
		}
		if (!n) break;
		ci_ep0_reply (d, l_ep0, n, wLength);
		return;
	case 0x0900:
		ci_ep_close_all (d);
		st.link_configured = wValue == 1;
		if (wValue == 1) l_eps_open ();
		else forget_box ();
		ci_ep0_status_in (d);
		if (wValue == 1) ev_post (kEvHello, kVersion >> 8, kVersion & 0xFF);
		return;
	case 0x0880: l_ep0[0] = (uint8_t) st.link_configured; ci_ep0_reply (d, l_ep0, 1, wLength); return;
	case 0x0080: case 0x0081: l_ep0[0] = 0; l_ep0[1] = 0; ci_ep0_reply (d, l_ep0, 2, wLength); return;
	case 0x0082: l_ep0[0] = ci_stalled (d, (uint8_t) wIndex) ? 1 : 0; l_ep0[1] = 0; ci_ep0_reply (d, l_ep0, 2, wLength); return;
	case 0x0102: if (wIndex & 0x0F) ci_stall (d, (uint8_t) wIndex, false); ci_ep0_status_in (d); return;
	case 0x0302: if (wIndex & 0x0F) ci_stall (d, (uint8_t) wIndex, true); ci_ep0_status_in (d); return;
	case 0x0B01: if (wValue == 0) { ci_ep0_status_in (d); return; } break;
	case 0x0A81: l_ep0[0] = 0; ci_ep0_reply (d, l_ep0, 1, wLength); return;
	}
	ci_ep0_stall (d);
}

static void l_reset (ci_dev *)
{
	st.link_resets++;
	st.link_configured = 0;
	forget_box ();		// the Pi went away or restarts: the computer must not see a box nobody serves
	ev_head = ev_tail = 0;
}

static void l_port (ci_dev *) { st.link_hs = ci_high_speed (&L); }

static void isr1 (void)
{
	uint32_t c = ARM_DWT_CYCCNT;
	ci_isr (&L);
	uint32_t us = cyc_us (ARM_DWT_CYCCNT - c);
	if (us > st.isr1_max_us) st.isr1_max_us = us;
}

static void isr2 (void)
{
	uint32_t c = ARM_DWT_CYCCNT;
	ci_isr (&P);
	uint32_t us = cyc_us (ARM_DWT_CYCCNT - c);
	if (us > st.isr2_max_us) st.isr2_max_us = us;
}

// ---------------------------------------------------------------- bring-up

extern "C" volatile uint8_t usb_configuration;	// the core's USB1 stack (usb.c): told it is gone

static bool usb2_phy_up (void)
{
	// USB2 PHY PLL (PLL7) to 480 MHz, as USBHost_t36 does (bounded); EMC_40 (the header's 5 V switch) stays low
	uint32_t t0 = millis ();
	while (1)
	{
		if (millis () - t0 > 500) return false;
		uint32_t n = CCM_ANALOG_PLL_USB2;
		if (n & CCM_ANALOG_PLL_USB2_DIV_SELECT)
		{
			CCM_ANALOG_PLL_USB2_CLR = 0xC000;
			CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_BYPASS;
			CCM_ANALOG_PLL_USB2_CLR = CCM_ANALOG_PLL_USB2_POWER | CCM_ANALOG_PLL_USB2_DIV_SELECT
				| CCM_ANALOG_PLL_USB2_ENABLE | CCM_ANALOG_PLL_USB2_EN_USB_CLKS;
			continue;
		}
		if (!(n & CCM_ANALOG_PLL_USB2_ENABLE)) { CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_ENABLE; continue; }
		if (!(n & CCM_ANALOG_PLL_USB2_POWER)) { CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_POWER; continue; }
		if (!(n & CCM_ANALOG_PLL_USB2_LOCK)) continue;
		if (n & CCM_ANALOG_PLL_USB2_BYPASS) { CCM_ANALOG_PLL_USB2_CLR = CCM_ANALOG_PLL_USB2_BYPASS; continue; }
		if (!(n & CCM_ANALOG_PLL_USB2_EN_USB_CLKS)) { CCM_ANALOG_PLL_USB2_SET = CCM_ANALOG_PLL_USB2_EN_USB_CLKS; continue; }
		break;
	}
	CCM_CCGR6 |= CCM_CCGR6_USBOH3 (CCM_CCGR_ON);
	*(volatile uint32_t *) 0x400D8214 = (1u << 20) | (1u << 19);	// USB_ANALOG_USB2_CHRG_DETECT_SET: EN_B, CHK_CHRG_B
	USBPHY2_CTRL_CLR = USBPHY_CTRL_SFTRST | USBPHY_CTRL_CLKGATE;
	USBPHY2_CTRL_SET = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;
	USBPHY2_PWD = 0;
	delay (10);
	return true;
}

void relay_begin (void)
{
	l_build_msos ();
	// USB1: the core enumerated it as its own device at startup; take it over (the Pi sees an unplug, then the link)
	relay_stage = 1;
	NVIC_DISABLE_IRQ (IRQ_USB1);
	USB1_USBCMD &= ~USB_USBCMD_RS;
	usb_configuration = 0;
	delay (50);
	ci_init (&L, IMXRT_USB1_ADDRESS, qh1);
	L.on_setup = l_setup_fn, L.on_reset = l_reset, L.on_port = l_port;
	NVIC_CLEAR_PENDING (IRQ_USB1);
	attachInterruptVector (IRQ_USB1, isr1);
	NVIC_SET_PRIORITY (IRQ_USB1, 96);
	// USB2: our own PHY bring-up, off the bus until the Pi uploads a box and says kReqConnect
	relay_stage = 2;
	if (!usb2_phy_up ()) { relay_stage = 0x82; rlog ("usb2: PLL7 no lock"); }
	ci_init (&P, IMXRT_USB2_ADDRESS, qh2);
	P.on_setup = p_setup, P.on_reset = p_reset, P.on_port = p_port, P.on_suspend = p_suspend, P.on_sof = p_sof;
	if (kPace) ci_sof_irq (&P, true);
	NVIC_CLEAR_PENDING (IRQ_USB2);
	attachInterruptVector (IRQ_USB2, isr2);
	NVIC_SET_PRIORITY (IRQ_USB2, 96);
	NVIC_ENABLE_IRQ (IRQ_USB2);
	NVIC_ENABLE_IRQ (IRQ_USB1);
	ci_run (&L, true);
	relay_stage = 5;
#ifndef RELAY_BUILD
#define RELAY_BUILD "unknown"
#endif
	rlog ("mirror relay up, protocol %u.%02u, build %s", kVersion >> 8, kVersion & 0xFF, RELAY_BUILD);
}

void relay_poll (void)
{
	if (pc_in_idle && P.ep[kAudioInEp & 7][1].open && ci_nak_take (&P, kAudioInEp))	// the computer polls again
	{
		__disable_irq ();
		pc_in_idle = false;
		pc_in_last_take = ARM_DWT_CYCCNT;
		st.pc_idle_leave++;
		pace_acc = 65536, pace_lvl = kPaceTarget * 256;	// the next SOF hands out the newest block
		a_to_pc_pump ();
		__enable_irq ();
	}
	if (L.ep[kAudioFromPiEp & 7][0].open && ci_nak_take (&L, kAudioFromPiEp)) st.link_nak_polls++;
	if (connect_due && (pc_off_at == 0 || millis () - pc_off_at >= 300))
	{
		__disable_irq ();
		if (connect_due)
		{
			connect_due = false;
			pc_show (true);
			ev_post (kEvHello, kVersion >> 8, kVersion & 0xFF);
		}
		__enable_irq ();
	}
	if (reboot_at && (int32_t) (millis () - reboot_at) >= 0)
	{
		ci_run (&P, false);
		ci_run (&L, false);
		delay (20);
		_reboot_Teensyduino_ ();
	}
}

bool relay_link_up (void) { return st.link_configured != 0; }
bool relay_shown (void) { return shown && pc_config != 0; }
