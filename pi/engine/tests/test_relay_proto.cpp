// pi/engine/tests/test_relay_proto.cpp - relay_proto.h (usb=mirror through a Teensy, docs/teensy-mirror.md):
// make build/test_relay_proto
//
// The fixture is test_mirror_desc.cpp's made-up shape (pid.codes IDs, no box's bytes): the audio pair 0x83 / 0x03, a
// pipe pair 0x82 / 0x02, USB MIDI 0x81 / 0x01.
#include "relay_proto.h"
#include "ob_block.h"
#include "check.h"

#include <cstdio>
#include <vector>

static std::vector<uint8_t> Cfg;
static void D (std::initializer_list<int> b) { for (int x : b) Cfg.push_back ((uint8_t) x); }

static void BuildFixture (int nExtraPipes = 0, int nPipeMps = 512)
{
	Cfg.clear ();
	D ({9, 2, 0, 0, 6, 1, 0, 0xC0, 0});
	D ({9, 4, 0, 0, 0, 0xFF, 0, 0, 0});
	D ({9, 4, 1, 0, 0, 0xFF, 0, 0, 0});
	const int Mps[3] = {32, 88, 256};
	for (int a = 1; a <= 3; a++)
	{
		D ({9, 4, 1, a, 1, 0xFF, 0, 0, 0});
		D ({7, 5, 0x83, 3, Mps[a - 1] & 0xFF, Mps[a - 1] >> 8, 1});
	}
	D ({9, 4, 2, 0, 0, 0xFF, 0, 0, 0});
	for (int a = 1; a <= 3; a++)
	{
		D ({9, 4, 2, a, 1, 0xFF, 0, 0, 0});
		D ({7, 5, 0x03, 3, Mps[a - 1] & 0xFF, Mps[a - 1] >> 8, 1});
	}
	D ({9, 4, 3, 0, (uint8_t) (2 + 2 * nExtraPipes), 0xFF, 0, 0, 0});
	D ({7, 5, 0x02, 2, nPipeMps & 0xFF, nPipeMps >> 8, 0});
	D ({7, 5, 0x82, 2, nPipeMps & 0xFF, nPipeMps >> 8, 0});
	for (int k = 0; k < nExtraPipes; k++)
	{
		D ({7, 5, 4 + k, 2, 0, 2, 0});
		D ({7, 5, 0x84 + k, 2, 0, 2, 0});
	}
	D ({9, 4, 4, 0, 0, 1, 1, 0, 0});
	D ({9, 0x24, 1, 0, 1, 9, 0, 1, 5});
	D ({9, 4, 5, 0, 2, 1, 3, 0, 0});
	D ({7, 0x24, 1, 0, 1, 0x41, 0});
	D ({9, 5, 0x01, 2, 0x00, 0x02, 0, 0, 0});
	D ({5, 0x25, 1, 1, 1});
	D ({9, 5, 0x81, 2, 0x00, 0x02, 0, 0, 0});
	D ({5, 0x25, 1, 1, 3});
	Cfg[2] = Cfg.size () & 0xFF;
	Cfg[3] = Cfg.size () >> 8;
}

int main ()
{
	using namespace relay;
	BuildFixture ();
	Map m;
	CHECK (make_map (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &m));
	CHECK (m.l.n == 6);
	CHECK (m.n_in == 2 && m.n_out == 2);			// pipe + MIDI each way
	CHECK (link_ep (m, 0x83) == kAudioFromPiEp);
	CHECK (link_ep (m, 0x03) == kAudioToPiEp);
	// descriptor order: the pipe interface (3) comes before MIDI (5)
	CHECK (link_ep (m, 0x02) == chan_in_ep (0) && link_ep (m, 0x01) == chan_in_ep (1));
	CHECK (link_ep (m, 0x82) == chan_out_ep (0) && link_ep (m, 0x81) == chan_out_ep (1));
	CHECK (link_ep (m, 0x85) == 0);
	CHECK (m.l.ep[m.ep_of_in[0]].addr == 0x82 && m.l.ep[m.ep_of_out[1]].addr == 0x01);
	CHECK (m.ep_of_in[2] == -1 && m.ep_of_out[3] == -1);
	CHECK (m.l.ep[m.l.audio_in].mps == 256);

	// four channels a direction fit (pipe, 2 extra pipes, MIDI), five do not
	BuildFixture (2);
	CHECK (make_map (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &m));
	CHECK (m.n_in == 4 && m.n_out == 4);
	CHECK (link_ep (m, 0x81) == chan_out_ep (3));
	BuildFixture (3);
	CHECK (!make_map (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &m));

	// a mirrored endpoint bigger than a link packet is refused
	BuildFixture (0, 1024);
	CHECK (!make_map (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &m));

	// the link endpoints: channel IN numbers never clash with 0x81 / 0x82, OUT with 0x02
	for (unsigned c = 0; c < kChannels; c++)
	{
		CHECK (chan_in_ep (c) != kEvEp && chan_in_ep (c) != kAudioToPiEp && (chan_in_ep (c) & 0x0F) <= 7);
		CHECK (chan_out_ep (c) != kAudioFromPiEp && chan_out_ep (c) <= 7);
	}

	// one control request at a time; a late or aborted answer is told apart
	CtlSlot s = {};
	const uint8_t Setup[8] = {0xC0, 1, 0, 0, 0, 0, 32, 0};
	uint8_t a = s.begin (Setup);
	CHECK (s.pending && !s.answer ((uint8_t) (a + 1)));
	CHECK (s.answer (a) && !s.pending);
	CHECK (!s.answer (a));					// twice: the second is late
	uint8_t b = s.begin (Setup);
	CHECK (b != a);
	CHECK (s.abort () && !s.abort ());
	CHECK (!s.answer (b));					// the computer moved on: for nobody
	s.seq = 255;
	CHECK (s.begin (Setup) == 0);				// wraps

	CHECK (hist_bin (0) == 0 && hist_bin (124) == 0 && hist_bin (125) == 1 && hist_bin (1000000) == kHistBins - 1);
	CHECK (sizeof (Stats) % 4 == 0);

	printf ("test_relay_proto: ok\n");
	return 0;
}
