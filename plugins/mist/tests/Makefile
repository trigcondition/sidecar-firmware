# plugins/mist/tests/Makefile - the host checks (mist_check.cpp). `make -C .. check` builds the package first.
# MIT, Copyright 2026 trigcondition and contributors.
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
B := ../build/check
$(B)/mist_check: mist_check.cpp $(RMPSRC)/rmp_loader.cpp
	@mkdir -p $(B)
	c++ -std=c++17 -O2 -Wall -I$(RMPSRC) -I../../../sdk -o $@ mist_check.cpp $(RMPSRC)/rmp_loader.cpp
# the CPU references: the native builds of sine_test (17 permille measured on the Teensy) and two starter machines
REFS := ../../sine_test/libsine_test.dylib ../../sc_acid/libsc_acid.dylib ../../sc_wavepad/libsc_wavepad.dylib
$(REFS):
	$(MAKE) -C $(dir $@) $(notdir $@)
run: $(B)/mist_check $(REFS)
	@mkdir -p ../../build-wav
	$(B)/mist_check ../out/mist.a64.rmp ../libmist.dylib $(REFS)
clean:
	rm -rf $(B)
.PHONY: run clean
