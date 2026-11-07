// pi/engine/usbc_pi.cpp - see usbc_pi.h.
#include "usbc_pi.h"

namespace usbc_pi {

static volatile int s_nMode = -1, s_nShown = 0, s_nAsk = -1;
static volatile unsigned s_nModes = 0;

void Publish (int nMode, unsigned nModes, int nShown)
{
	s_nMode = nMode;
	s_nModes = nModes;
	s_nShown = nShown;
}
int Mode () { return s_nMode; }
unsigned Modes () { return s_nModes; }
int Shown () { return s_nShown; }
void Ask (int nMode) { __atomic_store_n (&s_nAsk, nMode, __ATOMIC_RELEASE); }
int TakeAsk ()
{
	return __atomic_exchange_n (&s_nAsk, -1, __ATOMIC_ACQ_REL);	// read and clear in one: an Ask between them is not lost
}

}
