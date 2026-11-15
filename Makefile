# pmark build. Targets:
#   make          core library + CLI           (no dependencies beyond libc/libm)
#   make gui      SDL2 frontend                (needs sdl2 via pkg-config)
#   make test     unit tests + CLI smoke test
#   make asan     rebuild everything under ASan/UBSan and run the tests
#   make vendor   re-download the stb headers into third_party/

CC      ?= cc
OPT     ?= -O2 -g
WARN    := -Wall -Wextra -Wpedantic -Wshadow
CFLAGS  += -std=c11 $(OPT) -Iinclude -Ithird_party
LDLIBS  += -lm
B       := build

CORE_SRC := src/pm_image.c src/pm_exif.c src/pm_draw.c src/pm_text.c src/pm_doc.c
CORE_OBJ := $(CORE_SRC:src/%.c=$(B)/%.o) $(B)/pm_stb.o
HDRS     := include/pmark.h src/pm_internal.h

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

.PHONY: all gui test asan vendor clean
all: $(B)/pmark

$(B):
	mkdir -p $(B)

$(B)/pm_stb.o: src/pm_stb.c | $(B)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(B)/%.o: src/%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@

$(B)/libpmark.a: $(CORE_OBJ)
	$(AR) rcs $@ $^

$(B)/pmark: cli/main.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $< $(B)/libpmark.a $(LDLIBS) -o $@

gui: $(B)/pmark-gui
$(B)/pmark-gui: gui/main.c $(B)/libpmark.a $(HDRS)
	@test -n "$(SDL_LIBS)" || { echo "SDL2 not found (install libsdl2-dev / sdl2)"; exit 1; }
	$(CC) $(CFLAGS) $(WARN) $(SDL_CFLAGS) $< $(B)/libpmark.a $(SDL_LIBS) $(LDLIBS) -o $@

$(B)/test_core: tests/test_core.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -Isrc $< $(B)/libpmark.a $(LDLIBS) -o $@

test: $(B)/test_core $(B)/pmark
	$(B)/test_core
	sh tests/cli_smoke.sh $(B)/pmark

asan:
	$(MAKE) clean
	$(MAKE) test OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

STB := https://raw.githubusercontent.com/nothings/stb/master
vendor:
	for f in stb_image.h stb_image_write.h stb_truetype.h; do \
	  curl -sSfL $(STB)/$$f -o third_party/$$f || exit 1; done

clean:
	rm -rf $(B)
