# plugins/mdkit/tests/Makefile - the host checks (mdk_check.cpp) against mdkit's own voices, built natively with
# mdkit's own flags (cc -std=c99 -O2, the unwindowed sources). `make -C .. check` builds the packages first.
# MIT, Copyright 2026 trigcondition and contributors.
MDK := ../../../third_party/mdkit
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
B := ../build/check
VSRC := $(sort $(wildcard $(MDK)/src/*/*/mdkit_*.c))
VOBJ := $(patsubst $(MDK)/src/%.c,$(B)/ref/%.o,$(VSRC))
$(B)/ref/%.o: $(MDK)/src/%.c
	@mkdir -p $(dir $@)
	cc -std=c99 -O2 -I$(MDK)/include -I$(dir $<) -c $< -o $@
$(B)/mdk_check: mdk_check.cpp ../mdk_tap.h $(RMPSRC)/rmp_loader.cpp $(VOBJ)
	c++ -std=c++17 -O2 -Wall -I$(RMPSRC) -I$(MDK)/include -o $@ mdk_check.cpp $(RMPSRC)/rmp_loader.cpp $(VOBJ)
$(B)/pi_window: pi_window.c $(B)/ref/pi/cc/mdkit_pi_cc.o $(B)/ref/pi/hh/mdkit_pi_hh.o $(B)/ref/pi/rc/mdkit_pi_rc.o
	cc -std=c99 -O2 -I$(MDK)/include $(addprefix -I$(MDK)/src/pi/,cc hh rc) -o $@ $^
FAMS ?= md pi efm trx
run: $(B)/mdk_check $(if $(filter pi md,$(FAMS)),$(B)/pi_window)
	$(B)/mdk_check ../out $(FAMS)
	$(if $(filter pi md,$(FAMS)),$(B)/pi_window 200)
clean:
	rm -rf $(B)
.PHONY: run clean
