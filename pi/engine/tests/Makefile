# pi/engine/tests/Makefile - the Pi engine's portable parts on the Mac: make -C pi/engine/tests (shim/: the few
# Circle headers guard.h needs). make -C pi/engine/tests content: bridge.cpp's self-test content (test_content.cpp)
# against the core's Mac port (make -C core engines first; the core library is built here as needed). Also the build's version stamp (tools/tests/test_gen_version.sh), the Pi 5 release kernel's flags (test_pi5_flags.sh: the gadget, no test mode) and the Rytm MIDI bridge's Mac relay (tools/tests/test_rytm_bridge.py).
$(shell ../../../tools/gen_version.sh)
CXX ?= c++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -I.. -I../../../core -Ishim -fsanitize=address,undefined -fno-sanitize-recover=all
SRC := ../output.cpp ../midi_in.cpp ../emu_proto.cpp ../../../core/fx/out_measure.cpp
TESTS := $(patsubst %.cpp,build/%,$(filter-out test_content.cpp test_keys_ui.cpp,$(wildcard test_*.cpp))) build/test_stor_fatfs_exfat build/test_stor_fatfs_lfn3 \
         build/test_keys_ui
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
CORE := ../../../core
ENG := $(CORE)/build/engines
include $(CORE)/engines/engines.mk
CORE_SRC := $(addprefix $(CORE)/,engine.cpp project_key.cpp lfo.cpp hooks.cpp notes.cpp voices.cpp sysex.cpp host_link.cpp slots.cpp pool/card_index.cpp \
            pool/pool.cpp pool/sample_ram_pool.cpp presets/libs.cpp host/platform_host.cpp host/newlib_rand.cpp)
.PHONY: all clean content fatfs-mac
all: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; ../../../tools/tests/test_gen_version.sh; ./test_pi5_flags.sh; python3 ../../../tools/tests/test_rytm_bridge.py -q; python3 ../../../tools/tests/test_make_update.py -q; echo "ALL PI PORTABLE TESTS PASS"
build/%: %.cpp $(SRC) $(wildcard ../*.h) $(wildcard shim/circle/*.h) $(CORE)/fx/out.h $(CORE)/fx/out_mix.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)
# usb=mirror with two boxes: which one USB-C shows, which one the Teensy relay shows (mirror_assign.h, docs/usb-mirror.md)
build/test_mirror_assign: test_mirror_assign.cpp ../mirror_assign.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# usb=mirror's audio for a second port (mirror_stream.h, docs/teensy-mirror.md)
# a box's OUT starved by the host controller, named in the log (periodic_starve.h, diagnosis-periodic-starvation.md)
build/test_periodic_starve: test_periodic_starve.cpp ../periodic_starve.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# usb=mirror: a box shown only once its audio runs both ways; a starved stream restarted (attach_gate.h)
build/test_attach_gate: test_attach_gate.cpp ../attach_gate.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
build/test_slip_detect: test_slip_detect.cpp ../slip_detect.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
build/test_mirror_stream: test_mirror_stream.cpp ../mirror_stream.h ../mirror_desc.h ../../../teensy/libraries/RytmHost/ob_block.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../../teensy/libraries/RytmHost -o $@ $<
# usb=mirror through a Teensy: the link's channel map and control slot (relay_proto.h, docs/teensy-mirror.md)
build/test_relay_proto: test_relay_proto.cpp ../relay_proto.h ../mirror_desc.h ../../../teensy/libraries/RytmHost/ob_block.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../../teensy/libraries/RytmHost -o $@ $<
# the Teensy relay's program for its bootloader (teensy_hex.h, docs/teensy-flash.md)
build/test_teensy_hex: test_teensy_hex.cpp ../teensy_hex.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the write itself (teensy_flash.cpp) on a fake Circle (shim_tf/), copied so its quoted includes find the shim first
build/test_teensy_flash: test_teensy_flash.cpp ../teensy_flash.cpp ../teensy_flash.h ../teensy_hex.h ../fsweb.h $(wildcard shim_tf/*.h) $(wildcard shim_tf/circle/*.h) $(wildcard shim_tf/circle/*/*.h) $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build/tf_src
	cp ../teensy_flash.cpp build/tf_src/
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -Ishim_tf -I.. -I$(CORE) -DTEENSY_FLASH_TEST -o $@ test_teensy_flash.cpp build/tf_src/teensy_flash.cpp $(CORE)/lib/sha256.cpp
# the remote UI service's portable part (remote_srv.cpp) against the protocol library's C files (ui/remote), with the real client
RMT := ../../ui/remote
RMT_C := $(RMT)/rmt_proto.c $(RMT)/rmt_views.c $(RMT)/rmt_cli.c $(RMT)/rmt_sha256.c
build/test_remote_srv: test_remote_srv.cpp ../remote_srv.cpp ../remote_srv.h $(RMT_C) $(wildcard $(RMT)/*.h) check.h
	@mkdir -p build/rmt
	for f in $(RMT_C); do cc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -c $$f -o build/rmt/$$(basename $$f .c).o || exit 1; done
	$(CXX) $(CXXFLAGS) -o $@ test_remote_srv.cpp ../remote_srv.cpp build/rmt/*.o
# the remote UI's views with no display (remote_views_pi.cpp, the kernel's view section): a fake engine, the real service and client; links no LVGL
build/test_remote_views: test_remote_views.cpp ../remote_views_pi.cpp ../remote_views_pi.h ../remote_srv.cpp ../remote_srv.h $(RMT_C) $(wildcard $(RMT)/*.h) check.h
	@mkdir -p build/rmt
	for f in $(RMT_C); do cc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -c $$f -o build/rmt/$$(basename $$f .c).o || exit 1; done
	$(CXX) $(CXXFLAGS) -DSIDECAR_SEQED=1 -o $@ test_remote_views.cpp ../remote_views_pi.cpp ../remote_srv.cpp build/rmt/*.o
# usb=mirror's descriptor and per-block helpers (mirror_desc.h) with the Overbridge block layout (ob_block.h)
build/test_mirror_desc: test_mirror_desc.cpp ../mirror_desc.h ../../../teensy/libraries/RytmHost/ob_block.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../../teensy/libraries/RytmHost -o $@ $<
# the real ob_usb.cpp on a fake Circle (shim_ob/); copied, so its quoted includes find the shim's neighbours before the real ones
build/test_ob_usb: test_ob_usb.cpp ../ob_usb.cpp ../ob_usb.h ../ep_halt.h ../ob_follow.cpp ../ob_follow.h ../a4_in.cpp ../a4_in.h ../drift_src.h $(CORE)/device/a4.h ../output.cpp ../../../teensy/libraries/RytmHost/ob_block.h $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/device/device.h $(CORE)/fx/out.h $(CORE)/fx/out.cpp $(CORE)/fx/send_fx.h $(wildcard shim_ob/*.h) $(wildcard shim_ob/circle/*.h) $(wildcard shim_ob/circle/usb/*.h) check.h
	@mkdir -p build/ob_src
	cp ../ob_usb.cpp ../ob_usb.h ../ob_follow.cpp ../ob_follow.h build/ob_src/
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/ob_src -Ishim_ob -I.. -I../../../core -I../../../teensy/libraries/RytmHost -o $@ test_ob_usb.cpp build/ob_src/ob_usb.cpp build/ob_src/ob_follow.cpp ../a4_in.cpp ../output.cpp $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp -DOB_USB_TEST
# the looper's Pi side (test_loop_pi.cpp): the real ob_usb.cpp on the fake Circle, test_ob_usb.cpp's Bus, loop_keep.cpp on a temp folder
build/test_loop_pi: test_loop_pi.cpp test_ob_usb.cpp ../loop_keep.cpp ../loop_keep.h ../loop_plan.h ../loop_pi.h ../ob_usb.cpp ../ob_usb.h ../ep_halt.h ../ob_follow.cpp ../ob_follow.h ../a4_in.cpp ../a4_in.h ../drift_src.h $(CORE)/device/a4.h ../output.cpp ../output.h ../recorder.h $(CORE)/rec/rec_names.cpp $(CORE)/rec/rec_names.h $(CORE)/rec/rec_wav.h $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/device/device.h $(CORE)/fx/out.h $(CORE)/fx/out.cpp $(CORE)/loop/loop.h $(wildcard shim_ob/*.h) $(wildcard shim_ob/circle/*.h) $(wildcard shim_ob/circle/usb/*.h) check.h
	@mkdir -p build/loop_ob_src
	cp ../ob_usb.cpp ../ob_usb.h ../ob_follow.cpp ../ob_follow.h build/loop_ob_src/
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/loop_ob_src -Ishim_ob -I.. -I../../../core -I../../../teensy/libraries/RytmHost -o $@ test_loop_pi.cpp build/loop_ob_src/ob_usb.cpp build/loop_ob_src/ob_follow.cpp ../a4_in.cpp ../output.cpp ../loop_keep.cpp $(CORE)/rec/rec_names.cpp $(CORE)/device/digitakt.cpp $(CORE)/device/a4.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp -DOB_USB_TEST
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
# fifo_late.h with the measured FX guard: LT13 P5's two-box overload, the late mute within 20 ms (2026-11-25)
build/test_fifo_late_guard: test_fifo_late_guard.cpp ../fifo_late.h $(CORE)/fx/fx_guard.cpp $(CORE)/fx/fx_guard.h $(CORE)/fx/insert.cpp $(CORE)/fx/send_fx.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE)/fx/fx_guard.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/master.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# the send FX's Pi part with the core's (card-free) send FX
build/test_sendfx_pi: test_sendfx_pi.cpp ../sendfx_pi.cpp ../sendfx_pi.h ../spectrum_pi.cpp ../spectrum_pi.h ../spectrum.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/stutter_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../sendfx_pi.cpp ../spectrum_pi.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/master.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# the master FX: the OUT fill's crossfade, the ring to core 2 and back, bit for bit while bypassed, latency, cost
# the spectrum (spectrum.h: the tap, the FFT bands, the reader; spectrum_pi.cpp: the engine's taps) and its /ws topic, with
# the analysis's cost on the Mac (printed; the sanitizers make it slower than a plain -O2 build)
build/test_spectrum: test_spectrum.cpp ../spectrum.h ../spectrum_pi.cpp ../spectrum_pi.h ../ws_topics.h ../ws_session.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../spectrum_pi.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
build/test_masterfx_pi: test_masterfx_pi.cpp ../masterfx_pi.cpp ../masterfx_pi.h $(CORE)/fx/limiter.h $(CORE)/fx/master.cpp $(CORE)/fx/master.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../masterfx_pi.cpp $(CORE)/fx/master.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# two links, phase 3(d): slot 0's send FX and master chain bit for bit against the hash before the per-slot change
build/test_slotfx_ref: test_slotfx_ref.cpp ../sendfx_pi.cpp ../sendfx_pi.h ../masterfx_pi.cpp ../masterfx_pi.h ../spectrum_pi.cpp ../spectrum_pi.h $(CORE)/fx/master.cpp $(CORE)/fx/master.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../sendfx_pi.cpp ../masterfx_pi.cpp ../spectrum_pi.cpp $(CORE)/fx/master.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# ... on 16 voices (the Pi's): slot 1's instances beside slot 0's (SLOTFX_BOX1=1)
build/test_slotfx_ref16: test_slotfx_ref.cpp ../sendfx_pi.cpp ../sendfx_pi.h ../masterfx_pi.cpp ../masterfx_pi.h ../spectrum_pi.cpp ../spectrum_pi.h $(CORE)/fx/master.cpp $(CORE)/fx/master.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DCORE_VOICES=16 -o $@ $< ../sendfx_pi.cpp ../masterfx_pi.cpp ../spectrum_pi.cpp $(CORE)/fx/master.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
# the Rytm in either slot (attach order): its stems pace its slot's send FX and SRC MIX alike; a box's voices pace its own
build/test_rytm_slot1: test_rytm_slot1.cpp ../sendfx_pi.cpp ../sendfx_pi.h ../masterfx_pi.cpp ../masterfx_pi.h ../spectrum_pi.cpp ../spectrum_pi.h $(CORE)/fx/master.cpp $(CORE)/fx/master.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/insert.cpp $(CORE)/fx/insert.h $(CORE)/fx/amp.cpp $(CORE)/fx/amp.h $(CORE)/fx/out.cpp $(CORE)/fx/out.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DCORE_VOICES=16 -o $@ $< ../sendfx_pi.cpp ../masterfx_pi.cpp ../spectrum_pi.cpp $(CORE)/fx/master.cpp $(CORE)/fx/send_fx.cpp $(CORE)/fx/insert.cpp $(CORE)/fx/fx_guard.cpp $(CORE)/fx/amp.cpp $(CORE)/fx/out.cpp $(CORE)/fx/out_measure.cpp
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
# the web update (update.cpp, docs/web-update.md) on a temp folder: checks, trial, commit, revert, power cuts at every write
build/test_update: test_update.cpp ../update.cpp ../update.h ../fsweb.cpp ../fsweb.h ../webnext_path.h $(RMPSRC)/rmp_loader.cpp $(CORE)/lib/sha256.cpp ../../../tools/make_update.py check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFSWEB_HOST -I$(RMPSRC) -o $@ $< ../update.cpp ../fsweb.cpp $(RMPSRC)/rmp_loader.cpp $(CORE)/lib/sha256.cpp
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
# the INSERT FX / MASTER FX views' touch bases and defaults (insfx_pi.cpp) on the core's Mac library. insfx_pi.cpp's own
# core_port.h is the Pi's (sniprintf = the newlib subset): the copy built here leaves it out, the core's Mac port has sniprintf
build/test_insfx_pi: test_insfx_pi.cpp ../insfx_pi.cpp ../insfx_pi.h ../../ui/sidecar_insfx.h ../../ui/sidecar_mfx.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	sed '/#include "core_port.h"/d' ../insfx_pi.cpp > build/insfx_pi_mac.cpp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_POLY=1 -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_insfx_pi.cpp build/insfx_pi_mac.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# which VOICE knobs take a finger (voice_pi.cpp EditGate) on the core's Mac library; voice_pi.cpp's own core_port.h is the Pi's: left out
build/test_voice_gate: test_voice_gate.cpp ../voice_pi.cpp ../voice_pi.h ../../ui/sidecar_voice.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	sed '/#include "core_port.h"/d' ../voice_pi.cpp > build/voice_pi_mac.cpp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_POLY=1 -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. -I$(ENG)/Plaits -I$(ENG)/Juno -I$(ENG)/DaisySP -I$(ENG)/Sampler -I$(CORE)/../sdk -I$(RMPSRC) \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_voice_gate.cpp build/voice_pi_mac.cpp ../seditview_pi.cpp ../../ui/rytm_pattern.c $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# TBLB: the machine view's table B fields (wgview_pi.cpp Fill) on the core's Mac library. wgview_pi.cpp includes platform.h (the
# core's host/ one here); the sidecar_wgview_* state it asks the display for is the test's own (LVGL is not built here)
build/test_wgview_pi: test_wgview_pi.cpp ../wgview_pi.cpp ../wgview_pi.h ../../ui/sidecar_wgview.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_POLY=1 -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. -I$(ENG)/Plaits -I$(ENG)/Juno -I$(ENG)/DaisySP -I$(ENG)/Sampler -I$(CORE)/../sdk -I$(RMPSRC) \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_wgview_pi.cpp ../wgview_pi.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# 0.5.0: the display's LOOP page view (loopview_pi.cpp) from the looper (core/loop/loop.h) on the core's Mac library
build/test_loopview_pi: test_loopview_pi.cpp ../loopview_pi.cpp ../loopview_pi.h ../../ui/sidecar_loop.h $(CORE)/loop/loop.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_POLY=1 -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_loopview_pi.cpp ../loopview_pi.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# 0.5.0: the display's XY page view (xypad_pi.cpp) from the XY pad (core/mod/xypad.h) on the core's Mac library
build/test_xypad_pi: test_xypad_pi.cpp ../xypad_pi.cpp ../xypad_pi.h ../../ui/sidecar_xy.h $(CORE)/mod/xypad.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_POLY=1 -DCORE_MSAMP=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ test_xypad_pi.cpp ../xypad_pi.cpp $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
$(CORE)/build/libcore.dylib: $(wildcard $(CORE)/*.cpp $(CORE)/*/*.cpp $(CORE)/*.h $(CORE)/*/*.h)
	@test -f $(CORE)/build/libengines.a || { echo "bundleweb: run 'make -C core engines' first"; exit 1; }
	$(MAKE) -C $(CORE) lib
# touch2: the SCOPE page's provider (scope_pi.cpp) against a stand-in tap and selected track (the test's own)
build/test_scope_pi: test_scope_pi.cpp ../scope_pi.cpp ../scope_pi.h ../../ui/sidecar_scope.h check.h
	@mkdir -p build
	$(CXX) -I$(CORE)/host $(CXXFLAGS) -Wno-unused-parameter -o $@ $< ../scope_pi.cpp
# the storage layer (docs/STORAGE.md): the volumes' state machine and rules, the bench on a simulated drive and a temp folder
# the first boot's grow of the SD image (sdgrow.cpp) on in-memory disks laid out by sdimg_fixture.h
build/test_sdgrow: test_sdgrow.cpp ../sdgrow.cpp ../sdgrow.h sdimg_fixture.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../sdgrow.cpp
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
# gadget patch stack (pi/engine/build.sh's order, up to circle-gadget-unplug.patch), copied into build/dwc and patched
# there, on a fake register file (shim_dwc/)
DWC_ROOT := $(if $(filter %/circle,$(CIRCLE_BUILD)),$(CIRCLE_BUILD),$(CIRCLE_BUILD)/circle)
DWC_PATCHES := $(addprefix ../../circle/,circle-gadget.patch circle-gadget-wake.patch circle-gadget-reset.patch \
	       circle-gadget-mirror.patch circle-gadget-softconnect.patch circle-gadget-stray.patch \
	       circle-gadget-unplug.patch)
build/dwc/.patched: $(DWC_PATCHES) ../../circle/gadget_wake.h ../../circle/gadget_flush.h
	@test -f $(DWC_ROOT)/lib/usb/gadget/dwusbgadgetendpoint0.cpp || { echo "test_gadget_ep0: no Circle at $(DWC_ROOT) (CIRCLE_BUILD=.../pi/circle/build)"; exit 1; }
	@rm -rf build/dwc && mkdir -p build/dwc/lib/usb
	@cp -R $(DWC_ROOT)/include build/dwc/ && cp -R $(DWC_ROOT)/lib/usb/gadget build/dwc/lib/usb/
	@cp ../../circle/gadget_wake.h build/dwc/include/circle/usb/gadget/gadgetwake.h
	@cp ../../circle/gadget_flush.h build/dwc/include/circle/usb/gadget/gadgetflush.h
	@cd build/dwc && for p in $(DWC_PATCHES); do patch -s -p1 < ../../$$p || exit 1; done
	@touch $@
# the real OnTransferDropped bodies of CSidecarMIDIEP, CNcmEP and CMirrorEP (the text between the braces), included by the
# test's stand-ins (they name the members as the classes do): a changed body is compiled and run, a renamed one fails here
build/drop_midi.inc: DROPCLS := CSidecarMIDIEP
build/drop_ncm.inc: DROPCLS := CNcmEP
build/drop_mirror.inc: DROPCLS := CMirrorEP
build/drop_%.inc: ../sidecar_gadget.cpp
	@mkdir -p build
	@awk -v cls=$(DROPCLS) 'index($$0, "void " cls "::OnTransferDropped (boolean bIn)") == 1 {f=1; next} f && !s && /^\{$$/ {s=1; next} f && s && /^\}$$/ {exit} f && s {print}' ../sidecar_gadget.cpp > $@
	@test -s $@ || { echo "test_gadget_ep0: no $(DROPCLS)::OnTransferDropped in sidecar_gadget.cpp"; exit 1; }
build/test_gadget_ep0: test_gadget_ep0.cpp ../mirror_ep.h ../ep_stop.h ../sidecar_gadget.cpp build/dwc/.patched build/drop_midi.inc build/drop_ncm.inc build/drop_mirror.inc $(wildcard shim_dwc/*.h shim_dwc/circle/*.h shim_dwc/circle/usb/*.h shim_dwc/circle/usb/gadget/*.h) check.h
	@# the stand-ins stop an endpoint as the real classes do (the descriptor's number and direction): the statement is in the source
	@grep -q 'ep_stop::stop (\*this, GetEPNumber (), GetDirection () == DirectionIn);' ../sidecar_gadget.cpp || { echo "test_gadget_ep0: CSidecarMIDIEP::Stop changed (the stand-in CMidiTestEP follows it)"; exit 1; }
	@grep -q 'ep_stop::stop (\*this, m_nEP, m_Role != RoleOut);' ../sidecar_gadget.cpp || { echo "test_gadget_ep0: CNcmEP::Stop changed (CNcmTestEP follows it)"; exit 1; }
	@# the stop times are logged where a real bus suspend reaches them (LogStats), not in CSidecarGadget::OnSuspend (UpdatePlugAndPlay / SoftConnect only)
	@grep -q '^	LogSuspendStops (pLogger);' ../sidecar_gadget.cpp || { echo "test_gadget_ep0: LogStats does not call LogSuspendStops"; exit 1; }
	@awk '/^void CSidecarGadget::OnSuspend/ {f=1} f && /ep_stop::take/ {bad=1} f && /^}/ {f=0} END {exit bad}' ../sidecar_gadget.cpp || { echo "test_gadget_ep0: CSidecarGadget::OnSuspend takes the stop stats again (a bus suspend never calls it)"; exit 1; }
	@# the real dwusbgadget.cpp (the suspended-state acknowledgement of the OUT EP's EP disabled) compiles against the patched headers
	@grep -q 'DWHCI_DEV_OUT_EP_INT_EP_DISABLED);.*circle-gadget-unplug' build/dwc/lib/usb/gadget/dwusbgadget.cpp || { echo "test_gadget_ep0: dwusbgadget.cpp has no suspended-state EP disabled ack"; exit 1; }
	$(CXX) -std=c++17 -fsyntax-only -w -Ibuild/dwc/include build/dwc/lib/usb/gadget/dwusbgadget.cpp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -Ishim_dwc -Ibuild/dwc/include -I. -I.. -o $@ $< build/dwc/lib/usb/gadget/dwusbgadgetendpoint.cpp \
	  build/dwc/lib/usb/gadget/dwusbgadgetendpoint0.cpp
# the USB MIDI host's IN requests (test_midi_rxqueue.cpp, pi/circle/circle-midi-rxqueue.patch): Circle's usbmidihost.cpp with
# the engine's patch stack (pi/engine/build.sh's order), copied into build/umh and patched there; built for the xHCI
# (RASPPI 4: both buffers in flight, no pause after a zero-length packet) and the DWHCI (RASPPI 3: Circle's behaviour)
UMH_PATCHES := ../../circle/spike2/circle-xhci-urbqueue.patch $(addprefix ../../circle/,circle-midi-async.patch circle-xhci-dropurbs.patch \
	       circle-midi-reset.patch circle-midi-reset2.patch circle-usb-urb-free.patch circle-midi-async512.patch circle-midi-rxqueue.patch \
	       circle-midi-usbdevice.patch)
build/umh/.patched: $(UMH_PATCHES)
	@test -f $(DWC_ROOT)/lib/usb/usbmidihost.cpp || { echo "test_midi_rxqueue: no Circle at $(DWC_ROOT) (CIRCLE_BUILD=.../pi/circle/build)"; exit 1; }
	@rm -rf build/umh && mkdir -p build/umh/lib/usb
	@cp -R $(DWC_ROOT)/include build/umh/ && cp $(DWC_ROOT)/lib/usb/usbmidihost.cpp $(DWC_ROOT)/lib/usb/xhciendpoint.cpp $(DWC_ROOT)/lib/usb/usbmidi.cpp \
	  $(DWC_ROOT)/lib/usb/usbrequest.cpp build/umh/lib/usb/
	@# a patch already in the checkout (pi/circle/build.sh puts spike2's into pi/circle/build/circle) is not applied again
	@cd build/umh && for p in $(UMH_PATCHES); do patch -s -f -R -p1 --dry-run < ../../$$p >/dev/null 2>&1 || patch -s -p1 < ../../$$p || exit 1; done
	@touch $@
UMH_CXX = $(CXX) -std=c++17 -O1 -g -Wall -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -fno-exceptions -fno-rtti \
	  -DAARCH=64 -Ibuild/umh/include -I.
build/test_midi_rxqueue: test_midi_rxqueue.cpp build/umh/.patched check.h
	$(UMH_CXX) -DRASPPI=4 -o $@ $< build/umh/lib/usb/usbmidihost.cpp
	$(UMH_CXX) -DRASPPI=3 -o $@_dwhci $< build/umh/lib/usb/usbmidihost.cpp
	./$@_dwhci
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
# (bridge::tick calls the display's MORPH / XY / SONG pages' Tick: their portable halves linked too; test_content has
# its own song_pi::Tick)
BRIDGE_SRC := ../bridge.cpp ../morph_pi.cpp ../loopview_pi.cpp ../xypad_pi.cpp ../song_pi.cpp
build/test_content: test_content.cpp $(BRIDGE_SRC) ../bridge.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) $(CONTENT_FLAGS) -o $@ $< $(filter-out ../song_pi.cpp,$(BRIDGE_SRC)) $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
# bridge::keys_ui (Settings > MIDI on the touchscreen, 0.5.0): the same build as content's
build/test_keys_ui: test_keys_ui.cpp $(BRIDGE_SRC) ../bridge.h check.h $(CORE)/build/libcore.dylib
	@mkdir -p build
	$(CXX) $(CONTENT_FLAGS) -I.. -o $@ $< $(BRIDGE_SRC) $(CORE)/build/libcore.dylib -Wl,-rpath,$(abspath $(CORE)/build)
clean:
	rm -rf build
