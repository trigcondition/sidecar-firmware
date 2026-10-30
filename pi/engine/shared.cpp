// pi/engine/shared.cpp - see shared.h.
#include "shared.h"

Output g_Output;
Spsc<uint8_t, 8192> g_MidiIn;
Spsc<uint8_t, kMidiOutBytes> g_MidiOut;
RecordReader<Spsc<uint8_t, kMidiOutBytes>> g_ToHost (&g_MidiOut);
Spsc<TCmd, 16> g_Cmds;
Spsc<uint32_t, 1024> g_KeysIn;
Spsc<uint32_t, 512> g_KeysToRytm;
char g_KeyNames[5][24];
Spsc<uint32_t, 256> g_AsampMidi;
volatile bool g_bComputerMidi;
