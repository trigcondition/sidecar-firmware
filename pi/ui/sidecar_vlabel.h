// pi/ui/sidecar_vlabel.h - a Sidecar voice is named for the Rytm track(s) that play it, never a number (voice 1 is BT's,
// voice 2 SD's; the routing is core/ui/remote.cpp kTrackVoice). Lists run in the Rytm's track order.
#ifndef SIDECAR_VLABEL_H
#define SIDECAR_VLABEL_H

static const char *const kVoiceName[8] = {"BD", "BT", "SD", "LT", "CH/OH", "MT/HT", "CY/CB", "RS/CP"};
static const unsigned char kVoiceOrder[8] = {0, 2, 7, 1, 3, 5, 4, 6};	// the n-th place in a list: its voice
static const unsigned char kVoicePos[8] = {0, 3, 1, 4, 6, 5, 7, 2};	// a voice: its place in a list

#endif
