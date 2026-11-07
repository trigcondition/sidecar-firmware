// pi/engine/usbnet_ring.h - the USB-C network's frame queues (docs/usb-network.md): N slots of up to S bytes, one
// producer and one consumer, filled and read in place (no copy through a temporary). On the Pi both ends run on core 0:
// RX - the gadget IRQ fills (an OUT block's datagrams), the network task (Circle's CNetDeviceLayer) reads;
// TX - the network task fills, the IN endpoint reads (its IRQ, or the task with IRQs masked). Acquire/release on the
// indices as spsc.h, so it is also right across cores. Portable (tests/test_usbnet_ring.cpp).
#ifndef _usbnet_ring_h
#define _usbnet_ring_h

#include <stdint.h>

template <unsigned N, unsigned S>
class FrameRing
{
	static_assert ((N & (N - 1)) == 0, "N must be a power of 2");

public:
	static constexpr unsigned kSlots = N, kSlotSize = S;

	// the producer: a free slot to fill (0: full), then commit with its length
	uint8_t *claim ()
	{
		uint32_t w = __atomic_load_n (&m_nWr, __ATOMIC_RELAXED), r = __atomic_load_n (&m_nRd, __ATOMIC_ACQUIRE);
		return w - r >= N ? 0 : m_Slot[w & (N - 1)].data;
	}
	void commit (unsigned nLen)
	{
		uint32_t w = __atomic_load_n (&m_nWr, __ATOMIC_RELAXED);
		m_Slot[w & (N - 1)].len = (uint16_t) (nLen > S ? S : nLen);
		__atomic_store_n (&m_nWr, w + 1, __ATOMIC_RELEASE);
	}
	bool push (const uint8_t *p, unsigned nLen)		// claim + copy + commit (false: full or too long)
	{
		if (nLen > S) return false;
		uint8_t *q = claim ();
		if (q == 0) return false;
		for (unsigned i = 0; i < nLen; i++) q[i] = p[i];
		commit (nLen);
		return true;
	}

	// the consumer: the oldest frame (false: none), then release it
	bool peek (const uint8_t **ppData, unsigned *pLen) const
	{
		uint32_t r = __atomic_load_n (&m_nRd, __ATOMIC_RELAXED), w = __atomic_load_n (&m_nWr, __ATOMIC_ACQUIRE);
		if (r == w) return false;
		*ppData = m_Slot[r & (N - 1)].data;
		*pLen = m_Slot[r & (N - 1)].len;
		return true;
	}
	void release ()
	{
		uint32_t r = __atomic_load_n (&m_nRd, __ATOMIC_RELAXED);
		__atomic_store_n (&m_nRd, r + 1, __ATOMIC_RELEASE);
	}

	unsigned used () const	{ return __atomic_load_n (&m_nWr, __ATOMIC_ACQUIRE) - __atomic_load_n (&m_nRd, __ATOMIC_ACQUIRE); }
	unsigned free () const	{ return N - used (); }
	// the consumer's side only: everything queued dropped (a link that went down)
	void drain ()		{ __atomic_store_n (&m_nRd, __atomic_load_n (&m_nWr, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); }

private:
	struct Slot { uint16_t len; uint8_t data[S]; };
	Slot m_Slot[N];
	alignas (64) uint32_t m_nWr = 0;
	alignas (64) uint32_t m_nRd = 0;
};

#endif
