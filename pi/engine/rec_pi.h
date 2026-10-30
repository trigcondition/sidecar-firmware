// pi/engine/rec_pi.h - the recorder (recorder.h) and the takes' playback (rec_play.h) in the Pi kernel: its memory reserved once at boot (cmdline recbuf=MB,
// default: from the free heap, 64..256 MB (recorder.h recbuf_mb); 0: recording off), the card through FatFs (or a USB drive / the NVMe SSD: rec=vol:, docs/STORAGE.md
// "Recording to a USB drive"), the boot scan of /Recordings, the console's rec commands,
// /status's "rec" member. Everything here is core 0's: the IN completion (xHCI interrupt) and the OUT fill feed
// g_Rec, the loop drains it (Poll), the web task and the console control it.
#ifndef _rec_pi_h
#define _rec_pi_h

#include <circle/string.h>
#include "recorder.h"
#include "rec_play.h"
#include "../ui/sidecar_rec.h"

extern rec::Recorder g_Rec;
extern recplay::Player g_Play;		// the takes' playback (rec_play.h): mixed onto MAIN in the OUT fill (ob_usb.cpp)

namespace rec_pi {
void Init (void);			// CKernel::Initialize, after the card is mounted (bridge::begin)
void Poll (void);			// core 0's loop
bool Command (const char *pCmd);	// rec, rec=start|stop|arm|ch:LIST|bits:16|24|name:[PREFIX]|vol:SD|USB|USB2|USB3|NVME|
					// fix:VOL|del:TAKE (a drive's: del:USB:TAKE); play, play=start|stop|scan|take:N|ch:N|
					// level:N|start:TAKE/NN (true: handled). A take on a drive is "USB:take_004" everywhere.
void Status (CString *pOut);		// ,"rec":{...},"play":{...}
void Transport (u8 nStatus, u16 nSPP);	// MIDI realtime (core 3): PLAY/STOP for an armed recorder; then the send FX's clock
int UiState (unsigned *pSecs);		// the display: 0 off, 1 armed, 2 recording (and its seconds)
// The display's REC page (pi/ui/sidecar_rec.h), core 0: UiWant while it shows (the recorder's meters on, the takes
// read again on the way in); UiFill at most 25 times a second (the meters' peaks since the last fill)
void UiWant (bool bOn);
// /fs (web.cpp, fsweb.h Backend::busy): the path ("/Recordings/...", "/USB/Recordings/...") is the take being recorded
// (or closed), inside it or a folder above it - never deleted or renamed then
bool TakeBusy (const char *pPath);
void UiFill (sidecar_rec_view_t *pView);
// touch2: the SCOPE page (pi/ui/sidecar_scope.h): its meters are the recorder's (on while it shows); ScopePeaks the 12
// IN channels' peaks since the last call (24-bit). CardSpace: the card as the recorder last read it (every 10 s;
// 0 / 0 none) - the STATUS page
void ScopeWant (bool bOn);
void ScopePeaks (uint32_t *pPeaks);
void CardSpace (unsigned *pFreeMB, unsigned *pTotalMB);
// a storage bench done (stor_pi.cpp): that volume's 64 KB write speed for the warning before a take (recorder.h
// rate_warning), kept in SD:/.rytm/recspeed.txt
void BenchDone (int nVol, unsigned nWriteKBps);
}

#endif
