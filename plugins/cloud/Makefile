# plugins/cloud/Makefile - CLOUD, a texture machine for the Pi: Beads' granular cloud and wavetable grains (Emilie
# Gillet, MIT: third_party/eurorack, LICENSE.beads, NOTICE) over a built-in oscillator. Plugin API 3 only (the buffer
# lives in the voice's arena): the Pi, and the arm64 Mac's core lib; no Teensy package.
#   make a64        cloud.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make native     libcloud.dylib (the Mac: the CPU comparison in make check)
#   make package    out/: cloud.a64.rmp, LICENSE.txt (ours, Beads' and stmlib's), NOTICE.txt, SHA256SUMS,
#                   BUILD-INFO-plugins.txt
#   make check      tests/cloud_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it (API 3:
#                   the arena, stereo), WAVs in ../build-wav/cloud-*.wav, the CPU per block, the control calls' time
# MIT, Copyright 2026 trigcondition and contributors.
NAME := cloud
SRCS := cloud.cc cloud_engine.cc delay_clock.cc delay_controller.cc mu_law.cc trigger_generator.cc resources.cc \
  units.cc random.cc
EU := third_party/eurorack
vpath %.cc $(EU)/beads/dsp $(EU)/beads $(EU)/stmlib/dsp $(EU)/stmlib/utils
INCS := -I$(EU) -I.
# TEST: stmlib's portable code (no Cortex-M ssat / vsqrt asm); BEADS_DELAY_LINE_NO_CLEAR_ON_INIT: VENDORED.md
# declared CPU per voice (per mille of a Teensy voice; informational on the Pi): make check's cpu line
CLOUD_PERMILLE ?= 700
CFLAGS_EXTRA := -DTEST -DBEADS_DELAY_LINE_NO_CLEAR_ON_INIT -DCLOUD_PERMILLE=$(CLOUD_PERMILLE) -Wno-vla-extension \
  -Wno-unknown-warning-option
.DEFAULT_GOAL := native
include ../../sdk/rmp/plugin.mk
# GCC must not turn the buffer's clear loop back into a call of the plugin libc's byte-wise memset
A64FLAGS += -fno-tree-loop-distribute-patterns
$(OBJS) $(NOBJS) $(AOBJS): Makefile cloud_engine.h

native: lib$(NAME).dylib

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- Beads (the DSP CLOUD is built from) ----"; cat LICENSE.beads; } >> out/LICENSE.txt
	{ echo; echo "---- stmlib (Beads' DSP library) ----"; cat $(EU)/stmlib/LICENSE; } >> out/LICENSE.txt
	cp NOTICE out/NOTICE.txt
	cd out && shasum -a 256 *.rmp > SHA256SUMS
	echo "source commit: $$(git rev-parse HEAD)$$(git diff --quiet HEAD -- . || echo -dirty)" > out/BUILD-INFO-plugins.txt
	@ls -l out
check: package native
	$(MAKE) -C tests run
clean-all: clean
	rm -rf out
	$(MAKE) -C tests clean
.PHONY: native package check clean-all
