// teensy/mirror_relay/ci_dev.cpp - see ci_dev.h
#include "ci_dev.h"
#include "imxrt.h"
#include <string.h>

#define R(off) (*(volatile uint32_t *) (d->base + (off)))
#define USBCMD		R (0x140)
#define USBSTS		R (0x144)
#define USBINTR		R (0x148)
#define FRINDEX		R (0x14C)
#define DEVICEADDR	R (0x154)
#define EPLISTADDR	R (0x158)
#define BURSTSIZE	R (0x160)
#define PORTSC1		R (0x184)
#define USBMODE		R (0x1A8)
#define SETUPSTAT	R (0x1AC)
#define PRIME		R (0x1B0)
#define FLUSH		R (0x1B4)
#define EPSTATUS	R (0x1B8)
#define COMPLETE	R (0x1BC)
#define EPCTRL(n)	R (0x1C0 + 4 * (n))

// ENDPTCTRL bits
#define RXS	(1u << 0)
#define RXT(t)	((uint32_t) (t) << 2)
#define RXR	(1u << 6)
#define RXE	(1u << 7)
#define TXS	(1u << 16)
#define TXT(t)	((uint32_t) (t) << 18)
#define TXR	(1u << 22)
#define TXE	(1u << 23)

static inline uint32_t bit_of (uint8_t addr) { return (addr & 0x80) ? (1u << (16 + (addr & 7))) : (1u << (addr & 7)); }
static inline ci_qh *qh_of (ci_dev *d, uint8_t addr) { return &d->qh[(addr & 7) * 2 + ((addr & 0x80) ? 1 : 0)]; }

static void td_prep (ci_td *t, void *data, uint32_t len)
{
	uint32_t a = (uint32_t) data;
	t->next = 1;
	t->status = (len << 16) | (1u << 15) | (1u << 7);	// IOC, active
	t->p0 = a;
	t->p1 = (a & ~0xFFFu) + 0x1000;
	t->p2 = (a & ~0xFFFu) + 0x2000;
	t->p3 = (a & ~0xFFFu) + 0x3000;
	t->p4 = (a & ~0xFFFu) + 0x4000;
	t->len = len;
	t->buf = data;
}

// add a dTD to an endpoint (reference manual "Executing a transfer descriptor": the ATDTW tripwire), as the core's usb.c
static void schedule (ci_dev *d, ci_qh *q, uint32_t mask, ci_td *t)
{
	__disable_irq ();
	q->queued++;
	ci_td *last = q->last;
	if (last)
	{
		last->next = (uint32_t) t;
		if (PRIME & mask) goto end;
		uint32_t status, c = ARM_DWT_CYCCNT;
		do
		{
			USBCMD |= USB_USBCMD_ATDTW;
			status = EPSTATUS;
		} while (!(USBCMD & USB_USBCMD_ATDTW) && (ARM_DWT_CYCCNT - c < 2400));
		USBCMD &= ~USB_USBCMD_ATDTW;
		if (status & mask) goto end;
	}
	else
		q->first = t;
	q->next = (uint32_t) t;
	q->status = 0;
	PRIME |= mask;
end:
	q->last = t;
	__enable_irq ();
}

void ci_init (ci_dev *d, uintptr_t base, ci_qh *qh)
{
	ci_qh *keep = qh;
	memset (d, 0, sizeof *d);
	d->base = base;
	d->qh = keep;
	USBCMD &= ~USB_USBCMD_RS;
	USBCMD |= USB_USBCMD_RST;
	uint32_t c = ARM_DWT_CYCCNT;
	while ((USBCMD & USB_USBCMD_RST) && ARM_DWT_CYCCNT - c < 60000000u) ;	// 100 ms
	USBCMD = 0;					// ITC 0: interrupt at once (the reset leaves 8 microframes, 1 ms of batching)
	BURSTSIZE = 0x0404;
	USBMODE = USB_USBMODE_CM (2) | USB_USBMODE_SLOM;
	memset (d->qh, 0, sizeof (ci_qh) * 16);
	d->qh[0].config = (64u << 16) | (1u << 15);	// IOS: interrupt on setup
	d->qh[1].config = (64u << 16);
	for (int i = 2; i < 16; i++) d->qh[i].next = 1;
	EPLISTADDR = (uint32_t) d->qh;
	USBINTR = USB_USBINTR_UE | USB_USBINTR_UEE | USB_USBINTR_PCE | USB_USBINTR_URE | USB_USBINTR_SLE;
}

void ci_run (ci_dev *d, bool on)
{
	if (on) USBCMD |= USB_USBCMD_RS;
	else USBCMD &= ~USB_USBCMD_RS;
}

bool ci_high_speed (ci_dev *d) { return (PORTSC1 & USB_PORTSC1_HSP) != 0; }

uint32_t ci_uframes (ci_dev *d)
{
	uint32_t f = FRINDEX & 0x3FFF;
	d->fr_ext += (f - d->fr_last) & 0x3FFF;
	d->fr_last = f;
	return d->fr_ext;
}

void ci_ep_open (ci_dev *d, uint8_t addr, uint8_t type, uint16_t mps, ci_done_fn done)
{
	unsigned n = addr & 7, in = (addr & 0x80) ? 1 : 0;
	if (n == 0) return;
	ci_ep *e = &d->ep[n][in];
	ci_qh *q = qh_of (d, addr);
	memset (q, 0, sizeof *q);
	q->next = 1;
	q->config = ((uint32_t) mps << 16) | (1u << 29);	// ZLT off: a packet is a packet
	e->done = done, e->mps = mps, e->type = type & 3, e->open = 1;
	uint32_t c = EPCTRL (n);
	if (in) c = (c & 0xFFFFu) | TXE | TXR | TXT (type & 3);
	else c = (c & 0xFFFF0000u) | RXE | RXR | RXT (type & 3);
	// the other direction unused: not left as a control type (the controller's note on paired endpoints)
	if (in && !(c & RXE)) c = (c & ~RXT (3)) | RXT (2);
	if (!in && !(c & TXE)) c = (c & ~TXT (3)) | TXT (2);
	EPCTRL (n) = c;
}

void ci_ep_close (ci_dev *d, uint8_t addr)
{
	unsigned n = addr & 7, in = (addr & 0x80) ? 1 : 0;
	if (n == 0) return;
	uint32_t mask = bit_of (addr);
	uint32_t c = EPCTRL (n);
	c &= in ? ~(TXE | TXS) : ~(RXE | RXS);
	EPCTRL (n) = c;
	FLUSH = mask;
	uint32_t t = ARM_DWT_CYCCNT;
	while ((FLUSH & mask) && ARM_DWT_CYCCNT - t < 600000u) ;
	ci_qh *q = qh_of (d, addr);
	memset (q, 0, sizeof *q);
	q->next = 1;
	d->ep[n][in].open = 0;
}

void ci_ep_close_all (ci_dev *d)
{
	for (unsigned n = 1; n < 8; n++)
	{
		if (d->ep[n][0].open) ci_ep_close (d, (uint8_t) n);
		if (d->ep[n][1].open) ci_ep_close (d, (uint8_t) (0x80 | n));
	}
}

void ci_queue (ci_dev *d, uint8_t addr, ci_td *t, void *buf, uint32_t len)
{
	if (!d->ep[addr & 7][(addr & 0x80) ? 1 : 0].open) return;
	td_prep (t, buf, len);
	schedule (d, qh_of (d, addr), bit_of (addr), t);
}

uint32_t ci_queued (ci_dev *d, uint8_t addr) { return qh_of (d, addr)->queued; }

void ci_sof_irq (ci_dev *d, bool on)
{
	if (on) USBINTR |= USB_USBINTR_SRE;
	else USBINTR &= ~USB_USBINTR_SRE;
}

void ci_ep_flush (ci_dev *d, uint8_t addr)
{
	uint32_t mask = bit_of (addr);
	__disable_irq ();
	uint32_t t = ARM_DWT_CYCCNT;
	do
	{
		FLUSH = mask;
		while ((FLUSH & mask) && ARM_DWT_CYCCNT - t < 600000u) ;
	} while ((EPSTATUS & mask) && ARM_DWT_CYCCNT - t < 600000u);
	ci_qh *q = qh_of (d, addr);
	q->next = 1;
	q->status = 0;
	q->first = q->last = 0;
	q->queued = 0;
	COMPLETE = mask;
	__enable_irq ();
}

bool ci_nak_take (ci_dev *d, uint8_t addr)
{
	uint32_t m = bit_of (addr);
	bool nak = (R (0x178) & m) != 0;		// ENDPTNAK: RX bits 0-7, TX bits 16-23, write 1 to clear
	if (nak) R (0x178) = m;
	return nak;
}

void ci_stall (ci_dev *d, uint8_t addr, bool on)
{
	unsigned n = addr & 7;
	if (n == 0) { if (on) ci_ep0_stall (d); return; }
	uint32_t c = EPCTRL (n);
	if (addr & 0x80) c = on ? (c | TXS) : ((c & ~TXS) | TXR);
	else c = on ? (c | RXS) : ((c & ~RXS) | RXR);
	EPCTRL (n) = c;
}

bool ci_stalled (ci_dev *d, uint8_t addr)
{
	uint32_t c = EPCTRL (addr & 7);
	return (addr & 0x80) ? (c & TXS) != 0 : (c & RXS) != 0;
}

void ci_reset_toggle (ci_dev *d, uint8_t addr)
{
	unsigned n = addr & 7;
	if (n) EPCTRL (n) |= (addr & 0x80) ? TXR : RXR;
}

// ---- endpoint 0 ----

void ci_ep0_send (ci_dev *d, const void *data, uint32_t len)
{
	{					// a zero-length data stage too: the host asked for wLength > 0 bytes
		td_prep (&d->ep0_data, (void *) data, len);
		d->ep0_data.status &= ~(1u << 15);
		d->qh[1].next = (uint32_t) &d->ep0_data;
		d->qh[1].status = 0;
		PRIME |= (1u << 16);
		while (PRIME) ;
	}
	d->ep0_ack.next = 1;
	d->ep0_ack.status = (1u << 7);
	d->ep0_ack.p0 = 0;
	d->qh[0].next = (uint32_t) &d->ep0_ack;
	d->qh[0].status = 0;
	COMPLETE = (1u << 0) | (1u << 16);
	PRIME |= (1u << 0);
	while (PRIME) ;
}

void ci_ep0_reply (ci_dev *d, const void *data, uint32_t len, uint16_t wLength)
{
	if (wLength == 0) { ci_ep0_status_in (d); return; }	// no data stage: the status stage is IN
	ci_ep0_send (d, data, len < wLength ? len : wLength);
}

void ci_ep0_status_in (ci_dev *d)
{
	d->ep0_ack.next = 1;
	d->ep0_ack.status = (1u << 7);
	d->ep0_ack.p0 = 0;
	d->qh[1].next = (uint32_t) &d->ep0_ack;
	d->qh[1].status = 0;
	COMPLETE = (1u << 0) | (1u << 16);
	PRIME |= (1u << 16);
	while (PRIME) ;
}

void ci_ep0_recv (ci_dev *d, void *buf, uint32_t len, void (*cb) (ci_dev *, uint32_t))
{
	d->ep0_rx = cb;
	d->ep0_rx_buf = (uint8_t *) buf;
	d->ep0_rx_want = len;
	td_prep (&d->ep0_data, buf, len);			// IOC on: completion bit 0
	d->qh[0].next = (uint32_t) &d->ep0_data;
	d->qh[0].status = 0;
	COMPLETE = (1u << 0);
	PRIME |= (1u << 0);
	while (PRIME) ;
}

void ci_ep0_stall (ci_dev *d) { EPCTRL (0) = 0x00010001; }

void ci_set_address (ci_dev *d, uint8_t addr) { DEVICEADDR = USB_DEVICEADDR_USBADR (addr) | USB_DEVICEADDR_USBADRA; }

// ---- interrupt ----

static void run_done (ci_dev *d, uint8_t addr)
{
	ci_qh *q = qh_of (d, addr);
	ci_ep *e = &d->ep[addr & 7][(addr & 0x80) ? 1 : 0];
	ci_td *first = q->first;
	if (!first) return;
	uint32_t count = 0;
	ci_td *t = first;
	while (1)
	{
		if (t->status & (1u << 7)) { q->first = t; break; }
		count++;
		t = (ci_td *) t->next;
		if ((uint32_t) t == 1) { q->first = 0; q->last = 0; break; }
	}
	if (!count) return;
	q->queued -= count;
	while (count--)
	{
		ci_td *next = (ci_td *) first->next;
		uint32_t s = first->status;
		uint32_t moved = first->len - ((s >> 16) & 0x7FFF);
		if (e->done) e->done (d, addr, first, moved, s & 0x68);
		first = next;
	}
}

void ci_isr (ci_dev *d)
{
	uint32_t status = USBSTS;
	USBSTS = status;
	d->isr_count++;
	ci_uframes (d);
	if (status & USB_USBSTS_UI)
	{
		uint32_t setupstatus = SETUPSTAT;
		while (setupstatus)
		{
			SETUPSTAT = setupstatus;
			uint32_t w[2];
			do
			{
				USBCMD |= USB_USBCMD_SUTW;
				w[0] = d->qh[0].setup0;
				w[1] = d->qh[0].setup1;
			} while (!(USBCMD & USB_USBCMD_SUTW));
			USBCMD &= ~USB_USBCMD_SUTW;
			FLUSH = (1u << 16) | (1u << 0);
			while (FLUSH & ((1u << 16) | (1u << 0))) ;
			d->ep0_rx = 0;				// a data stage armed for the last request is gone
			if (d->on_setup) d->on_setup (d, (const uint8_t *) w);
			setupstatus = SETUPSTAT;
		}
		uint32_t complete = COMPLETE;
		if (complete)
		{
			COMPLETE = complete;
			if ((complete & 1u) && d->ep0_rx)
			{
				void (*cb) (ci_dev *, uint32_t) = d->ep0_rx;
				d->ep0_rx = 0;
				uint32_t moved = d->ep0_rx_want - ((d->ep0_data.status >> 16) & 0x7FFF);
				cb (d, moved);
			}
			for (unsigned n = 1; n < 8; n++)
			{
				if (complete & (1u << n)) run_done (d, (uint8_t) n);
				if (complete & (1u << (16 + n))) run_done (d, (uint8_t) (0x80 | n));
			}
		}
	}
	if ((status & USB_USBSTS_SRI) && d->on_sof) d->on_sof (d);
	if (status & USB_USBSTS_URI)
	{
		SETUPSTAT = SETUPSTAT;
		COMPLETE = COMPLETE;
		while (PRIME != 0) ;
		FLUSH = 0xFFFFFFFF;
		for (unsigned n = 1; n < 8; n++)
			for (unsigned k = 0; k < 2; k++)
				if (d->ep[n][k].open) ci_ep_close (d, (uint8_t) (n | (k ? 0x80 : 0)));
		DEVICEADDR = 0;
		d->ep0_rx = 0;
		d->resets++;
		if (d->on_reset) d->on_reset (d);
	}
	if (status & USB_USBSTS_PCI)
		if (d->on_port) d->on_port (d);
	if (status & USB_USBSTS_SLI)
	{
		d->suspends++;
		if (d->on_suspend) d->on_suspend (d);
	}
}
