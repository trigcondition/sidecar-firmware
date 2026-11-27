// teensy/mirror_relay/ci_dev.h - a small device-mode driver for the i.MX RT1062's ChipIdea USB controllers, one
// instance per port (USB1 = the micro-USB, USB2 = the 5-pin header). Grown from the bench driver of
// Projects\Sidecar\work\teensy-usb2-device (check 1: USB2 as a high-speed device, no loss at 368 B a microframe),
// itself modelled on the Teensy core's usb.c.
//
// Every queue head, dTD and buffer must sit in DTCM (plain globals): the USB DMA reaches it and it is not cached.
// Callbacks run in the port's interrupt; both ports' interrupts have the same priority, so they never preempt each other.
#pragma once
#include <stdint.h>

struct ci_td
{
	volatile uint32_t next;
	volatile uint32_t status;
	uint32_t p0, p1, p2, p3, p4;
	uint32_t param;			// the owner's: which buffer
	uint32_t len;			// the driver's: bytes asked (the controller reads only the first 28 bytes)
	void *buf;
} __attribute__ ((aligned (32)));

struct ci_qh
{
	uint32_t config;
	uint32_t current;
	volatile uint32_t next;
	volatile uint32_t status;
	uint32_t p0, p1, p2, p3, p4;
	uint32_t reserved;
	volatile uint32_t setup0;
	volatile uint32_t setup1;
	ci_td *first;			// the driver's own list of dTDs queued (not the controller's)
	ci_td *last;
	uint32_t queued;
	uint32_t unused;
};

struct ci_dev;
// a dTD finished: len = bytes moved, err = the dTD's error bits (0x68: halted, buffer, transaction)
typedef void (*ci_done_fn) (ci_dev *d, uint8_t addr, ci_td *t, uint32_t len, uint32_t err);

struct ci_ep
{
	ci_done_fn done;
	uint16_t mps;
	uint8_t type;			// 1 iso, 2 bulk, 3 interrupt
	uint8_t open;
};

struct ci_dev
{
	uintptr_t base;			// IMXRT_USB1_ADDRESS or IMXRT_USB2_ADDRESS
	ci_qh *qh;			// 16 queue heads, 4096-aligned, owned by the caller
	ci_ep ep[8][2];			// [number][0 = OUT, 1 = IN]
	ci_td ep0_data, ep0_ack;
	void (*on_setup) (ci_dev *d, const uint8_t *setup);
	void (*on_reset) (ci_dev *d);
	void (*on_port) (ci_dev *d);		// port change: speed known (ci_high_speed)
	void (*on_suspend) (ci_dev *d);
	void (*on_sof) (ci_dev *d);		// each (micro)frame start, when ci_sof_irq turned it on
	void (*ep0_rx) (ci_dev *d, uint32_t len);	// the OUT data stage armed by ci_ep0_recv arrived
	uint8_t *ep0_rx_buf;
	uint32_t ep0_rx_want;
	uint32_t isr_count, resets, suspends;
	uint32_t fr_last, fr_ext;		// FRINDEX extended (microframes)
};

void ci_init (ci_dev *d, uintptr_t base, ci_qh *qh);	// controller reset, device mode, queue heads; not on the bus yet
void ci_run (ci_dev *d, bool on);			// pull-up on (the host sees a device) or off
void ci_isr (ci_dev *d);
bool ci_high_speed (ci_dev *d);
uint32_t ci_uframes (ci_dev *d);

void ci_ep_open (ci_dev *d, uint8_t addr, uint8_t type, uint16_t mps, ci_done_fn done);
void ci_ep_close (ci_dev *d, uint8_t addr);		// flushed: its dTDs are dropped without callbacks
void ci_ep_close_all (ci_dev *d);			// every endpoint but 0
void ci_queue (ci_dev *d, uint8_t addr, ci_td *t, void *buf, uint32_t len);	// IN: len to send; OUT: buffer size
uint32_t ci_queued (ci_dev *d, uint8_t addr);
void ci_sof_irq (ci_dev *d, bool on);		// the SOF interrupt (8000 a second at high speed) on or off
void ci_ep_flush (ci_dev *d, uint8_t addr);	// drop every dTD primed on an open endpoint (no completions), keep it open
bool ci_nak_take (ci_dev *d, uint8_t addr);	// the endpoint NAKed since the last call (ENDPTNAK, cleared)
void ci_stall (ci_dev *d, uint8_t addr, bool on);	// on = FALSE also resets the data toggle
bool ci_stalled (ci_dev *d, uint8_t addr);
void ci_reset_toggle (ci_dev *d, uint8_t addr);

void ci_ep0_send (ci_dev *d, const void *data, uint32_t len);	// data stage IN (len 0: a zero-length packet) + status
void ci_ep0_reply (ci_dev *d, const void *data, uint32_t len, uint16_t wLength);	// an IN request's answer, cut to wLength
void ci_ep0_status_in (ci_dev *d);				// the status stage of a no-data or OUT request
void ci_ep0_recv (ci_dev *d, void *buf, uint32_t len, void (*cb) (ci_dev *, uint32_t));	// arm an OUT data stage
void ci_ep0_stall (ci_dev *d);
void ci_set_address (ci_dev *d, uint8_t addr);
