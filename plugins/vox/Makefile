# plugins/vox/Makefile - VOX, a sung / spoken vocal tract user machine: Pink Trombone's DSP (Neil Thapen, MIT:
# LICENSE.pinktrombone, NOTICE) as one voice a state; a poly track plays chords on instances (manifest max_voices).
#   make a64        vox.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make            vox.rmp (the Teensy, Teensyduino's arm-none-eabi-gcc) and libvox.dylib (the Mac)
#   make package    out/: vox.a64.rmp, LICENSE.txt (ours and Pink Trombone's), NOTICE, SHA256SUMS, BUILD-INFO-plugins.txt
#   make check      tests/vox_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it (a chord,
#                   rms, finite, guard bytes, release, ns a voice) and the manifest packed by tools/rmp_pack.py
#   make demo       libvox.dylib, then tests/demo.py: sung vowels, a chord, closures, a glide as a WAV (DEMO=path)
# MIT, Copyright 2026 trigcondition and contributors.
NAME := vox
SRCS := vox.c
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): the Mac's timing against sine_test (17 permille measured on the Teensy), make check's cpu line
VOX_PERMILLE ?= 180
CFLAGS_EXTRA := -DVOX_PERMILLE=$(VOX_PERMILLE) -O3
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- Pink Trombone (the DSP VOX is built from) ----"; cat LICENSE.pinktrombone; } >> out/LICENSE.txt
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
