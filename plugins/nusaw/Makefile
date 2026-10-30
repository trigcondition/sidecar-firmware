# plugins/nusaw/Makefile - NUSAW, a supersaw user machine for pads and stabs: NuSaw's DSP (Charles Vestal, MIT:
# LICENSE.nusaw, NOTICE) as one voice a state; a poly track plays chords on instances (manifest max_voices 8).
#   make a64        nusaw.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make            nusaw.rmp (the Teensy, Teensyduino's arm-none-eabi-gcc) and libnusaw.dylib (the Mac)
#   make package    out/: nusaw.a64.rmp, LICENSE.txt (ours and NuSaw's), NOTICE, SHA256SUMS, BUILD-INFO-plugins.txt
#   make check      tests/nsaw_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it (a chord,
#                   rms, finite, guard bytes, release, ns a voice) and the manifest packed by tools/rmp_pack.py
#   make demo       libnusaw.dylib, then tests/demo.py: a pad and a stab line as a WAV (DEMO=path)
# MIT, Copyright 2026 trigcondition and contributors.
NAME := nusaw
SRCS := nusaw.c
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): the Mac's timing against sine_test (17 permille measured on the Teensy), make check's cpu line
NSAW_PERMILLE ?= 55
CFLAGS_EXTRA := -DNSAW_PERMILLE=$(NSAW_PERMILLE)
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- NuSaw (the DSP NUSAW is built from) ----"; cat LICENSE.nusaw; } >> out/LICENSE.txt
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
