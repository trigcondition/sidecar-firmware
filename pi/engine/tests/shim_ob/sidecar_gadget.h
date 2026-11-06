#pragma once
#include <circle/types.h>
#ifndef CIRCLE_GADGET_MIRROR
class CSidecarGadget {   // no gadget in the test
 public:
  static CSidecarGadget* Get() { return nullptr; }
  void ToRytm(s32 (*)[12], const s16 (*)[8], unsigned, boolean) {}
  void FromRytmBlock(const u8*) {}
  void RytmMIDIConnected(bool) {}
  bool ForRytm(u32*) { return false; }
  bool ForRytmInSysEx() { return false; }
  void ComputerSendFailed() {}
};
#else
// usb=mirror (test_ob_mirror.cpp): a gadget that notes what the box's side hands it
#include <vector>
#include "mirror_desc.h"
class CSidecarGadget {
 public:
  static CSidecarGadget* Get() { return s_pThis; }
  static inline CSidecarGadget* s_pThis = nullptr;
  void ToRytm(s32 (*)[12], const s16 (*)[8], unsigned, boolean) {}
  boolean MirrorOutExtra(u8 *, u16) { return FALSE; }
  void FromRytmBlock(const u8*) {}
  void RytmMIDIConnected(bool) {}
  bool ForRytm(u32*) { return false; }
  bool ForRytmInSysEx() { return false; }
  void ComputerSendFailed() {}
  boolean Mirror() { return mirror; }
  boolean MirrorShown() const { return shown; }   // the computer sees the box (else the Sidecar device)
  boolean MirrorSetDescriptors(const u8*, const u8*, unsigned) { return TRUE; }
  void MirrorSetString(u8, const u8*, unsigned) { ++strings; }
  void MirrorBoxUp(boolean b) { box.push_back(b); }
  void MirrorControlDone(unsigned seq, const u8*, int n) { done.push_back({seq, n}); }
  boolean MirrorPipeIn(u8 a, const u8*, unsigned n) { pipe_in.push_back({a, n}); return hold_pipe_in; }
  void MirrorPipeOutDone(u8 a) { pipe_out_done.push_back(a); }
  struct Done { unsigned seq; int n; };
  struct Pkt { u8 addr; unsigned n; };
  bool mirror = true, shown = true, hold_pipe_in = false;
  int strings = 0;
  std::vector<int> box;
  std::vector<Done> done;
  std::vector<Pkt> pipe_in;
  std::vector<u8> pipe_out_done;
};
#endif
