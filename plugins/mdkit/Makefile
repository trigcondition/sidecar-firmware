# plugins/mdkit/Makefile - mdkit's drum voices (https://github.com/soejrd/mdkit, CC0 1.0) as three user machines for
# the Pi (RMPA): MD, all three families in one machine (P9 ENGINE, P10 TYPE), and MD-PI, MD-EFM, MD-TRX, one per
# family (README.md). mdkit: third_party/mdkit (vendored at 96157fe).
#   make FAM=md a64         mdk_md.a64.rmp (FAM md, pi, efm or trx; aarch64-none-elf-gcc from PATH or the rytm-circle image)
#   make packages           the four, copied to out/ (mdk.rmp - MD -, mdk_pi.rmp, mdk_efm.rmp, mdk_trx.rmp: what goes
#                           into /Plugins)
#   make check              the host checks (tests/mdk_check.cpp: the RMPA packages run on this arm64 Mac)
# Pi only: a PI voice's state (up to 25 KB) is over the Teensy's 16 KB without PSRAM, so there is no .rmp (RMP1).
# MIT, Copyright 2026 trigcondition and contributors.
.DEFAULT_GOAL := a64
FAM ?= pi
MDK := ../../third_party/mdkit
ifeq ($(wildcard $(MDK)/include/mdkit.h),)
$(error plugins/mdkit: ./fetch.sh first)
endif
VOICES_pi := bd sd mt ml ma rs rc cc hh
VOICES_efm := bd sd xt cp rs cb hh cy
VOICES_trx := bd sd xt cp rs cb ch oh cy ma cl xc b2
# MD: the three in ENGINE order (FM, PI, TRX: families.h MDK_FAM_MD), each voice as <family>_<voice>
KEYS_md := $(addprefix efm_,$(VOICES_efm)) $(addprefix pi_,$(VOICES_pi)) $(addprefix trx_,$(VOICES_trx))
KEYS_pi := $(addprefix pi_,$(VOICES_pi))
KEYS_efm := $(addprefix efm_,$(VOICES_efm))
KEYS_trx := $(addprefix trx_,$(VOICES_trx))
ifeq ($(KEYS_$(FAM)),)
$(error FAM=$(FAM): md, pi, efm or trx)
endif
FAMU := $(shell echo $(FAM) | tr a-z A-Z)
# declared CPU per voice (per mille of a Teensy voice; only instancing reads it, and these are mono): from the Mac's
# measured worst TYPE (tests/mdk_check.cpp's cpu line) scaled to the Pi; the Pi's own figure is the JSON's "ns"
MDK_PERMILLE_pi := 300
MDK_PERMILLE_efm := 300
MDK_PERMILLE_trx := 200
MDK_PERMILLE_md := 300
NAME := mdk_$(FAM)
MANIFEST := $(FAM).json
BUILD := build/$(FAM)
PATCHED := build/patched
WINDOWED := $(if $(filter pi md,$(FAM)),rc cc hh)
SRCS := mdk.c $(foreach k,$(KEYS_$(FAM)),mdkit_$(k).c)
VDIRS := $(foreach k,$(KEYS_$(FAM)),$(MDK)/src/$(subst _,/,$(k)))
# the windowed PI cymbals (patch_pi.py) are found before mdkit's own
vpath %.c $(PATCHED) $(VDIRS)
INCS := -I. -I$(PATCHED) $(addprefix -I,$(VDIRS)) -I$(MDK)/include
CFLAGS_EXTRA := -DMDK_FAM_$(FAMU) -DMDK_PERMILLE=$(MDK_PERMILLE_$(FAM))
A64_MAKE_ARGS := FAM=$(FAM)
include ../../sdk/rmp/plugin.mk
# mdkit's own build is C99 (-std=c99) and, with the Mac's clang that its checks ran on, fuses a * b + c into one
# rounding (clang's default contraction); gcc's ISO C would not (EFM-BD then differs in the last bit). -ffp-contract=fast
# on gcc gives mdkit's clang build bit for bit, every voice (tests/mdk_check.cpp "exact"). The glue is C11.
# -O3 (review I2): gcc vectorises mdkit's init loops (its tables and its zeroing): a TYPE's set-up on the control core
# 19.5 -> 5.8 us on the Mac (MD-PI's worst), render ~5 % faster, the images ~10 % larger; still bit-exact
$(BUILD)/a64/mdkit_%.o: A64FLAGS += -std=c99 -ffp-contract=fast -O3
$(BUILD)/a64/mdk.o: A64FLAGS += -std=c11
$(BUILD)/native/mdkit_%.o: NFLAGS += -std=c99

define WINDOW_RULE
$(PATCHED)/mdkit_pi_$(1).h: $(PATCHED)/mdkit_pi_$(1).c
$(PATCHED)/mdkit_pi_$(1).c: $(MDK)/src/pi/$(1)/mdkit_pi_$(1).c $(MDK)/src/pi/$(1)/mdkit_pi_$(1).h patch_pi.py
	python3 patch_pi.py $(MDK)/src/pi/$(1) $(PATCHED) $(1)
endef
$(foreach v,rc cc hh,$(eval $(call WINDOW_RULE,$(v))))
patched: $(foreach v,$(WINDOWED),$(PATCHED)/mdkit_pi_$(v).c)
# every object sees families.h's headers: the windowed ones made first (on the Mac, before the docker build)
$(OBJS) $(NOBJS) $(AOBJS): | patched
a64-elf: patched
$(AOBJS) $(NOBJS): Makefile

FAMS ?= md pi efm trx
packages:
	@mkdir -p out
	for f in $(FAMS); do $(MAKE) --no-print-directory FAM=$$f a64 && cp mdk_$$f.a64.rmp out/$$([ $$f = md ] && echo mdk || echo mdk_$$f).rmp || exit 1; done
	@ls -l out
check: packages
	$(MAKE) -C tests run FAMS="$(FAMS)"
clean-all:
	rm -rf build out mdk_*.elf mdk_*.rmp libmdk_*.dylib
	$(MAKE) -C tests clean
.PHONY: packages patched check clean-all
