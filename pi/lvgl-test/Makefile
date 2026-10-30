# pi/lvgl-test/Makefile - the display test kernel (docs/pi-display.md). Built by ./build.sh in Docker against the
# engine's circle-engine tree with LVGL added (build.sh); flashed by
# CIRCLE_TREE=circle-engine ../circle/flash.sh ../lvgl-test.
CIRCLEHOME ?= ../circle/build/circle-engine
LVGLDIR := $(CIRCLEHOME)/addon/lvgl/lvgl

OBJS = main.o kernel.o ../ui/sidecar_display.o ../ui/sidecar_ui.o ../ui/sidecar_wordmark.o ../ui/sidecar_splash.o
OBJS += ../ui/sidecar_seqed.o ../ui/sidecar_rytmpat.o ../ui/rytm_pattern.o ../ui/sidecar_theme.o ../ui/sidecar_dot.o ../ui/sidecar_dotfont.o	# the pattern pages (no sequencer here: the SEQ page says so)

EXTRAINCLUDE += -I ../ui -I $(LVGLDIR)

LIBS = $(CIRCLEHOME)/addon/lvgl/liblvgl.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
