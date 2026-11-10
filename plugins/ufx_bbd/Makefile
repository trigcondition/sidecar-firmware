# plugins/ufx_bbd/Makefile - BBD, a bucket-brigade style delay user effect (Plugin API 3; bbd.c; MIT).
#   make a64        ufx_bbd.a64.rmp (aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image)
#   make fixtures   also copied to core/tests/api3_pkgs (core/tests/test_ufx.cpp loads it; CI has no AArch64 compiler)
NAME := ufx_bbd
SRCS := bbd.c
include ../../sdk/rmp/plugin.mk
fixtures: a64
	cp ufx_bbd.a64.rmp ../../core/tests/api3_pkgs/
.PHONY: fixtures
