# pi/engine/tests/Makefile - the Pi engine's portable parts on the Mac: make -C pi/engine/tests (shim/: the few
# Circle headers guard.h needs). make -C pi/engine/tests content: bridge.cpp's self-test content (test_content.cpp)
# against the core's Mac port (make -C core engines first)
$(shell ../../../tools/gen_version.sh)
CXX ?= c++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -I.. -I../../../core -Ishim -fsanitize=address,undefined -fno-sanitize-recover=all
SRC := ../output.cpp ../midi_in.cpp ../emu_proto.cpp
TESTS := $(patsubst %.cpp,build/%,$(filter-out test_content.cpp,$(wildcard test_*.cpp)))
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
CORE := ../../../core
ENG := $(CORE)/build/engines
include $(CORE)/engines/engines.mk
CORE_SRC := $(addprefix $(CORE)/,engine.cpp lfo.cpp hooks.cpp notes.cpp voices.cpp sysex.cpp host_link.cpp slots.cpp pool/card_index.cpp \
            pool/pool.cpp pool/sample_ram_pool.cpp presets/libs.cpp host/platform_host.cpp host/newlib_rand.cpp)
.PHONY: all clean content
all: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; echo "ALL PI PORTABLE TESTS PASS"
build/%: %.cpp $(SRC) $(wildcard ../*.h) $(wildcard shim/circle/*.h) check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)
# the send FX's Pi part with the core's (card-free) send FX
build/test_sendfx_pi: test_sendfx_pi.cpp ../sendfx_pi.cpp ../sendfx_pi.h $(CORE)/fx/send_fx.cpp $(CORE)/fx/send_fx.h $(CORE)/fx/stutter_fx.h ../spsc.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../sendfx_pi.cpp $(CORE)/fx/send_fx.cpp
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
build/test_fsweb: test_fsweb.cpp ../fsweb.cpp ../fsweb.h $(RMPSRC)/rmp_loader.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFSWEB_HOST -I$(RMPSRC) -o $@ $< ../fsweb.cpp $(RMPSRC)/rmp_loader.cpp
# the power supply's state (power_state.cpp): get_throttled and the Pi 5's max_current to the warning and /status
build/test_power_state: test_power_state.cpp ../power_state.cpp ../power_state.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../power_state.cpp
# the Wi-Fi settings (wifi_conf.cpp) on a temp folder
build/test_wifi_conf: test_wifi_conf.cpp ../wifi_conf.cpp ../wifi_conf.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../wifi_conf.cpp
content: build/test_content
	./build/test_content
# the engines' headers too (core/build/engines: make -C core engines copies them, e.g. sim/plaits/fm_machine.h)
build/test_content: test_content.cpp ../bridge.cpp $(CORE_SRC) $(wildcard $(CORE)/*.h $(CORE)/*/*.h) check.h \
  $(shell find $(CORE)/build/engines \( -name '*.h' -o -name '*.hpp' \) 2>/dev/null)
	@test -f $(CORE)/build/libengines.a || { echo "content: run 'make -C core engines' first"; exit 1; }
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -I$(CORE) -I$(CORE)/host \
	  $(ENG_INC) $(ENG_FLAGS) -o $@ $< ../bridge.cpp $(CORE_SRC) $(CORE)/build/libengines.a
clean:
	rm -rf build
