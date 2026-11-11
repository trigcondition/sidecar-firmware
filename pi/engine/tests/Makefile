# pi/engine/tests/Makefile - the Pi engine's portable parts on the Mac: make -C pi/engine/tests (shim/: the few
# Circle headers guard.h needs). make -C pi/engine/tests content: bridge.cpp's self-test content (test_content.cpp)
# against the core's Mac port (make -C core engines first; the core library is built here as needed). Also the build's version stamp (tools/tests/test_gen_version.sh), the Pi 5 release kernel's flags (test_pi5_flags.sh: the gadget, no test mode) and the Rytm MIDI bridge's Mac relay (tools/tests/test_rytm_bridge.py).
$(shell ../../../tools/gen_version.sh)
CXX ?= c++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -I.. -I../../../core -Ishim -fsanitize=address,undefined -fno-sanitize-recover=all
SRC := ../output.cpp ../midi_in.cpp ../emu_proto.cpp ../../../core/fx/out_measure.cpp
TESTS := $(patsubst %.cpp,build/%,$(filter-out test_content.cpp,$(wildcard test_*.cpp))) build/test_stor_fatfs_exfat build/test_stor_fatfs_lfn3
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
CORE := ../../../core
ENG := $(CORE)/build/engines
include $(CORE)/engines/engines.mk
CORE_SRC := $(addprefix $(CORE)/,engine.cpp project_key.cpp lfo.cpp hooks.cpp notes.cpp voices.cpp sysex.cpp host_link.cpp slots.cpp pool/card_index.cpp \
            pool/pool.cpp pool/sample_ram_pool.cpp presets/libs.cpp host/platform_host.cpp host/newlib_rand.cpp)
.PHONY: all clean content fatfs-mac
all: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; ../../../tools/tests/test_gen_version.sh; ./test_pi5_flags.sh; python3 ../../../tools/tests/test_rytm_bridge.py -q; echo "ALL PI PORTABLE TESTS PASS"
build/%: %.cpp $(SRC) $(wildcard ../*.h) $(wildcard shim/circle/*.h) $(CORE)/fx/out.h $(CORE)/fx/out_mix.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)
# usb=mirror's descriptor and per-block helpers (mirror_desc.h) with the Overbridge block layout (ob_block.h)
build/test_mirror_desc: test_mirror_desc.cpp ../mirror_desc.h ../../../teensy/libraries/RytmHost/ob_block.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../../teensy/libraries/RytmHost -o $@ $<
# the real ob_usb.cpp on a fake Circle (shim_ob/); copied, so its quoted includes find the shim's neighbours before the real ones
build/test_ob_usb: test_ob_usb.cpp ../ob_usb.cpp ../ob_usb.h ../ep_halt.h ../ob_follow.cpp ../ob_follow.h ../a4_in.cpp ../a4_in.h ../drift_src.h $(CORE)/device/a4.h ../output.cpp ../../../teensy/libraries/RytmHost/ob_block.h $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/device/device.h $(CORE)/fx/out.h $(CORE)/fx/out.cpp $(CORE)/fx/send_fx.h $(wildcard shim_ob/*.h) $(wildcard shim_ob/circle/*.h) $(wildcard shim_ob/circle/usb/*.h) check.h
	@mkdir -p build/ob_src
	cp ../ob_usb.cpp ../ob_usb.h ../ob_follow.cpp ../ob_follow.h build/ob_src/
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/ob_src -Ishim_ob -I.. -I../../../core -I../../../teensy/libraries/RytmHost -o $@ test_ob_usb.cpp build/ob_src/ob_usb.cpp build/ob_src/ob_follow.cpp ../a4_in.cpp ../output.cpp $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp -DOB_USB_TEST
# usb=mirror's box side: the same, built with CIRCLE_GADGET_MIRROR and RASPPI=4 (the xHC's Reset Endpoint), against the
# recording gadget of shim_ob/sidecar_gadget.h
build/test_ob_mirror: test_ob_mirror.cpp ../ob_usb.cpp ../ob_usb.h ../ep_halt.h ../ob_follow.cpp ../ob_follow.h ../a4_in.cpp ../a4_in.h ../mirror_desc.h ../drift_src.h $(CORE)/device/a4.h ../output.cpp ../../../teensy/libraries/RytmHost/ob_block.h $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/device/device.h $(CORE)/fx/out.h $(wildcard shim_ob/*.h) $(wildcard shim_ob/circle/*.h) $(wildcard shim_ob/circle/usb/*.h) check.h
	@mkdir -p build/ob_mirror_src
	cp ../ob_usb.cpp ../ob_usb.h ../ob_follow.cpp ../ob_follow.h build/ob_mirror_src/
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/ob_mirror_src -Ishim_ob -I.. -I../../../core -I../../../teensy/libraries/RytmHost -o $@ test_ob_mirror.cpp build/ob_mirror_src/ob_usb.cpp build/ob_mirror_src/ob_follow.cpp ../a4_in.cpp ../output.cpp $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp -DOB_USB_TEST -DCIRCLE_GADGET_MIRROR -DRASPPI=4
# usb=mirror's forwarded request and the box going away (mirror_ctl.h), as sidecar_gadget.cpp drives them
build/test_mirror_ctl: test_mirror_ctl.cpp ../mirror_ctl.h ../mirror_desc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# core/ui/tap_names.h on its own, no system header (rec_pi.cpp's include: the kernel build has no <new>); rec_pi.cpp
# includes it and declares tap_source_name nowhere by hand
build/test_tap_names: test_tap_names.cpp $(CORE)/ui/tap_names.h ../rec_pi.cpp
	@mkdir -p build
	grep -q '^#include "ui/tap_names.h"' ../rec_pi.cpp && ! grep -n 'tap_source_name *(int' ../rec_pi.cpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -nostdinc -nostdinc++ -I$(CORE) -o $@ $<
# the USB touch screen's descriptor parser and contact tracker (pi/circle/hid_digitizer.h, circle-touch.patch)
build/test_hid_digitizer: test_hid_digitizer.cpp ../../circle/hid_digitizer.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# when a halted HID report endpoint is escalated to a device re-enumeration (pi/circle/hid_reenum.h,
# circle-hid-reenum2.patch): the timeline measured on the real panel, replayed
build/test_hid_reenum: test_hid_reenum.cpp ../../circle/hid_reenum.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the DWC2 gadget's bus suspend and resume (pi/circle/gadget_wake.h, circle-gadget-wake.patch)
build/test_gadget_wake: test_gadget_wake.cpp ../../circle/gadget_wake.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# a USB reset's TX FIFO flush at IRQ level (pi/circle/gadget_flush.h, circle-gadget-reset.patch)
build/test_gadget_flush: test_gadget_flush.cpp ../../circle/gadget_flush.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the Pi 5's 640x480-without-EDID hint (pi/ui/display_hint.h) and the boot screen that shows it (sidecar_splash.c, C)
HINT_C := ../../ui/sidecar_splash.c ../../ui/sidecar_dot.c ../../ui/sidecar_dotfont.c ../../ui/sidecar_wordmark.c
build/test_display_hint: test_display_hint.cpp ../../ui/display_hint.h ../../ui/sidecar_splash.h $(HINT_C) check.h
	@mkdir -p build/hint
	for f in $(HINT_C); do $(CC) -O2 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=all -c $$f -o build/hint/$$(basename $$f .c).o || exit 1; done
	$(CXX) $(CXXFLAGS) -o $@ $< $(patsubst ../../ui/%.c,build/hint/%.o,$(HINT_C))
# the touch screen's 4-point calibration (pi/ui/touch_cal.c, Settings > Display)
build/test_touch_cal: test_touch_cal.cpp ../../ui/touch_cal.c ../../ui/touch_cal.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the send FX's Pi part with the core's (card-free) send FX
build/test_sendfx_pi: test_sendfx_pi.cpp ../sendfx_pi.cpp ../sendfx_pi.h ../spectrum_pi.cpp ../spectrum_pi.h ../spectrum.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/stutter_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../sendfx_pi.cpp ../spectrum_pi.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/master.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# the master FX: the OUT fill's crossfade, the ring to core 2 and back, bit for bit while bypassed, latency, cost
# the spectrum (spectrum.h: the tap, the FFT bands, the reader; spectrum_pi.cpp: the engine's taps) and its /ws topic, with
# the analysis's cost on the Mac (printed; the sanitizers make it slower than a plain -O2 build)
build/test_spectrum: test_spectrum.cpp ../spectrum.h ../spectrum_pi.cpp ../spectrum_pi.h ../ws_topics.h ../ws_session.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../spectrum_pi.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
build/test_masterfx_pi: test_masterfx_pi.cpp ../masterfx_pi.cpp ../masterfx_pi.h $(CORE)/fx/limiter.h $(CORE)/fx/master.cpp $(CORE)/fx/master.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../masterfx_pi.cpp $(CORE)/fx/master.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# the recorder (WAV writer, ring, channel lists) on a temp folder
# (core/rec/rec_names.cpp: the parts shared with the Sidecar Mini's recorder, with the Mac's port of the core)
build/test_recorder: test_recorder.cpp ../recorder.cpp ../recorder.h ../rec_wav.h $(CORE)/rec/rec_wav.h $(CORE)/rec/rec_names.h $(CORE)/rec/rec_names.cpp ../spsc.h check.h
	@mkdir -p build
	$(CXX) -I$(CORE)/host $(CXXFLAGS) -DREC_HOST -o $@ $< ../recorder.cpp $(CORE)/rec/rec_names.cpp
# the takes' playback (WAV reader, ring, fades) on a temp folder
build/test_rec_play: test_rec_play.cpp ../rec_play.cpp ../rec_play.h $(CORE)/rec/rec_wav.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DREC_HOST -o $@ $< ../rec_play.cpp
# the web file browser (fsweb.cpp) on a temp folder, with the plugin loader's header/CRC check
build/test_fsweb: test_fsweb.cpp ../fsweb.cpp ../fsweb.h $(RMPSRC)/rmp_loader.cpp $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFSWEB_HOST -I$(RMPSRC) -o $@ $< ../fsweb.cpp $(RMPSRC)/rmp_loader.cpp $(CORE)/lib/sha256.cpp
# the power supply's state (power_state.cpp): get_throttled and the Pi 5's max_current to the warning and /status
build/test_power_state: test_power_state.cpp ../power_state.cpp ../power_state.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../power_state.cpp
# the Wi-Fi settings (wifi_conf.cpp) on a temp folder
build/test_wifi_conf: test_wifi_conf.cpp ../wifi_conf.cpp ../wifi_conf.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../wifi_conf.cpp
# the Rytm MIDI bridge (midibridge.h, usbmidi_merge.h: header only), with the core's SHA-256 (its HMAC)
build/test_midibridge: test_midibridge.cpp ../midibridge.h ../usbmidi_merge.h ../usbmidi_pack.h $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE)/lib/sha256.cpp
# the pairing code (paircode.cpp) on a temp folder, with the core's SHA-256
build/test_paircode: test_paircode.cpp ../paircode.cpp ../paircode.h ../wifi_conf.cpp ../wifi_conf.h $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../paircode.cpp ../wifi_conf.cpp $(CORE)/lib/sha256.cpp
# the web page's bundle endpoints (bundleweb.cpp) over core/bundle: linked with the core's Mac library (make -C core
# engines lib), its service worker (www/bundle-sw.js) built in as build.sh does
build/hosted/bundle_sw.h: ../www/bundle-sw.js
	@mkdir -p build/hosted
	perl -e 'local $$/; my $$s = <STDIN>; print "static const char BundleSw[] = R\"SIDECARSW($$s)SIDECARSW\";\n"' < $< > $@
build/test_bundleweb: test_bundleweb.cpp ../bundleweb.cpp ../bundleweb.h build/hosted/bundle_sw.h $(wildcard $(CORE)/bundle/*.h) check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. -Ibuild -I$(RMPSRC) \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_bundleweb.cpp ../bundleweb.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# the web page's Pool tab JSON (poolweb.cpp, GET /pool) over core/lib/pools on the core's Mac library
build/test_poolweb: test_poolweb.cpp ../poolweb.cpp ../poolweb.h $(CORE)/lib/pools.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_poolweb.cpp ../poolweb.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# the web page's JUNO / 6-OP FM lists (presetweb.cpp, GET /presets) over core/presets' index on the core's Mac library
build/test_presetweb: test_presetweb.cpp ../presetweb.cpp ../presetweb.h $(CORE)/presets/preset_index.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_presetweb.cpp ../presetweb.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# 0.5.0: the display's MORPH page view (morph_pi.cpp) from the scenes (core/mod/scene.h) on the core's Mac library
build/test_morph_pi: test_morph_pi.cpp ../morph_pi.cpp ../morph_pi.h ../../ui/sidecar_morph.h $(CORE)/mod/scene.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_POLY=1 -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_morph_pi.cpp ../morph_pi.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
$(CORE)/build/libcore.dylib: $(wildcard $(CORE)/*.cpp $(CORE)/*/*.cpp $(CORE)/*.h $(CORE)/*/*.h)
	@test -f $(CORE)/build/libengines.a || { echo "bundleweb: run 'make -C core engines' first"; exit 1; }
	$(MAKE) -C $(CORE) lib
# touch2: the SCOPE page's provider (scope_pi.cpp) against a stand-in tap and selected track (the test's own)
build/test_scope_pi: test_scope_pi.cpp ../scope_pi.cpp ../scope_pi.h ../../ui/sidecar_scope.h check.h
	@mkdir -p build
	$(CXX) -I$(CORE)/host $(CXXFLAGS) -Wno-unused-parameter -o $@ $< ../scope_pi.cpp
# the storage layer (docs/STORAGE.md): the volumes' state machine and rules, the bench on a simulated drive and a temp folder
build/test_stor_vol: test_stor_vol.cpp ../stor_vol.cpp ../stor_vol.h ../stor_out.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../stor_vol.cpp
build/test_stor_bench: test_stor_bench.cpp ../stor_bench.cpp ../stor_bench.h ../stor_vol.cpp ../stor_vol.h ../stor_out.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../stor_bench.cpp ../stor_vol.cpp
# FatFs on file images (test_stor_fatfs.cpp): Circle's addon/fatfs sources (CIRCLE_BUILD: pi/circle/build, or the main
# checkout's from a worktree), copied into build/fatfs-{fat,exfat} with the host's changes (no lock, f_mkfs, no clock,
# LFN on the stack); exfat: the STOR_EXFAT switch's two (pi/engine/build.sh: FF_FS_EXFAT 1, FF_LBA64 1).
# make fatfs-mac: the same against images macOS makes (hdiutil: MBR / GPT x FAT32 / exFAT), removed after
CIRCLE_BUILD ?= $(firstword $(wildcard ../../circle/build/circle) $(shell git -C ../../.. worktree list --porcelain 2>/dev/null | sed -n '1s/^worktree //p')/pi/circle/build/circle)
FATFS_SRC ?= $(if $(filter %/circle,$(CIRCLE_BUILD)),$(CIRCLE_BUILD),$(CIRCLE_BUILD)/circle)/addon/fatfs
FATFS_HOST := -e 's/^\#define FF_FS_REENTRANT.*/\#define FF_FS_REENTRANT 0/' -e 's/^\#define FF_USE_MKFS.*/\#define FF_USE_MKFS 1/' \
	      -e 's/^\#define FF_FS_NORTC.*/\#define FF_FS_NORTC 1/' -e 's/^\#define FF_USE_LFN.*/\#define FF_USE_LFN 1/'
STOR_EXFAT_SED := -e 's/^\#define FF_FS_EXFAT.*/\#define FF_FS_EXFAT 1/' -e 's/^\#define FF_LBA64.*/\#define FF_LBA64 1/'
build/fatfs-%/ff.h: $(FATFS_SRC)/ff.c $(FATFS_SRC)/ffconf.h
	@test -f $(FATFS_SRC)/ff.c || { echo "fatfs: no $(FATFS_SRC) (CIRCLE_BUILD=.../pi/circle/build)"; exit 1; }
	@mkdir -p $(dir $@)
	cp $(FATFS_SRC)/ff.c $(FATFS_SRC)/ff.h $(FATFS_SRC)/diskio.h $(FATFS_SRC)/ffunicode.c $(dir $@)
	sed $(FATFS_HOST) $(if $(filter exfat,$*),$(STOR_EXFAT_SED),) $(FATFS_SRC)/ffconf.h > $(dir $@)ffconf.h
FATFS_DEPS = test_stor_fatfs.cpp ../stor_vol.cpp ../stor_vol.h check.h
# the DWC2 gadget's EP0 and endpoint cancel (test_gadget_ep0.cpp): Circle's dwusbgadgetendpoint*.cpp with the Pi 4
# gadget patch stack (pi/engine/build.sh's order, up to circle-gadget-stray.patch), copied into build/dwc and patched
# there, on a fake register file (shim_dwc/)
DWC_ROOT := $(if $(filter %/circle,$(CIRCLE_BUILD)),$(CIRCLE_BUILD),$(CIRCLE_BUILD)/circle)
DWC_PATCHES := $(addprefix ../../circle/,circle-gadget.patch circle-gadget-wake.patch circle-gadget-reset.patch \
	       circle-gadget-mirror.patch circle-gadget-softconnect.patch circle-gadget-stray.patch)
build/dwc/.patched: $(DWC_PATCHES) ../../circle/gadget_wake.h ../../circle/gadget_flush.h
	@test -f $(DWC_ROOT)/lib/usb/gadget/dwusbgadgetendpoint0.cpp || { echo "test_gadget_ep0: no Circle at $(DWC_ROOT) (CIRCLE_BUILD=.../pi/circle/build)"; exit 1; }
	@rm -rf build/dwc && mkdir -p build/dwc/lib/usb
	@cp -R $(DWC_ROOT)/include build/dwc/ && cp -R $(DWC_ROOT)/lib/usb/gadget build/dwc/lib/usb/
	@cp ../../circle/gadget_wake.h build/dwc/include/circle/usb/gadget/gadgetwake.h
	@cp ../../circle/gadget_flush.h build/dwc/include/circle/usb/gadget/gadgetflush.h
	@cd build/dwc && for p in $(DWC_PATCHES); do patch -s -p1 < ../../$$p || exit 1; done
	@touch $@
build/test_gadget_ep0: test_gadget_ep0.cpp ../mirror_ep.h build/dwc/.patched $(wildcard shim_dwc/*.h shim_dwc/circle/*.h shim_dwc/circle/usb/*.h shim_dwc/circle/usb/gadget/*.h) check.h
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -Ishim_dwc -Ibuild/dwc/include -I. -I.. -o $@ $< build/dwc/lib/usb/gadget/dwusbgadgetendpoint.cpp \
	  build/dwc/lib/usb/gadget/dwusbgadgetendpoint0.cpp
build/test_stor_fatfs: build/fatfs-fat/ff.h $(FATFS_DEPS)
	cc -O1 -g -w -c -o build/fatfs-fat/ff.o build/fatfs-fat/ff.c && cc -O1 -g -w -c -o build/fatfs-fat/ffunicode.o build/fatfs-fat/ffunicode.c
	$(CXX) $(CXXFLAGS) -Ibuild/fatfs-fat -o $@ test_stor_fatfs.cpp ../stor_vol.cpp build/fatfs-fat/ff.o build/fatfs-fat/ffunicode.o
# the Pi's FF_USE_LFN 3 (ff_memalloc: dir_clear's 32 KB zero-fill buffer) instead of the host's 1
build/fatfs-lfn3/ff.h: $(FATFS_SRC)/ff.c $(FATFS_SRC)/ffconf.h
	@mkdir -p $(dir $@)
	cp $(FATFS_SRC)/ff.c $(FATFS_SRC)/ff.h $(FATFS_SRC)/diskio.h $(FATFS_SRC)/ffunicode.c $(dir $@)
	sed $(FATFS_HOST) -e 's/^\#define FF_USE_LFN.*/\#define FF_USE_LFN 3/' $(FATFS_SRC)/ffconf.h > $(dir $@)ffconf.h
build/test_stor_fatfs_lfn3: build/fatfs-lfn3/ff.h $(FATFS_DEPS)
	cc -O1 -g -w -c -o build/fatfs-lfn3/ff.o build/fatfs-lfn3/ff.c && cc -O1 -g -w -c -o build/fatfs-lfn3/ffunicode.o build/fatfs-lfn3/ffunicode.c
	$(CXX) $(CXXFLAGS) -Ibuild/fatfs-lfn3 -o $@ test_stor_fatfs.cpp ../stor_vol.cpp build/fatfs-lfn3/ff.o build/fatfs-lfn3/ffunicode.o
build/test_stor_fatfs_exfat: build/fatfs-exfat/ff.h $(FATFS_DEPS)
	cc -O1 -g -w -c -o build/fatfs-exfat/ff.o build/fatfs-exfat/ff.c && cc -O1 -g -w -c -o build/fatfs-exfat/ffunicode.o build/fatfs-exfat/ffunicode.c
	$(CXX) $(CXXFLAGS) -Ibuild/fatfs-exfat -o $@ test_stor_fatfs.cpp ../stor_vol.cpp build/fatfs-exfat/ff.o build/fatfs-exfat/ffunicode.o
MACIMG := build/macimg
fatfs-mac: build/test_stor_fatfs build/test_stor_fatfs_exfat
	@rm -rf $(MACIMG); mkdir -p $(MACIMG)
	hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -layout MBRSPUD -volname SIDECAR -o $(MACIMG)/fat32-mbr.dmg
	hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -layout GPTSPUD -volname SIDECAR -o $(MACIMG)/fat32-gpt.dmg
	hdiutil create -quiet -size 64m -fs ExFAT -layout MBRSPUD -volname SIDECAR -o $(MACIMG)/exfat-mbr.dmg
	hdiutil create -quiet -size 64m -fs ExFAT -layout GPTSPUD -volname SIDECAR -o $(MACIMG)/exfat-gpt.dmg
	./build/test_stor_fatfs $(MACIMG) && ./build/test_stor_fatfs_exfat $(MACIMG); s=$$?; rm -rf $(MACIMG); exit $$s
content: build/test_content
	./build/test_content
# Linked with the core's Mac library (make -C core lib, as test_bundleweb), compiled with its feature flags: a list of
# core sources and flags of its own here drifted from core/Makefile's (CORE_POLY, CORE_MSAMP and every new .cpp) and the
# target stopped building. The engines' headers too (core/build/engines: make -C core engines copies them).
CONTENT_FLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -DCORE_POLY=1 -DCORE_MSAMP=1 \
  -I$(CORE) -I$(CORE)/host -I$(RMPSRC) $(ENG_INC) $(ENG_FLAGS)
build/test_content: test_content.cpp ../bridge.cpp ../bridge.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) $(CONTENT_FLAGS) -o $@ $< ../bridge.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
clean:
	rm -rf build
