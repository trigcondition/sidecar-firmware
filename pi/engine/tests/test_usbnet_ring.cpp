// pi/engine/tests/test_usbnet_ring.cpp - usbnet_ring.h: the USB-C network's frame queues: order, full and empty,
// lengths kept, the index wrap, drain, and one producer and one consumer thread at once.
#include <cstdio>
#include <cstring>
#include <thread>

#include "check.h"
#include "usbnet_ring.h"

int main ()
{
	static FrameRing<4, 1514> r;
	const uint8_t *p;
	unsigned n;
	CHECK (!r.peek (&p, &n) && r.used () == 0 && r.free () == 4);
	uint8_t f[1514];
	for (unsigned i = 0; i < sizeof f; i++) f[i] = (uint8_t) i;
	CHECK (r.push (f, 60) && r.push (f, 1514) && r.push (f, 14));
	CHECK (!r.push (f, 1515));				// too long
	uint8_t *q = r.claim ();
	CHECK (q != 0);
	memcpy (q, f + 1, 100);
	r.commit (100);
	CHECK (r.claim () == 0 && !r.push (f, 60) && r.free () == 0);	// full
	CHECK (r.peek (&p, &n) && n == 60 && !memcmp (p, f, 60));
	r.release ();
	CHECK (r.peek (&p, &n) && n == 1514 && !memcmp (p, f, 1514));
	r.release ();
	CHECK (r.peek (&p, &n) && n == 14);
	r.release ();
	CHECK (r.peek (&p, &n) && n == 100 && p[0] == 1);
	r.release ();
	CHECK (!r.peek (&p, &n));
	// many rounds through the wrap of the indices (2^32 is not reached; the masks are what matter)
	for (unsigned k = 0; k < 1000; k++)
	{
		f[0] = (uint8_t) k;
		CHECK (r.push (f, 20 + k % 100));
		CHECK (r.peek (&p, &n) && n == 20 + k % 100 && p[0] == (uint8_t) k);
		r.release ();
	}
	// drain: the consumer drops what is queued
	r.push (f, 60); r.push (f, 60);
	r.drain ();
	CHECK (r.used () == 0 && !r.peek (&p, &n));

	// two threads: every frame arrives once, in order, with its length
	static FrameRing<16, 64> t;
	const unsigned kN = 200000;
	std::thread prod ([&] {
		for (unsigned i = 0; i < kN; )
		{
			uint8_t *s = t.claim ();
			if (!s) continue;
			memcpy (s, &i, 4);
			t.commit (4 + i % 60);
			i++;
		}
	});
	unsigned got = 0;
	while (got < kN)
	{
		if (!t.peek (&p, &n)) continue;
		unsigned v;
		memcpy (&v, p, 4);
		CHECK (v == got && n == 4 + got % 60);
		t.release ();
		got++;
	}
	prod.join ();
	printf ("test_usbnet_ring: OK\n");
	return 0;
}
