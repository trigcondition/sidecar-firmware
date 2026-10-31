# pi/engine/Makefile - the Pi engine (docs/pi-engine.md): a Circle kernel (RASPPI=4, AArch64, multi-core) around the
# engine core (core/) and the Teensy's curated engines (engines/: core/engines/setup_engines.sh, git-ignored).
# Built by ./build.sh in Docker; flashed by CIRCLE_TREE=circle-engine ../circle/flash.sh ../engine.
CIRCLEHOME ?= ../circle/build/circle-engine
# the tree's RASPPI (4: circle-engine, kernel8-rpi4.img; 5: circle-engine-pi5, kernel_2712.img, docs/pi5.md), read here
# for the choices below; Rules.mk includes it again
-include $(CIRCLEHOME)/Config.mk
CORE := ../../core
ENG  := engines
include $(CORE)/engines/engines.mk

# MULTISAMPLE (core/msamp): the Circle side's platform_pi.cpp defines its arena (core/platform.h)
DEFINE += -DCORE_MSAMP=1
# Circle-side objects: Circle's flags and headers (Rules.mk's %.o: %.cpp)
OBJS = main.o kernel.o platform_pi.o shared.o ob_usb.o cores.o console.o selftest.o guard.o tcp_link.o $(GADGET_OBJS) web.o sdfiles.o um_pi.o wifi.o rec_pi.o
OBJS += paircode_pi.o          # the pairing code's Pi side (paircode_pi.h; the portable paircode.cpp is in PORT_SRC)
OBJS += midibridge_pi.o        # the Rytm MIDI bridge (midibridge_pi.h; the portable part is the header midibridge.h)
# the volumes besides the card and the storage bench (docs/STORAGE.md): Circle's side; the logic is hosted (PORT_SRC)
OBJS += stor_pi.o
OBJS += blackbox_pi.o          # the black box: the log to /.rytm/blackbox.log (blackbox.h)
# FatFs's disk layer in place of Circle's addon/fatfs/diskio.cpp (linked before libfatfs.a, so the archive's diskio.o
# is never pulled in): the USB drives' transfers kept to the 64 KB TRB rule below FatFs (stor_diskio.cpp)
OBJS += stor_diskio.o
ifeq ($(strip $(RASPPI)),5)
# Pi 5: Circle builds its DWC2 gadget for the Pi <= 4 only (lib/usb/gadget/Makefile; docs/pi5.md), so the USB-C Sidecar and
# P1's MIDI echo are compiled out (gadget.h, sidecar_gadget.h: stand-ins that are never started). The serial console:
# PI5_UART (build.sh, default 0: GPIO14/15, pins 8/10, as on the Pi 4; 10: the 3-pin debug connector, Circle's default)
DEFINE += -DSIDECAR_NO_GADGET -DSERIAL_DEVICE_DEFAULT=$(or $(PI5_UART),0)
else
GADGET_OBJS = gadget.o sidecar_gadget.o
endif
# the display (docs/pi-display.md; HDMI, DSI): pi/ui, LVGL from the tree's addon/lvgl (build.sh adds it)
OBJS += ../ui/sidecar_display.o ../ui/sidecar_ui.o ../ui/sidecar_splash.o ../ui/sidecar_wordmark.o
# the pattern pages (docs/TOUCH-EDITOR.md): the SEQ editor and the Rytm pattern view, LVGL only
OBJS += ../ui/sidecar_seqed.o ../ui/sidecar_rytmpat.o ../ui/rytm_pattern.o ../ui/sidecar_theme.o ../ui/sidecar_dot.o ../ui/sidecar_dotfont.o
# the WAVETABLE / GRANULAR machine view (pi/ui/sidecar_wgview.h), filled by wgview_pi.cpp
OBJS += ../ui/sidecar_wgview.o
# the CHOP page (pi/ui/sidecar_chop.h), filled by chopview_pi.cpp
OBJS += ../ui/sidecar_chop.o
# the touch calibration (pi/ui/touch_cal.h: Settings > Display > Calibrate)
OBJS += ../ui/touch_cal.o
# the VOICE page (pi/ui/sidecar_voice.h, rytm-mods rui1036b), filled by voice_pi.cpp
OBJS += ../ui/sidecar_voice.o
# the SONG page (pi/ui/sidecar_song.h, core/song), filled by song_pi.cpp
OBJS += ../ui/sidecar_song.o
# the INSERT FX page (pi/ui/sidecar_insfx.h, fx-ins), filled by insfx_pi.cpp
OBJS += ../ui/sidecar_insfx.o
# the REC page (pi/ui/sidecar_rec.h, the Overbridge recorder), filled by rec_pi.cpp
OBJS += ../ui/sidecar_rec.o
# touch2: the SCOPE page (pi/ui/sidecar_scope.h, scope_pi.cpp) and the SIDECAR STATUS page (sidecar_status.h, status_pi.cpp)
OBJS += ../ui/sidecar_scope.o ../ui/sidecar_status.o
# bundles phase 3: the PROJECT page (pi/ui/sidecar_project.h; its view from core/bundle/feed via bundle_pi.cpp)
OBJS += ../ui/sidecar_project.o

# Hosted objects: the toolchain's C++ headers and newlib's libm, not Circle's (as spike3's Plaits): the core,
# the engines and the engine's portable parts. Named *.cpp.o / *.cc.o / *.c.o under hosted/.
# core/host/newlib_rand.cpp: DAISYSP's rand() with newlib's sequence, without newlib's libc (the one host file here).
CORE_SRC := $(addprefix $(CORE)/,md/md.cpp project_key.cpp lfo.cpp hooks.cpp notes.cpp voices.cpp poly.cpp inst.cpp inst_alloc.cpp keys.cpp sysex.cpp host_link.cpp slots.cpp engine.cpp fx/send_fx.cpp fx/send_fx_card.cpp fx/insert.cpp fx/insert_card.cpp fx/amp.cpp fx/out.cpp fx/out_measure.cpp \
            ui/canvas.cpp ui/widgets.cpp ui/pages.cpp ui/remote.cpp \
            pool/card_index.cpp pool/pool.cpp pool/pcache.cpp pool/sample_ram_pool.cpp pool/stream_budget.cpp presets/libs.cpp msamp/sfz.cpp msamp/msamp.cpp msamp/sdtest.cpp wavetable/tables.cpp wavetable/wavetable.cpp granular/granular.cpp livein/livein.cpp chop/chop.cpp chop/slicemap.cpp host/newlib_rand.cpp user/um.cpp user/lists.cpp)
CORE_SRC += $(wildcard $(CORE)/ui/page_*.cpp)
CORE_SRC += $(wildcard $(CORE)/seq/*.cpp)   # the Sidecar sequencer (core/seq/seq.h)
CORE_SRC += $(wildcard $(CORE)/song/*.cpp)  # SONG RECORD (core/song/song.h)
CORE_SRC += $(wildcard $(CORE)/bundle/*.cpp)   # project bundles (core/bundle, docs/BUNDLES.md)
CORE_SRC += $(wildcard $(CORE)/lib/*.cpp) $(wildcard $(CORE)/fmt/*.cpp)   # the source browser and pools (srclib), the sample library and sha256 (docs/LIBRARY.md), the format registry (core/fmt)
CORE_SRC += $(wildcard $(CORE)/store/*.cpp)   # the RAM store: one project's card files in RAM, filled by the card core (core/store/store.h)
CORE_SRC += $(CORE)/rec/rec_names.cpp   # the recorder's sources, channel lists and take names (shared with the Sidecar Mini)
CORE_SRC += $(CORE)/msamp/cardq.cpp $(wildcard $(CORE)/samp/*.cpp)   # the card queue (core/msamp/cardq.h); sampling, auto-sampler (core/samp)
# the plugin loader's portable parts (teensy/libraries/RytmPlugins/src: verify, place, manifest, generations, the slot
# messages, the slot map), hosted: user machines on the Pi (core/user/um.cpp, docs/user-machines-pi.md)
RMPSRC := ../../teensy/libraries/RytmPlugins/src
RMP_SRC := rmp_loader.cpp rmp_manifest.cpp rmp_meta.cpp rmp_proto.cpp rmp_resolve.cpp rmp_slots.cpp rmp_presets.cpp rmp_pool.cpp
PORT_SRC := output.cpp midi_in.cpp bridge.cpp emu_proto.cpp tones.cpp sendfx_pi.cpp fsweb.cpp bundleweb.cpp poolweb.cpp bundle_pi.cpp recorder.cpp rec_play.cpp wifi_conf.cpp paircode.cpp power_state.cpp wgview_pi.cpp chopview_pi.cpp voice_pi.cpp insfx_pi.cpp song_pi.cpp \
            scope_pi.cpp status_pi.cpp stor_vol.cpp stor_bench.cpp lib_pi.cpp
# the SEQ page's engine side (seqed_pi.cpp, ../ui/seq_touch.cpp): only with the sequencer's engine in the tree
# (core/seq/seq.cpp, the seq-mvp branch); without it the page says there is no sequencer
SEQED := $(if $(wildcard $(CORE)/seq/seq.cpp),1,)
ifeq ($(SEQED),1)
PORT_SRC += seqed_pi.cpp rytmpat_pi.cpp   # rytmpat_pi: the Rytm pattern view's data (core/seq/rytm_dump.h)
DEFINE += -DSIDECAR_SEQED=1
endif
HOSTED_OBJS := $(patsubst $(CORE)/%,hosted/core/%.o,$(CORE_SRC)) $(patsubst %,hosted/%.o,$(PORT_SRC)) \
               $(patsubst %,hosted/rmp/%.o,$(RMP_SRC)) $(if $(SEQED),hosted/ui/seq_touch.cpp.o,) \
               $(patsubst %,hosted/%.o,$(ENG_SRC) $(ENG_C_SRC))
OBJS += $(HOSTED_OBJS)

EXTRAINCLUDE += -I . -I ../../teensy/libraries/RytmHost -I $(CIRCLEHOME)/addon -I $(CORE) -I $(RMPSRC)
EXTRAINCLUDE += -I ../ui -I $(CIRCLEHOME)/addon/lvgl/lvgl

# Pi 5: the DSI display (addon/rp1dsi, build.sh builds it), before libinput: both define a CRPiTouchScreen (rp1dsi's
# is the display; libinput's, the firmware's touch buffer, is the Pi 4's), the first archive that resolves it is used
LIBS = $(if $(filter 5,$(strip $(RASPPI))),$(CIRCLEHOME)/addon/rp1dsi/librp1dsi.a,) \
       $(CIRCLEHOME)/addon/lvgl/liblvgl.a \
       $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
       $(CIRCLEHOME)/addon/wlan/libwlan.a \
       $(if $(filter 5,$(strip $(RASPPI))),$(CIRCLEHOME)/addon/nvme/libnvme.a,) \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/net/libnet.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(if $(filter 5,$(strip $(RASPPI))),,$(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a) \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

# RYTM_RENDER_CORES=4: the render state voices share is per core (cores 1 and 2 render at once; rytm_render_core() in
# platform_pi.cpp): a 6-OP FM reload's stage and bank pointer, GRAIN's budget, stmlib's Random and rand(). Every
# hosted object needs the same value (the per-core arrays are inline functions' statics).
HOSTED_FLAGS = $(ARCH) -DRYTM_RENDER_CORES=4 -DCORE_POLY=1 -DCORE_MSAMP=1 -O2 -std=gnu++17 -fsigned-char -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-math-errno -ffp-contract=fast -ffunction-sections -fdata-sections -g -MMD -MP -I. -I$(CORE) -I$(RMPSRC) -DRMP_HOST_A64 $(ENG_INC) $(ENG_FLAGS) \
	$(if $(SEQED),-DSIDECAR_SEQED=1,)
HOSTED_CFLAGS = $(ARCH) -O2 -std=c99 -fno-math-errno -ffp-contract=fast -g -MMD -MP -w $(ENG_INC)

hosted/core/%.o: $(CORE)/%
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<
hosted/rmp/%.cpp.o: $(RMPSRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -include core_port.h -DRMP_SNPRINTF=sniprintf -c -o $@ $<
hosted/ui/%.cpp.o: ../ui/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<
hosted/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<
hosted/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -w -c -o $@ $<
hosted/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $@ (hosted)"
	@$(CC) $(HOSTED_CFLAGS) -c -o $@ $<

EXTRACLEAN += -r hosted

# Circle's objects: Rules.mk's %.d rules make their .d files when included (as spike3). Hosted objects: -MMD -MP
# writes hosted/**/*.d beside them during the compile; include the ones that exist.
-include $(filter-out hosted/%,$(DEPS))
-include $(wildcard $(HOSTED_OBJS:.o=.d))
