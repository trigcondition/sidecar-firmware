# plugins/api3_test/Makefile - Plugin API 3's test machine (probe.c; core/tests/test_api3.cpp), in variants:
#   make a64              api3_test.a64.rmp     a dual package (API 2 table + rmp3_plugin): the probe
#   make V=only a64       api3_only.a64.rmp     the same as an API 3 only table (api 3, no API 2 render)
#   make V=initcrash a64  api3_initcrash.a64.rmp  its init() faults
#   make V=needs a64      api3_needs.a64.rmp    needs a capability no host has yet (the signal bus)
#   make V=big a64        api3_big.a64.rmp      an arena over the per-slot limit (64 MB a state)
#   make V=stereo a64     api3_stereo.a64.rmp   out_channels 2 (R = -L on PRE / POST)
#   make V=poly|polyarena|setupcrash|setupslow|setupmore|setuprefuse|attachcrash|fx a64: the other variants
#                         (probe.c's and fxprobe.c's headers)
#   make all-variants     every one; make fixtures copies them for the tests, make check-fixtures compares
# aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image (sdk/rmp/plugin.mk).
V ?= dual
VARIANTS := dual only initcrash needs big stereo poly polyarena setupcrash setupslow setupmore setuprefuse attachcrash fx
NAME_dual := api3_test
NAME_only := api3_only
NAME_initcrash := api3_initcrash
NAME_needs := api3_needs
NAME_big := api3_big
NAME_stereo := api3_stereo
NAME_poly := api3_poly
NAME_polyarena := api3_polyar
NAME_setupcrash := api3_setupcr
NAME_setupslow := api3_setupsl
NAME_setupmore := api3_setupmo
NAME_setuprefuse := api3_setupre
NAME_attachcrash := api3_attachcr
NAME_fx := api3_fx
DEF_dual :=
DEF_only := -DPROBE_ONLY=1
DEF_initcrash := -DPROBE_INITCRASH=1
DEF_needs := -DPROBE_NEEDS=1
DEF_big := -DPROBE_BIG=1
DEF_stereo := -DPROBE_STEREO=1
DEF_poly := -DPROBE_NOARENA=1
DEF_polyarena := 
DEF_setupcrash := -DPROBE_SETUP=1
DEF_setupslow := -DPROBE_SETUP=2
DEF_setupmore := -DPROBE_SETUP=3
DEF_setuprefuse := -DPROBE_SETUP=4
DEF_attachcrash := -DPROBE_ATTACHCRASH=1
DEF_fx := 
NAME := $(NAME_$(V))
ifeq ($(NAME),)
$(error V=$(V): one of $(VARIANTS))
endif
SRCS := $(if $(filter fx,$(V)),fxprobe.c,probe.c)
BUILD := build/$(V)
CFLAGS_EXTRA := $(DEF_$(V))
MANIFEST := $(V).json
A64_MAKE_ARGS := V=$(V)
include ../../sdk/rmp/plugin.mk
all-variants:
	for v in $(VARIANTS); do $(MAKE) V=$$v a64 || exit 1; done
.PHONY: all-variants
# the packages core/tests/test_api3.cpp loads (CI has no AArch64 compiler): rebuild and copy after a change to probe.c
fixtures: all-variants
	for v in $(VARIANTS); do cp $$($(MAKE) -s V=$$v name).a64.rmp ../../core/tests/api3_pkgs/ || exit 1; done
# the committed packages are what probe.c / fxprobe.c build now (with an AArch64 compiler or the Docker image)
check-fixtures: all-variants
	for v in $(VARIANTS); do n=$$($(MAKE) -s V=$$v name); cmp $$n.a64.rmp ../../core/tests/api3_pkgs/$$n.a64.rmp || exit 1; done
	@echo "api3_test: the committed packages match the sources"
name:
	@echo $(NAME)
.PHONY: fixtures check-fixtures name
