# plugins/api3_test/Makefile - Plugin API 3's test machine (probe.c; core/tests/test_api3.cpp), in variants:
#   make a64              api3_test.a64.rmp     a dual package (API 2 table + rmp3_plugin): the probe
#   make V=only a64       api3_only.a64.rmp     the same as an API 3 only table (api 3, no API 2 render)
#   make V=initcrash a64  api3_initcrash.a64.rmp  its init() faults
#   make V=needs a64      api3_needs.a64.rmp    needs a capability no host has yet (the signal bus)
#   make V=big a64        api3_big.a64.rmp      an arena over the per-slot limit (64 MB a state)
#   make all-variants     the five
# aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image (sdk/rmp/plugin.mk).
V ?= dual
NAME_dual := api3_test
NAME_only := api3_only
NAME_initcrash := api3_initcrash
NAME_needs := api3_needs
NAME_big := api3_big
DEF_dual :=
DEF_only := -DPROBE_ONLY=1
DEF_initcrash := -DPROBE_INITCRASH=1
DEF_needs := -DPROBE_NEEDS=1
DEF_big := -DPROBE_BIG=1
NAME := $(NAME_$(V))
ifeq ($(NAME),)
$(error V=$(V): dual, only, initcrash, needs or big)
endif
SRCS := probe.c
BUILD := build/$(V)
CFLAGS_EXTRA := $(DEF_$(V))
MANIFEST := $(V).json
A64_MAKE_ARGS := V=$(V)
include ../../sdk/rmp/plugin.mk
all-variants:
	for v in dual only initcrash needs big; do $(MAKE) V=$$v a64 || exit 1; done
.PHONY: all-variants
# the packages core/tests/test_api3.cpp loads (CI has no AArch64 compiler): rebuild and copy after a change to probe.c
fixtures: all-variants
	cp api3_test.a64.rmp api3_only.a64.rmp api3_initcrash.a64.rmp api3_needs.a64.rmp api3_big.a64.rmp ../../core/tests/api3_pkgs/
.PHONY: fixtures
