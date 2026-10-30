# core/engines/engines.mk - the engines' sources for a non-Arduino build of the core (core/Makefile,
# pi/engine/Makefile). Set ENG to the folder core/engines/setup_engines.sh filled, include this, then use
# ENG_SRC (C++), ENG_C_SRC (C), ENG_INC (include flags) and ENG_FLAGS (defines).
ENG_SRC   := $(addprefix $(ENG)/Plaits/,$(shell cd $(ENG)/Plaits 2>/dev/null && find plaits stmlib -name '*.cc' | sort)) \
             $(wildcard $(ENG)/DaisySP/Filters/*.cpp $(ENG)/DaisySP/Utility/*.cpp)
# ENG_C_SRC: the C sources: mdkit's voices and the MD glue (the built-in MD, core/md/md.h; DIGIDRUM's until 2026-10-29)
ENG_C_SRC := $(wildcard $(ENG)/MDKit/*.c)
ENG_INC   := -I$(ENG)/Plaits -I$(ENG)/Juno -I$(ENG)/DaisySP -I$(ENG)/Sampler -I$(ENG)/MDKit
# stmlib's portable code paths (no Cortex-M ssat/usat asm, no .ramtext section), as sim/plaits/build.sh and spike3
ENG_FLAGS := -DTEST
