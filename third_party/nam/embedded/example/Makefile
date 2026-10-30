# Host build of host_example.c with both engines of one target.
#
#   make                                  # Cortex-A76 engines (default)
#   make TARGET=cortex-m7                 # Cortex-M7 engines (C tiles on a host)
#   make TARGET=cortex-m7 TUNE=-b16       # engines tuned for 16-frame blocks,
#                                         # where they differ (else the default)
#   make selftest MODEL=a2lite            # compare with the reference output
#   make check-all                        # every engine, block size and setting
#
# On an AArch64 host (Linux on A76, Apple Silicon) the A76 engines build with
# their NEON assembly; on other hosts with a portable scalar fallback. The M7
# engines always build their C tiles on a host. Results match the reference
# either way. Extra settings go in CFLAGS, e.g.
#   make CFLAGS="-O2 -DNAM_A2LITE_MAX_BUFFER_SIZE=16 -DNAM_A2FULL_MAX_BUFFER_SIZE=16"

CC      ?= cc
CFLAGS  ?= -O2
TARGET  ?= cortex-a76
TUNE    ?=
MODEL   ?= a2full
ENGINES ?= ../engines/$(TARGET)
NAMB    ?= ../namb
WEIGHTS ?= ../weights
TEST    ?= ../test

# The engine directory of a model: the tuned one if it exists.
engine = $(firstword $(wildcard $(ENGINES)/$(1)$(TUNE)) $(ENGINES)/$(1))
LITE := $(call engine,a2lite)
FULL := $(call engine,a2full)

host_example: host_example.c a2_engine.c a2_engine.h $(LITE)/nam_a2lite.c $(FULL)/nam_a2full.c $(NAMB)/namb_reader.c
	$(CC) $(CFLAGS) -I. -I$(LITE) -I$(FULL) -I$(NAMB) host_example.c a2_engine.c \
	    $(LITE)/nam_a2lite.c $(FULL)/nam_a2full.c $(NAMB)/namb_reader.c -lm -o $@

selftest: host_example
	./host_example selftest $(WEIGHTS)/$(MODEL).namb $(TEST)/golden_input.raw $(TEST)/golden_$(MODEL)_output.raw

# Both targets; each tuning that exists; the default NAM_*_MAX_BUFFER_SIZE and
# 16; on the A76, block hints 48, 16 and 20.
check-all:
	@set -e; for t in cortex-m7 cortex-a76; do for tune in "" -b16; do \
	  if [ -n "$$tune" ] && ! ls -d ../engines/$$t/*$$tune > /dev/null 2>&1; then continue; fi; \
	  hints=""; [ $$t = cortex-a76 ] && hints="48 16 20"; \
	  for mb in "" 16; do for h in $${hints:-none}; do \
	    defs=""; [ -n "$$mb" ] && defs="-DNAM_A2LITE_MAX_BUFFER_SIZE=$$mb -DNAM_A2FULL_MAX_BUFFER_SIZE=$$mb"; \
	    [ $$h != none ] && defs="$$defs -DNAM_A2LITE_BLOCK_HINT=$$h -DNAM_A2FULL_BLOCK_HINT=$$h"; \
	    label="$$t$$tune"; [ -n "$$mb" ] && label="$$label, max block $$mb"; \
	    [ $$h != none ] && label="$$label, block hint $$h"; echo "== $$label"; \
	    $(MAKE) -s -B host_example TARGET=$$t TUNE=$$tune CFLAGS="$(CFLAGS) $$defs"; \
	    for m in a2lite a2full; do \
	      ./host_example selftest $(WEIGHTS)/$$m.namb $(TEST)/golden_input.raw $(TEST)/golden_$${m}_output.raw; \
	    done; \
	  done; done; \
	done; done

clean:
	rm -f host_example

.PHONY: selftest check-all clean
