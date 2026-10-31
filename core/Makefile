# core/Makefile - the engine core on the Mac (docs/core.md).
#   make -C core engines   once, and after the Teensy's engine libraries change: the curated engine sources into
#                          build/engines (engines/setup_engines.sh) and build/libengines.a
#   make -C core test      build and run every tests/test_*.cpp, tests/mc/test_*.cpp (two render cores: the
#                          engines and the core again with RYTM_RENDER_CORES=3, in build/mc) and tests/small/test_*.cpp
#                          (the Teensy's send FX set, RYTM_SENDFX_LARGE=0, in build/small); the last line says
#                          ALL CORE TESTS PASS
#   make -C core lib       build/libcore.dylib for Python (sim/coreengine.py, sim/tests/test_core_*.py)
# version.h (VERSION, the commit): regenerated when it changed; kept out of DEPS_H so a new commit does not rebuild every test
$(shell ../tools/gen_version.sh)
CXX      ?= c++
CC       ?= cc
B        := build
ENG      := $(B)/engines
include engines/engines.mk
RMPSRC   := ../teensy/libraries/RytmPlugins/src
CXXFLAGS := -std=c++17 -O2 -DCORE_UI_TOOLS=1 -DCORE_POLY=1 -DCORE_MSAMP=1 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -fPIC -I. -Ihost -I$(RMPSRC) -I../sdk $(ENG_INC) $(ENG_FLAGS)
CFLAGS   := -std=c99 -O2 -g -ffp-contract=fast -fPIC -w $(ENG_INC)
# The core's own sources (grows task by task) and the Mac port.
CORE_SRC := engine.cpp project_key.cpp md/md.cpp $(wildcard bundle/*.cpp) lfo.cpp hooks.cpp notes.cpp voices.cpp poly.cpp inst.cpp inst_alloc.cpp keys.cpp sysex.cpp host_link.cpp slots.cpp fx/send_fx.cpp fx/send_fx_card.cpp fx/insert.cpp fx/insert_card.cpp fx/amp.cpp fx/out.cpp fx/out_measure.cpp ui/canvas.cpp ui/widgets.cpp ui/pages.cpp ui/remote.cpp $(wildcard ui/page_*.cpp) $(wildcard seq/*.cpp) $(wildcard song/*.cpp) $(wildcard rec/*.cpp) pool/card_index.cpp pool/pool.cpp pool/pcache.cpp pool/sample_ram_pool.cpp pool/stream_budget.cpp presets/libs.cpp msamp/sfz.cpp msamp/msamp.cpp msamp/sdtest.cpp msamp/cardq.cpp $(wildcard samp/*.cpp) $(wildcard wavetable/*.cpp) $(wildcard granular/*.cpp) livein/livein.cpp $(wildcard chop/*.cpp) $(wildcard lib/*.cpp) $(wildcard fmt/*.cpp) $(wildcard store/*.cpp) host/platform_host.cpp host/newlib_rand.cpp \
            user/um.cpp user/lists.cpp host/um_host.cpp $(addprefix $(RMPSRC)/,rmp_loader.cpp rmp_manifest.cpp rmp_meta.cpp rmp_proto.cpp rmp_resolve.cpp rmp_slots.cpp rmp_presets.cpp rmp_pool.cpp)
TESTS    := $(patsubst tests/%.cpp,$(B)/%,$(wildcard tests/test_*.cpp))
ENG_OBJ  := $(patsubst $(ENG)/%,$(B)/eng-obj/%.o,$(ENG_SRC) $(ENG_C_SRC))
DEPS_H   := $(filter-out version.h,$(wildcard *.h */*.h host/*.h tests/*.h))
# Two render cores at once (the Pi engine's split) and a control core: engines and core built with RYTM_RENDER_CORES=3
MC       := $(B)/mc
MC_FLAGS := -DRYTM_RENDER_CORES=3
MC_OBJ   := $(patsubst $(ENG)/%,$(MC)/eng-obj/%.o,$(ENG_SRC) $(ENG_C_SRC))

.PHONY: engines test lib clean demo-wavegrain demo-insfx demo-tape demo-amp demo-livein demo-chop bundle-tool fuzz-fmt
# the format registry's fuzz target (tests/fuzz_fmt.cpp; docs/LIBRARY.md "Tests"): libFuzzer + ASan + UBSan with
# Homebrew's clang (FUZZ_CXX; brew install llvm) for FUZZ_SECS seconds (default 60) from the golden files; without it
# the same target under ASan + UBSan with its own driver (FUZZ_RUNS mutated inputs, default 200000)
FUZZ_CXX ?= /opt/homebrew/opt/llvm/bin/clang++
FUZZ_LLVM := $(patsubst %/bin/clang++,%,$(FUZZ_CXX))
FUZZ_SRC := tests/fuzz_fmt.cpp $(wildcard fmt/*.cpp) chop/slicemap.cpp lib/sha256.cpp lib/index_check.cpp lib/ref.cpp lib/pools_fmt.cpp seq/pattern.cpp song/json.cpp song/model.cpp \
            host/platform_host.cpp $(addprefix $(RMPSRC)/,rmp_slots.cpp rmp_resolve.cpp rmp_loader.cpp rmp_meta.cpp rmp_proto.cpp)
FUZZ_FLAGS := -std=c++17 -O1 -g -fno-sanitize-recover=all -DCORE_MSAMP=1 -DCORE_POLY=1 -I. -Ihost -I$(RMPSRC) $(ENG_INC) -Wl,-dead_strip
fuzz-fmt:
	@mkdir -p $(B)/fuzz/corpus
	@find tests/golden -type f -path '*/v*/*' -exec cp {} $(B)/fuzz/corpus/ \;
	@if [ -x "$(FUZZ_CXX)" ]; then \
	  $(FUZZ_CXX) $(FUZZ_FLAGS) -fsanitize=fuzzer,address,undefined -L$(FUZZ_LLVM)/lib/c++ -Wl,-rpath,$(FUZZ_LLVM)/lib/c++ -o $(B)/fuzz/fuzz_fmt $(FUZZ_SRC) && \
	  ./$(B)/fuzz/fuzz_fmt -max_total_time=$(or $(FUZZ_SECS),60) -max_len=65536 $(B)/fuzz/corpus; \
	else \
	  $(CXX) $(FUZZ_FLAGS) -fsanitize=address,undefined -DFUZZ_STANDALONE -o $(B)/fuzz/fuzz_fmt $(FUZZ_SRC) && \
	  ./$(B)/fuzz/fuzz_fmt tests/golden $(or $(FUZZ_RUNS),200000); \
	fi
# WAVETABLE's and GRANULAR's demo renders (tests/demo/render_wavegrain.cpp) into OUT (default build/)
demo-wavegrain: $(B)/render_wavegrain
	./$(B)/render_wavegrain $(or $(OUT),$(B))
$(B)/render_wavegrain: tests/demo/render_wavegrain.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a
# the INSERT effects' and FX buses' demo (tests/demo/render_insfx.cpp) into OUT (default build/): insfx_demo.wav, the
# remote INSERT page as PPMs, the chains' CPU
demo-insfx: $(B)/render_insfx
	./$(B)/render_insfx $(or $(OUT),$(B))
$(B)/render_insfx: tests/demo/render_insfx.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a
# SPACE's (send FX TYPE 5, fx/tape_fx.h) demo (tests/demo/render_tape.cpp) into OUT (default build/): space_demo.wav
demo-tape: $(B)/render_tape
	./$(B)/render_tape $(or $(OUT),$(B))
$(B)/render_tape: tests/demo/render_tape.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a
# INSERT AMP's (core/fx/amp.h) demos (tests/demo/render_amp.cpp) into OUT (default build/): amp_demo.wav (a guitar-like
# line) and amp_kick_demo.wav (a 909-style kick -> AMP -> SPACE); capture A SYNTHETIC (not an amp) unless MODEL names a
# real A2-Lite .namb / .nam
demo-amp: $(B)/render_amp
	./$(B)/render_amp $(or $(OUT),$(B)) $(MODEL)
$(B)/render_amp: tests/demo/render_amp.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a
# LIVE IN's demo (tests/demo/render_livein.cpp) into OUT (default build/): livein_demo.wav, a synthetic guitar on AUDIO
# IN -> LIVE IN -> INSERT AMP (MODEL: a real A2-Lite .nam / .namb, else a SYNTHETIC capture) -> SPACE
demo-livein: $(B)/render_livein
	./$(B)/render_livein $(or $(OUT),$(B)) $(MODEL)
$(B)/render_livein: tests/demo/render_livein.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a
# CHOP's demo (tests/demo/render_chop.cpp) into OUT (default build/): chop_demo.wav, a synthetic break chopped by LAZY,
# then replayed in a new order, SHUFFLE, STUTTER, TAPE STOP / SPIN-UP, MORPH
demo-chop: $(B)/render_chop
	./$(B)/render_chop $(or $(OUT),$(B))
$(B)/render_chop: tests/demo/render_chop.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a
# the bundle job on a Mac over a card folder (tests/demo/bundle_tool.cpp; docs/BUNDLES.md "On a Mac")
bundle-tool: $(B)/bundle_tool
$(B)/bundle_tool: tests/demo/bundle_tool.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE_SRC) $(B)/libengines.a
engines:
	engines/setup_engines.sh $(abspath $(ENG))
	$(MAKE) $(B)/libengines.a

$(B)/libengines.a: $(ENG_OBJ)
	@test -d $(ENG)/Plaits || { echo "core: run 'make -C core engines' first"; exit 1; }
	rm -f $@ && ar rcs $@ $^

$(B)/eng-obj/%.cc.o: $(ENG)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<
$(B)/eng-obj/%.cpp.o: $(ENG)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<
$(MC)/eng-obj/%.cc.o: $(ENG)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MC_FLAGS) -w -c -o $@ $<
$(MC)/eng-obj/%.cpp.o: $(ENG)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MC_FLAGS) -w -c -o $@ $<
$(MC)/eng-obj/%.c.o: $(ENG)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(MC)/libengines.a: $(MC_OBJ)
	@test -d $(ENG)/Plaits || { echo "core: run 'make -C core engines' first"; exit 1; }
	rm -f $@ && ar rcs $@ $^
MC_TESTS := $(patsubst tests/mc/%.cpp,$(MC)/%,$(wildcard tests/mc/test_*.cpp))
$(MC)/test_%: tests/mc/test_%.cpp $(CORE_SRC) $(DEPS_H) $(MC)/libengines.a
	$(CXX) $(CXXFLAGS) $(MC_FLAGS) -pthread -o $@ $< $(CORE_SRC) $(MC)/libengines.a

$(B)/eng-obj/%.c.o: $(ENG)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The Teensy's send FX set (RYTM_SENDFX_LARGE=0: CHORUS COMB CRUSH RINGMOD, 16-bit lines): tests/small, with the core
SMALL       := $(B)/small
SMALL_FLAGS := -DRYTM_SENDFX_LARGE=0
SMALL_TESTS := $(patsubst tests/small/%.cpp,$(SMALL)/%,$(wildcard tests/small/test_*.cpp))
$(SMALL)/test_%: tests/small/test_%.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	@mkdir -p $(SMALL)
	$(CXX) $(CXXFLAGS) $(SMALL_FLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a

# Every test run is its own target, so `make -j10 test` runs them side by side (2026-10-26: the one-after-another loop
# took ~45 min with the builds; every test works in its own mkdtemp folder). A run's output goes to <test>.log and is
# shown when the test fails; test-serial keeps the old ordered run with all output.
ALL_TESTS := $(TESTS) $(MC_TESTS) $(SMALL_TESTS)
# The tests that check a wall-clock bound (a control pass under the FIFO, a call under 50 us): run one at a time after
# the parallel batch, when nothing else loads the CPU (under -j10 test_library_stall measured a 2142 us pass, 1835 alone)
TIMING_TESTS := $(addprefix $(B)/,test_bundle_stall test_library_stall test_loadstall test_insert test_library test_tape \
                  test_send_fx test_srclib test_seqtop test_noteedit test_stereo test_msamp_load)
# The Teensy's configuration (no CORE_POLY, no CORE_MSAMP: no built-in MD), compiled for syntax only on the files the
# MD change touched that build there today (voices.cpp, ui/remote.cpp, lfo.cpp and a few more already fail in that
# configuration on integ-1028, unrelated: not listed)
TEENSY_CFG_SRC := md/md.cpp inst.cpp inst_alloc.cpp host_link.cpp notes.cpp ui/page_lfo.cpp livein/livein.cpp
teensy-cfg:
	@for f in $(TEENSY_CFG_SRC); do $(CXX) -std=c++17 -fsyntax-only -Wall -Wno-unused-parameter -I. -Ihost -I$(RMPSRC) -I../sdk $(ENG_INC) $(ENG_FLAGS) $$f || exit 1; done
	@echo "teensy configuration: $(TEENSY_CFG_SRC) compile"
.PHONY: teensy-cfg
test: teensy-cfg $(patsubst %,%.run,$(filter-out $(TIMING_TESTS),$(ALL_TESTS))) $(TIMING_TESTS)
	@set -e; for t in $(TIMING_TESTS); do ./$$t > $$t.log 2>&1 || { cat $$t.log; echo "FAILED: $$t"; exit 1; }; done
	@echo "ALL CORE TESTS PASS"
%.run: %
	@./$< > $<.log 2>&1 || { cat $<.log; echo "FAILED: $<"; exit 1; }
test-serial: $(ALL_TESTS)
	@set -e; for t in $(ALL_TESTS); do ./$$t; done; echo "ALL CORE TESTS PASS"
.PHONY: test-serial

$(B)/test_%: tests/test_%.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE_SRC) $(B)/libengines.a

lib: $(B)/libcore.dylib
$(B)/libcore.dylib: host/core_c.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -shared -install_name @rpath/libcore.dylib -o $@ host/core_c.cpp $(CORE_SRC) $(B)/libengines.a

clean:
	rm -rf $(B)
