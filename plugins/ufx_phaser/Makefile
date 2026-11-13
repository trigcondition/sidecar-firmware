# plugins/ufx_phaser/Makefile - PHASER, a stereo phaser user effect (Plugin API 3; phaser.c; MIT).
#   make a64        ufx_phaser.a64.rmp (aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image)
#   make fixtures   also copied to core/tests/api3_pkgs (core/tests/test_ufx.cpp loads it; CI has no AArch64 compiler)
NAME := ufx_phaser
SRCS := phaser.c
include ../../sdk/rmp/plugin.mk
fixtures: a64
	cp ufx_phaser.a64.rmp ../../core/tests/api3_pkgs/
.PHONY: fixtures
