# plugins/ufx_pingpong/Makefile - PINGPONG, the example Plugin API 3 user effect (pingpong.c; MIT).
#   make a64        ufx_pingpong.a64.rmp (aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image)
#   make fixtures   also copied to core/tests/api3_pkgs (core/tests/test_api3.cpp loads it; CI has no AArch64 compiler)
NAME := ufx_pingpong
SRCS := pingpong.c
include ../../sdk/rmp/plugin.mk
fixtures: a64
	cp ufx_pingpong.a64.rmp ../../core/tests/api3_pkgs/
.PHONY: fixtures
