//
// mirror_target.h - where a box's usb=mirror goes (docs/usb-mirror.md): the computer side of one box's mirror. A box's stream
// (CObUsb, one per box: ob_usb.h) hands its descriptors, strings, pipe packets, EP0 answers and per-block bytes to its target,
// and the target calls back into that box (CObUsb's members: Control, PipeOut, PipeInNext, ...). The Pi's USB-C gadget
// (CSidecarGadget, through an adapter in ob_usb.cpp) is one target; a second device port (a Teensy relay) can be another, so
// each box can have its own. Which box goes where is decided at run time, USB-C first (mirror_assign.h).
//
#ifndef _mirror_target_h
#define _mirror_target_h

#include <circle/types.h>

class CObUsb;

class IMirrorTarget
{
public:
	virtual ~IMirrorTarget (void) {}

	virtual void SetBox (CObUsb *pBox) = 0;		// the box it mirrors now (0: none); core 0's loop or task level
	virtual boolean Mirror (void) const = 0;	// it mirrors at all now (the gadget: USB-C mode OVERBRIDGE with usb=mirror)
	virtual boolean MirrorShown (void) const = 0;	// the computer sees the box now
	virtual boolean MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg) = 0;	// FALSE: not one it can carry
	virtual void MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength) = 0;
	virtual void MirrorBoxUp (boolean bUp) = 0;	// read completely / gone
	virtual void MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength) = 0;	// xHCI IRQ: the box's answer (< 0: STALL)
	virtual boolean MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength) = 0;	// xHCI IRQ: FALSE dropped (no computer)
	virtual void MirrorPipeOutDone (u8 uchAddr) = 0;	// xHCI IRQ: the box took the computer's packet
	virtual boolean MirrorOutExtra (u8 *pBlock, u16 nCounter) = 0;	// the box's OUT fill: the computer's per-block bytes into its block
	virtual void FromBoxBlock (const u8 *pBlock) = 0;	// xHCI IRQ: one IN block of the box (12-channel layout) for the computer
	// the box's OUT fill: the computer's OUT added to the frames (pVoices: the Sidecar's voices on the box's channels, 0 none)
	virtual void ToBox (s32 (*pFrames)[12], const s16 (*pVoices)[8], unsigned nFrames) = 0;
	virtual void MirrorPoll (void) {}		// core 0's loop, every pass: its own control and attach work
	virtual void MirrorMoved (void) {}		// its box moved to another target (not gone): off the computer's bus now
};

#endif
