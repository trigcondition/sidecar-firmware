# Daisy Seed build of m7_bench, with both engines (the embedded capture's
# .namb file selects the model).
#
#   make MODEL=a2lite LIBDAISY_DIR=/path/to/libDaisy GCC_PATH=/path/to/arm/bin
#   make MODEL=a2lite BLOCK=16 ...   # 16-frame blocks, engines tuned for them
#   make program-dfu       # Daisy bootloader waiting for DFU (see README)
#
# Needs a built libDaisy and an arm-none-eabi toolchain with newlib
# (DaisyToolchain or Arm GNU Toolchain).

TARGET = m7bench

ROOT   = ../../..
MODEL  ?= a2full
# Frames per timed block. BLOCK=16 selects the engines tuned for 16-frame
# blocks where they differ from the default ones.
BLOCK  ?= 48
TUNE   = $(if $(filter 16,$(BLOCK)),-b16)
engine = $(firstword $(wildcard $(ROOT)/engines/cortex-m7/$(1)$(TUNE)) $(ROOT)/engines/cortex-m7/$(1))
LITE  := $(call engine,a2lite)
FULL  := $(call engine,a2full)
# Capture embedded in flash and loaded at boot (any A2-Lite or A2-Full .namb).
NAMB   ?= $(ROOT)/weights/$(MODEL).namb
MAX_BUFFER_SIZE ?= $(BLOCK)

GCC_PATH ?=
LIBDAISY_DIR ?= $(ROOT)/../DaisyExamples/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

CPP_SOURCES = bench_main.cpp
C_SOURCES = ../m7_bench.c $(ROOT)/example/a2_engine.c $(LITE)/nam_a2lite.c $(FULL)/nam_a2full.c \
            $(ROOT)/namb/namb_reader.c
C_INCLUDES = -I.. -I$(ROOT)/example -I$(LITE) -I$(FULL) -I$(ROOT)/namb

# The Daisy bootloader stays in internal flash; this app goes to QSPI.
APP_TYPE = BOOT_QSPI
CPP_STANDARD = -std=gnu++17
OPT = -O2

LDSCRIPT = $(BUILD_DIR)/m7bench.lds

# Instance and capture in DTCM (A2_HOT); ring buffers in AXI SRAM (.bss).
BENCH_DEFS = '-DA2_HOT=__attribute__((section(".dtcmram_bss")))' \
             -DNAM_A2LITE_MAX_BUFFER_SIZE=$(MAX_BUFFER_SIZE) \
             -DNAM_A2FULL_MAX_BUFFER_SIZE=$(MAX_BUFFER_SIZE) \
             -DM7_BENCH_BLOCK=$(BLOCK) \
             '-DBENCH_NAMB="$(NAMB)"'

include $(SYSTEM_FILES_DIR)/Makefile

CFLAGS   += -ffast-math $(BENCH_DEFS)
CPPFLAGS += $(BENCH_DEFS)

# libDaisy's QSPI linker script with itcm_section.lds inserted before .text,
# so the engines' code goes to ITCM.
$(BUILD_DIR)/m7bench.lds: $(SYSTEM_FILES_DIR)/STM32H750IB_qspi.lds itcm_section.lds | $(BUILD_DIR)
	awk 'FNR == NR { blk = blk $$0 "\n"; next } \
	     !done && /^[ \t]*\.text[ \t]*:/ { printf "%s", blk; done = 1 } \
	     { print } \
	     END { if (!done) { print "no .text section found" > "/dev/stderr"; exit 1 } }' \
	    itcm_section.lds $< > $@

$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)
$(BUILD_DIR)/nam_a2lite.o $(BUILD_DIR)/nam_a2full.o $(BUILD_DIR)/a2_engine.o $(BUILD_DIR)/m7_bench.o: \
    $(LITE)/nam_a2lite.h $(FULL)/nam_a2full.h
$(BUILD_DIR)/bench_main.o: $(NAMB)
