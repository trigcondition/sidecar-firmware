# Host build of the board-agnostic M7 benchmark (m7_bench.c + host_m7_bench.c),
# to try it and its output check before porting it to a board. For the Daisy
# Seed port see daisy_seed/.
#
#   make
#   ./host_m7_bench ../../weights/a2lite.namb

CC     ?= cc
CFLAGS ?= -O2
ROOT   := ../..
TUNE   ?=
engine = $(firstword $(wildcard $(ROOT)/engines/cortex-m7/$(1)$(TUNE)) $(ROOT)/engines/cortex-m7/$(1))
LITE := $(call engine,a2lite)
FULL := $(call engine,a2full)

host_m7_bench: m7_bench.c host_m7_bench.c expected.h $(ROOT)/example/a2_engine.c
	$(CC) $(CFLAGS) -I. -I$(ROOT)/example -I$(LITE) -I$(FULL) -I$(ROOT)/namb m7_bench.c host_m7_bench.c \
	    $(ROOT)/example/a2_engine.c $(LITE)/nam_a2lite.c $(FULL)/nam_a2full.c $(ROOT)/namb/namb_reader.c -lm -o $@

clean:
	rm -f host_m7_bench
.PHONY: clean
