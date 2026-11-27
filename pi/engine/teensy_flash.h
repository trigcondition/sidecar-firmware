// pi/engine/teensy_flash.h - the Pi writes a new program into the Teensy 4.1 relay (docs/teensy-flash.md), so the Teensy's
// micro-USB stays on the Pi for a firmware change. The web (paired, web.cpp):
//   POST /teensy/put?off=&total=&crc=   the .hex in chunks (multipart, as /update/put): parsed and checked as it comes;
//                                       the last chunk's CRC-32 must be the whole file's
//   POST /teensy/flash  [expect=<build>]   start: the relay (1209:0002) is told to enter its bootloader (kReqBootloader),
//                                       or a Teensy already in it is used; the blocks are written, the program started,
//                                       and the relay must come back and name its build (its log's start line);
//                                       expect= (build.ps1's stamp, git short hash, "+" for a changed tree) must match
//   GET  /teensy/state                  JSON: where it stands
// What runs where: the web worker hands the file over (Put, Start); a task of its own (core 0) steps the write; the
// bootloader's driver (CTeensyBoot, teensy_flash.cpp) sends each block as an async SET_REPORT and the xHCI IRQ completes it.
// A bad write leaves the Teensy in its bootloader: flash again (the driver takes a Teensy already there), or press its
// program button.
#ifndef _teensy_flash_h
#define _teensy_flash_h

#include "fsweb.h"
#include <stdint.h>

namespace teensy_flash {

// one chunk of the upload (off 0 starts a new one). r: 200 "ok <next offset>" or "done <bytes> bytes, <n> blocks, sha256 <hex>"
void Put (uint64_t nOff, uint64_t nTotal, uint32_t nCrc, const uint8_t *pData, unsigned nData, fsweb::Result *r);
// start the write of the uploaded program; pExpect "" or the build the relay must name afterwards
void Start (const char *pExpect, fsweb::Result *r);
// the state as JSON (bytes written, <= 0: no room)
int StateJson (char *pOut, unsigned nCap);
// the Pi's hub is about to be deleted (ob_usb.cpp's root-port reset of a hub that does not answer): no more polls of it
void HubGone (void);

}	// namespace teensy_flash

#endif
