# plugins/raffo/Makefile - RAFFO, a 4-oscillator mono synth with a ladder lowpass (basses, leads): RaffoSynth's DSP
# (Nicolas Roulet, Julian Palladino, MIT: LICENSE.raffo, NOTICE) as one voice a state; legato glide on a mono track,
# chords on a poly track's instances (manifest max_voices 8).
#   make a64        raffo.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make            raffo.rmp (the Teensy, Teensyduino's arm-none-eabi-gcc) and libraffo.dylib (the Mac)
#   make package    out/: raffo.a64.rmp, LICENSE.txt (ours and RaffoSynth's), NOTICE, SHA256SUMS, BUILD-INFO-plugins.txt
#   make check      tests/raffo_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it (a chord,
#                   rms, finite, guard bytes, release silence, legato glide, retrigger, ns a voice)
#   make demo       libraffo.dylib, then tests/demo.py: a bassline, a gliding lead and a filter sweep (DEMO=path)
# MIT, Copyright 2026 trigcondition and contributors.
NAME := raffo
SRCS := raffo.c
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): the Mac's timing against sine_test (17 permille measured on the Teensy), make check's cpu line
RAFFO_PERMILLE ?= 70
CFLAGS_EXTRA := -DRAFFO_PERMILLE=$(RAFFO_PERMILLE)
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- RaffoSynth (the DSP RAFFO is built from) ----"; cat LICENSE.raffo; } >> out/LICENSE.txt
	cp NOTICE out/NOTICE.txt
	cd out && shasum -a 256 *.rmp > SHA256SUMS
	echo "source commit: $$(git rev-parse HEAD)$$(git diff --quiet HEAD -- . || echo -dirty)" > out/BUILD-INFO-plugins.txt
	@ls -l out
check: package lib$(NAME).dylib
	$(MAKE) -C tests run
demo: lib$(NAME).dylib
	$(PY) tests/demo.py $(DEMO)
clean-all: clean
	rm -rf out
	$(MAKE) -C tests clean
.PHONY: package check demo clean-all
