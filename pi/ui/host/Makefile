# pi/ui/host/Makefile - sidecar_ui on the Mac (shot.c): LVGL from pi/circle/build/lvgl-src (pi/ui/fetch-lvgl.sh),
# with Circle's lv_conf.h (addon/lvgl) minus its halt() assert. make -> shot.png. Objects in obj/.
# ./shot writes its frames as PNG (zlib, -lz); make bounds writes none; make clean-shots drops every frame.
# The SEQ pages (sidecar_seqed.c, sidecar_rytmpat.c) edit a core/seq pattern of their own through seq_touch.cpp: core's
# seq/pattern.cpp and poly.cpp (its chord table) are compiled here, the rest of core left out by the linker
# (-dead_strip). poly.cpp needs the engines' headers: ENG (make -C core engines, once; from a worktree point ENG at the
# main checkout's core/build/engines).
B    ?= ../../circle/build
LVGL ?= $(B)/lvgl-src
unexport SHOT_NOFRAMES		# only `make bounds` sets it, per call
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
	../sidecar_rytmpat.c ../rytm_pattern.c ../sidecar_wgview.c ../sidecar_voice.c ../sidecar_insfx.c ../sidecar_mfx.c ../sidecar_morph.c ../sidecar_loop.c ../sidecar_xy.c ../sidecar_song.c ../sidecar_rec.c \
	../sidecar_scope.c ../sidecar_status.c ../sidecar_project.c ../sidecar_chop.c ../sidecar_sedit.c ../sidecar_preset.c ../touch_cal.c rytm_fake.c
UI_O := $(addprefix obj/ui-,$(notdir $(UI_C:.c=.o)))
FONT_O := $(patsubst %,obj/ui-sidecar_arb%.o,11 12 14 16 18 19 20 24 30 36 44) $(patsubst %,obj/ui-sidecar_ar%.o,13 18 28)
SEQ_O := obj/fuzz_blit.o obj/seq_touch.o obj/seqdemo.o obj/seq_pattern.o obj/seq_music.o obj/seq_xcmd.o obj/poly.o obj/rytm_dump.o obj/inst_alloc.o
UI_H := ../sidecar_ui.h ../sidecar_theme.h ../sidecar_dot.h ../sidecar_dotfont.h ../sidecar_seqed.h ../sidecar_rytmpat.h \
	../rytm_pattern.h ../seq_touch.h ../sidecar_wgview.h ../sidecar_voice.h ../sidecar_insfx.h ../sidecar_mfx.h ../sidecar_morph.h ../sidecar_loop.h ../sidecar_xy.h ../sidecar_song.h ../sidecar_rec.h \
	../sidecar_scope.h ../sidecar_status.h ../sidecar_project.h ../sidecar_chop.h ../sidecar_sedit.h ../sidecar_preset.h ../touch_cal.h ../sidecar_logo.h

# shots/: every page in both themes (oled-*.png, sidecar-*.png: home, sound, fx, display, about, hidden, home-info,
# power, seq-*, rytm-*). ./shot also checks the commands the controls send and fails if a SEQ gesture does not reach the
# model; it prints the draw costs. shot.png: the OLED home page.
# SIZE=1024x600 or 1280x720: those layouts, into shots-WxH/ (and shot-WxH.png); shots-all: all three sizes
SIZE ?= 800x480
SHOTS := $(if $(filter 800x480,$(SIZE)),shots,shots-$(SIZE))
SHOTPNG := $(if $(filter 800x480,$(SIZE)),shot.png,shot-$(SIZE).png)
shot.png: shot
	@rm -rf obj/$(SHOTS); mkdir -p obj/$(SHOTS) $(SHOTS)
	./shot obj/$(SHOTS) $(SIZE)
	mv obj/$(SHOTS)/*.png $(SHOTS)/
	cp $(SHOTS)/oled-home.png $(SHOTPNG)
# shots-hc: the SIDECAR theme's high contrast (0.5.1), blue, green and Viper: sidecar-hc-*.png, sidecar-hcg-*.png, sidecar-vp-*.png beside the others in
# shots/ (each look in a process of its own; ./shot fails on a miss as in the plain run)
shots-hc: shot
	@mkdir -p obj/$(SHOTS)-hc obj/$(SHOTS)-hcg obj/$(SHOTS)-viper $(SHOTS)
	./shot obj/$(SHOTS)-hc $(SIZE) hc
	./shot obj/$(SHOTS)-hcg $(SIZE) hcg
	./shot obj/$(SHOTS)-viper $(SIZE) viper
	mv obj/$(SHOTS)-hc/sidecar-hc-*.png obj/$(SHOTS)-hcg/sidecar-hcg-*.png obj/$(SHOTS)-viper/sidecar-vp-*.png $(SHOTS)/
# shots-grid: the GRID theme: grid-*.png beside the others in shots/ (a process of its own, as hc is)
shots-grid: shot
	@mkdir -p obj/$(SHOTS)-grid $(SHOTS)
	./shot obj/$(SHOTS)-grid $(SIZE) grid
	mv obj/$(SHOTS)-grid/grid-*.png $(SHOTS)/
shots-all: shot
	$(MAKE) shot.png SIZE=800x480
	$(MAKE) shot.png SIZE=1024x600
	$(MAKE) shot.png SIZE=1280x720
# shots-a4/: an A4 the box in use (./shot a4): its word and its T1..T4 on the splash, HOME, STATUS, REC, SCOPE, INSERT FX, Sound
shots-a4: shot
	@rm -rf obj/shots-a4; mkdir -p obj/shots-a4 shots-a4
	./shot a4 obj/shots-a4 $(SIZE)
	mv obj/shots-a4/*.png shots-a4/
# bounds: every style and contrast (OLED + SIDECAR, SIDECAR high blue and high green, GRID) at the three sizes. Each ./shot run
# fails on a text that does not fit or a check that misses; the 1024x600 and 1280x720 shots' objects are then compared
# (check_bounds.py). The 800x480 runs are the text-fit and check runs only. SHOT_NOFRAMES=1: ./shot writes no frames,
# only the .bounds files (run ./shot without it to keep the frames).
LOOKS := plain hc hcg viper grid
bounds: shot
	@rm -rf obj/bounds-800 obj/bounds-1024 obj/bounds-1280; mkdir -p obj/bounds-800 obj/bounds-1024 obj/bounds-1280
	@for look in $(LOOKS); do \
		SHOT_NOFRAMES=1 ./shot obj/bounds-800 800x480 $$look >/dev/null || exit 1; \
		SHOT_NOFRAMES=1 ./shot obj/bounds-1024 1024x600 $$look >/dev/null || exit 1; \
		SHOT_NOFRAMES=1 ./shot obj/bounds-1280 1280x720 $$look >/dev/null || exit 1; \
	done
	python3 check_bounds.py obj/bounds-1024 obj/bounds-1280
# splash.png (OLED, the default) and splash-sidecar.png: the boot screen (sidecar_splash.c), no LVGL
splash.png: ../sidecar_splash.c ../sidecar_splash.h ../sidecar_wordmark.c ../sidecar_dot.c ../sidecar_dotfont.c splash.c ../sidecar_logo.h
	@mkdir -p obj
	@$(CC) -O1 -Wall -o obj/splash splash.c ../sidecar_splash.c ../sidecar_wordmark.c ../sidecar_dot.c ../sidecar_dotfont.c
	obj/splash obj/splash.ppm 0
	obj/splash obj/splash-sidecar.ppm 1
	sips -s format png obj/splash.ppm --out splash.png >/dev/null
	sips -s format png obj/splash-sidecar.ppm --out splash-sidecar.png >/dev/null
# obj/splash-1024x600.png, obj/splash-sidecar-1024x600.png: the boot screen on a 1024x600 frame buffer (not kept)
splash-wide: splash.png
	obj/splash obj/splash-1024x600.ppm 0 1024x600
	obj/splash obj/splash-sidecar-1024x600.ppm 1 1024x600
	sips -s format png obj/splash-1024x600.ppm --out obj/splash-1024x600.png >/dev/null
	sips -s format png obj/splash-sidecar-1024x600.ppm --out obj/splash-sidecar-1024x600.png >/dev/null
# anim/: the OLED SEQ page playing (shot anim): frames, then seq-oled.mp4 and seq-oled.gif (ffmpeg)
anim: shot
	@rm -rf obj/anim; mkdir -p obj/anim
	./shot anim obj/anim 300
	ffmpeg -y -loglevel error -framerate 20 -i obj/anim/frame-%04d.png -c:v libx264 -pix_fmt yuv420p -crf 16 seq-oled.mp4
	ffmpeg -y -loglevel error -framerate 20 -i obj/anim/frame-%04d.png -vf "fps=20,split[a][b];[a]palettegen=max_colors=64[p];[b][p]paletteuse=dither=none" seq-oled.gif
	rm -rf obj/anim
# ui-objs: the pages, fonts, SEQ model and LVGL objects without shot.o
ui-objs: $(UI_O) $(FONT_O) $(SEQ_O) $(OBJ)
# the remote UI service's allowlist, for shot.c's check of the lines the SEQ page sends (named test_allow_*: the remote app's link leaves them out)
ALLOW_O := obj/test_allow_shim.o obj/test_allow_srv.o obj/test_allow_proto.o obj/test_allow_views.o obj/test_allow_sha.o
obj/test_allow_shim.o: allow_shim.cpp ../../engine/remote_srv.h
	$(CXX) -std=c++17 -O1 -c $< -o $@
obj/test_allow_srv.o: ../../engine/remote_srv.cpp ../../engine/remote_srv.h
	$(CXX) -std=c++17 -O1 -c $< -o $@
obj/test_allow_proto.o: ../remote/rmt_proto.c $(wildcard ../remote/*.h)
	$(CC) -std=c11 -O1 -c $< -o $@
obj/test_allow_views.o: ../remote/rmt_views.c $(wildcard ../remote/*.h) $(UI_H)
	$(CC) -std=c11 -O1 -c $< -o $@
obj/test_allow_sha.o: ../remote/rmt_sha256.c $(wildcard ../remote/*.h)
	$(CC) -std=c11 -O1 -c $< -o $@
shot: obj/shot.o $(UI_O) $(FONT_O) $(SEQ_O) $(OBJ) $(ALLOW_O)
	@$(CXX) -o $@ obj/shot.o $(UI_O) $(FONT_O) $(SEQ_O) $(OBJ) $(ALLOW_O) -lz -Wl,-dead_strip
obj/shot.o: shot.c $(UI_H) obj/lv_conf.h
	$(CC) $(CFLAGS) -c $< -o $@
obj/ui-%.o: ../%.c $(UI_H) obj/lv_conf.h
	$(CC) $(CFLAGS) -Wall -Wno-unused-function -c $< -o $@
obj/ui-%.o: %.c $(UI_H) obj/lv_conf.h
	$(CC) $(CFLAGS) -Wall -c $< -o $@
obj/ui-sidecar_arb%.o: ../fonts/sidecar_arb%.c obj/lv_conf.h
	$(CC) $(CFLAGS) -c $< -o $@
obj/ui-sidecar_ar%.o: ../fonts/sidecar_ar%.c obj/lv_conf.h
	$(CC) $(CFLAGS) -c $< -o $@
obj/fuzz_blit.o: fuzz_blit.cpp ../dsi_rotate.h
	$(CXX) $(SEQFLAGS) -c $< -o $@
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
# test: the editor's model side on core/seq, the Rytm pattern record, the remote UI's protocol library (../remote)
test: obj/test_seq_touch obj/test_rytm_pattern obj/test_voice_follow
	obj/test_seq_touch && obj/test_rytm_pattern && obj/test_voice_follow
	$(MAKE) -C ../remote test
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
# clean-shots: drops every frame (the shots dirs, the SEQ stills, the animation) and the bounds dirs; keeps the objects
clean-shots:
	rm -rf obj/shots* obj/bounds-* obj/anim shots shots-* shot*.png seq-*.png rytm-*.png seq-oled.mp4 seq-oled.gif
.PHONY: ui-objs test clean clean-shots anim shots-grid shots-hc shots-all shots-a4 shot.png splash-wide bounds
clean:
	rm -rf obj shot shot*.png seq-*.png rytm-*.png shots shots-* seq-oled.mp4 seq-oled.gif
