/* core/sidecar_features.h - compile-time feature switches shared by the engine (C++) and the touchscreen (C).
   SIDECAR_MORPH: the scene morph fader (core/mod/scene.*, the MORPH page, the touchscreen's MORPH page, MORPH as an XY
   target, the web's Morph page). 0 in 0.5.0: every way in is hidden and the engine is inert, but the code stays built
   and a project's scene and XY data is read and written unchanged. 0.5.2 turns it on: change the 0 below to 1. */
#pragma once
/* SIDECAR_LOOPER: the master looper (core/loop, the LOOP page on the Pi screen and the Rytm, the web's Loop page, the console's
   loop= commands, the Pi's ring and grab RAM). 0 in 0.5.2: every way in is hidden and the engine can never act (no ring, no
   record, no play, no Rytm track mute, no LIVE VIA, loop.txt on the card ignored), but the code stays built and tested with
   it on. The redesign (POST at unity) turns it on: change the 0 below to 1. */
#ifndef SIDECAR_LOOPER
#define SIDECAR_LOOPER 0
#endif
#ifndef SIDECAR_MORPH
#define SIDECAR_MORPH 1
#endif
