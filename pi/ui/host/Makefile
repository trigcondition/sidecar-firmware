# pi/ui/host/Makefile - sidecar_ui on the Mac (shot.c): LVGL from pi/circle/build/lvgl-src (pi/ui/fetch-lvgl.sh),
# with Circle's lv_conf.h (addon/lvgl) minus its halt() assert. make -> shot.png (sips). Objects in obj/.
# The SEQ pages (sidecar_seqed.c, sidecar_rytmpat.c) edit a core/seq pattern of their own through seq_touch.cpp: core's
# seq/pattern.cpp and poly.cpp (its chord table) are compiled here, the rest of core left out by the linker
# (-dead_strip). poly.cpp needs the engines' headers: ENG (make -C core engines, once; from a worktree point ENG at the
# main checkout's core/build/engines).
B    ?= ../../circle/build
LVGL ?= $(B)/lvgl-src
LVCONF ?= $(B)/circle-engine/addon/lvgl/lv_conf.h
CORE ?= ../../../core
ENG  ?= $(CORE)/build/engines
SRC  := $(shell find $(LVGL)/src -name '*.c')
OBJ  := $(patsubst $(LVGL)/%.c,obj/%.o,$(SRC))
CFLAGS = -O1 -w -I obj -I $(LVGL) -DLV_CONF_INCLUDE_SIMPLE
SEQFLAGS = -std=c++17 -O1 -Wall -Wno-unused-function -DCORE_POLY=1 -DCORE_MSAMP=1 -DTEST -I$(CORE) -I$(CORE)/host \
	   -I$(CORE)/../teensy/libraries/RytmPlugins/src -I$(CORE)/../sdk -I$(ENG)/Plaits -I$(ENG)/Juno -I$(ENG)/DaisySP \
	   -I$(ENG)/Sampler -I obj -I $(LVGL) -DLV_CONF_INCLUDE_SIMPLE
UI_C := ../sidecar_ui.c ../sidecar_theme.c ../sidecar_dot.c ../sidecar_dotfont.c ../sidecar_wordmark.c ../sidecar_seqed.c \
	../sidecar_rytmpat.c ../rytm_pattern.c ../sidecar_wgview.c ../sidecar_voice.c ../sidecar_insfx.c ../sidecar_song.c ../sidecar_rec.c \
	../sidecar_scope.c ../sidecar_status.c ../sidecar_project.c ../sidecar_chop.c ../touch_cal.c rytm_fake.c
UI_O := $(addprefix obj/ui-,$(notdir $(UI_C:.c=.o)))
SEQ_O := obj/seq_touch.o obj/seqdemo.o obj/seq_pattern.o obj/seq_music.o obj/seq_xcmd.o obj/poly.o obj/rytm_dump.o obj/inst_alloc.o
UI_H := ../sidecar_ui.h ../sidecar_theme.h ../sidecar_dot.h ../sidecar_dotfont.h ../sidecar_seqed.h ../sidecar_rytmpat.h \
	../rytm_pattern.h ../seq_touch.h ../sidecar_wgview.h ../sidecar_voice.h ../sidecar_insfx.h ../sidecar_song.h ../sidecar_rec.h \
	../sidecar_scope.h ../sidecar_status.h ../sidecar_project.h ../sidecar_chop.h ../touch_cal.h

# shots/: every page in both themes (oled-*.png, classic-*.png: home, sound, fx, display, about, hidden, home-info,
# power, seq-*, rytm-*). ./shot also checks the commands the controls send and fails if a SEQ gesture does not reach the
# model; it prints the draw costs. shot.png: the OLED home page.
# SIZE=1024x600: the wide layouts, into shots-1024x600/ (and shot-1024x600.png); shots-all: both sizes
SIZE ?= 800x480
SHOTS := $(if $(filter 800x480,$(SIZE)),shots,shots-$(SIZE))
SHOTPNG := $(if $(filter 800x480,$(SIZE)),shot.png,shot-$(SIZE).png)
shot.png: shot
	@rm -rf obj/$(SHOTS); mkdir -p obj/$(SHOTS) $(SHOTS)
	./shot obj/$(SHOTS) $(SIZE)
	for f in obj/$(SHOTS)/*.ppm; do sips -s format png $$f --out $(SHOTS)/$$(basename $$f .ppm).png >/dev/null; done
	cp $(SHOTS)/oled-home.png $(SHOTPNG)
shots-all: shot
	$(MAKE) shot.png SIZE=800x480
	$(MAKE) shot.png SIZE=1024x600
# splash.png (OLED, the default) and splash-classic.png: the boot screen (sidecar_splash.c), no LVGL
splash.png: ../sidecar_splash.c ../sidecar_splash.h ../sidecar_wordmark.c ../sidecar_dot.c ../sidecar_dotfont.c splash.c
	@mkdir -p obj
	@$(CC) -O1 -Wall -o obj/splash splash.c ../sidecar_splash.c ../sidecar_wordmark.c ../sidecar_dot.c ../sidecar_dotfont.c
	obj/splash obj/splash.ppm 0
	obj/splash obj/splash-classic.ppm 1
	sips -s format png obj/splash.ppm --out splash.png >/dev/null
	sips -s format png obj/splash-classic.ppm --out splash-classic.png >/dev/null
# obj/splash-1024x600.png, obj/splash-classic-1024x600.png: the boot screen on a 1024x600 frame buffer (not kept)
splash-wide: splash.png
	obj/splash obj/splash-1024x600.ppm 0 1024x600
	obj/splash obj/splash-classic-1024x600.ppm 1 1024x600
	sips -s format png obj/splash-1024x600.ppm --out obj/splash-1024x600.png >/dev/null
	sips -s format png obj/splash-classic-1024x600.ppm --out obj/splash-classic-1024x600.png >/dev/null
# anim/: the OLED SEQ page playing (shot anim): frames, then seq-oled.mp4 and seq-oled.gif (ffmpeg)
anim: shot
	@rm -rf obj/anim; mkdir -p obj/anim
	./shot anim obj/anim 300
	ffmpeg -y -loglevel error -framerate 20 -i obj/anim/frame-%04d.ppm -c:v libx264 -pix_fmt yuv420p -crf 16 seq-oled.mp4
	ffmpeg -y -loglevel error -framerate 20 -i obj/anim/frame-%04d.ppm -vf "fps=20,split[a][b];[a]palettegen=max_colors=64[p];[b][p]paletteuse=dither=none" seq-oled.gif
shot: obj/shot.o $(UI_O) $(SEQ_O) $(OBJ)
	@$(CXX) -o $@ obj/shot.o $(UI_O) $(SEQ_O) $(OBJ) -Wl,-dead_strip
obj/shot.o: shot.c $(UI_H) obj/lv_conf.h
	$(CC) $(CFLAGS) -c $< -o $@
obj/ui-%.o: ../%.c $(UI_H) obj/lv_conf.h
	$(CC) $(CFLAGS) -Wall -Wno-unused-function -c $< -o $@
obj/ui-%.o: %.c $(UI_H) obj/lv_conf.h
	$(CC) $(CFLAGS) -Wall -c $< -o $@
obj/seq_touch.o: ../seq_touch.cpp ../seq_touch.h ../sidecar_seqed.h $(CORE)/seq/seq.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -c $< -o $@
obj/seqdemo.o: seqdemo.cpp ../seq_touch.h $(CORE)/seq/seq.h $(CORE)/seq/rytm_dump.h $(CORE)/inst_alloc.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -c $< -o $@
obj/seq_pattern.o: $(CORE)/seq/pattern.cpp $(CORE)/seq/seq.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -w -c $< -o $@
obj/seq_music.o: $(CORE)/seq/music.cpp $(CORE)/seq/seq.h $(CORE)/seq/rng.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -w -c $< -o $@
obj/seq_xcmd.o: $(CORE)/seq/xcmd.cpp $(CORE)/seq/seq.h $(CORE)/seq/rng.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -w -c $< -o $@
obj/rytm_dump.o: $(CORE)/seq/rytm_dump.cpp $(CORE)/seq/rytm_dump.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -w -c $< -o $@
obj/poly.o: $(CORE)/poly.cpp $(CORE)/poly.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -w -c $< -o $@
obj/inst_alloc.o: $(CORE)/inst_alloc.cpp $(CORE)/inst_alloc.h $(CORE)/seq/seq.h obj/lv_conf.h
	$(CXX) $(SEQFLAGS) -w -c $< -o $@
# test: the editor's model side on core/seq, the Rytm pattern record
test: obj/test_seq_touch obj/test_rytm_pattern obj/test_voice_follow
	obj/test_seq_touch && obj/test_rytm_pattern && obj/test_voice_follow
obj/test_seq_touch: test_seq_touch.cpp obj/seq_touch.o obj/seq_pattern.o obj/poly.o
	$(CXX) $(SEQFLAGS) -o $@ $< obj/seq_touch.o obj/seq_pattern.o obj/poly.o -Wl,-dead_strip
obj/test_rytm_pattern: test_rytm_pattern.c ../rytm_pattern.c rytm_fake.c ../rytm_pattern.h
	@mkdir -p obj
	$(CC) -O1 -Wall -o $@ test_rytm_pattern.c ../rytm_pattern.c rytm_fake.c
obj/test_voice_follow: test_voice_follow.cpp ../../engine/voice_pi.h $(CORE)/machines.h
	@mkdir -p obj
	$(CXX) $(SEQFLAGS) -o $@ $<
obj/lv_conf.h: $(LVCONF)
	@mkdir -p obj
	sed -e 's|<circle/startup.h>|<stdlib.h>|' -e 's|halt();|abort();|' $< > $@
obj/%.o: $(LVGL)/%.c obj/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<
.PHONY: test clean anim shots-all shot.png splash-wide
clean:
	rm -rf obj shot shot*.png seq-*.png rytm-*.png shots shots-* seq-oled.mp4 seq-oled.gif
