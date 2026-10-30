# plugins/hush1/Makefile - HUSH1, an SH-101-style mono synth (bass, leads): HUSH ONE's DSP (Charles Vestal, MIT:
# LICENSE.hush1, NOTICE) as one voice a state (a mono track: the SH-101, with glide); a poly track plays chords on
# instances (manifest max_voices 8).
#   make a64        hush1.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make            hush1.rmp (the Teensy, Teensyduino's arm-none-eabi-gcc) and libhush1.dylib (the Mac)
#   make package    out/: hush1.a64.rmp, LICENSE.txt (ours and HUSH ONE's), NOTICE, SHA256SUMS, BUILD-INFO-plugins.txt
#   make check      tests/hush_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it (a bass line,
#                   a chord, rms, finite, guard bytes, release, ns a voice) and the manifest packed by tools/rmp_pack.py
#   make demo       libhush1.dylib, then tests/demo.py: a bass line, a glide lead, a filter sweep as a WAV (DEMO=path)
# MIT, Copyright 2026 trigcondition and contributors.
NAME := hush1
SRCS := hush1.c
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): the Mac's timing against sine_test (17 permille measured on the Teensy), make check's cpu line
HUSH_PERMILLE ?= 75
CFLAGS_EXTRA := -DHUSH_PERMILLE=$(HUSH_PERMILLE)
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- HUSH ONE (the DSP HUSH1 is built from) ----"; cat LICENSE.hush1; } >> out/LICENSE.txt
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
