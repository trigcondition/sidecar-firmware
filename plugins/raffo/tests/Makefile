# plugins/raffo/tests/Makefile - the host checks (raffo_check.cpp). `make -C .. check` builds the package first.
# MIT, Copyright 2026 trigcondition and contributors.
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
B := ../build/check
$(B)/raffo_check: raffo_check.cpp $(RMPSRC)/rmp_loader.cpp
	@mkdir -p $(B)
	c++ -std=c++17 -O2 -Wall -I$(RMPSRC) -o $@ raffo_check.cpp $(RMPSRC)/rmp_loader.cpp
# the CPU references: the native builds of sine_test (17 permille measured on the Teensy) and two starter machines
REFS := ../../sine_test/libsine_test.dylib ../../sc_acid/libsc_acid.dylib ../../sc_wavepad/libsc_wavepad.dylib
$(REFS):
	$(MAKE) -C $(dir $@) $(notdir $@)
run: $(B)/raffo_check $(REFS)
	$(B)/raffo_check ../out/raffo.a64.rmp ../libraffo.dylib $(REFS)
clean:
	rm -rf $(B)
.PHONY: run clean
