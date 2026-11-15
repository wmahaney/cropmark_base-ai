# pmark build. Targets:
#   make          core library + CLI           (no dependencies beyond libc/libm)
#   make gui      SDL2 frontend                (needs sdl2 via pkg-config)
#   make test     unit tests, CLI smoke test, golden images
#   make test-gui headless GUI selftest (needs SDL2; uses SDL's dummy driver)
#   make asan     rebuild everything under ASan/UBSan and run the tests
#   make fuzz-smoke      mutation-fuzz the parsers (gcc + ASan, FUZZ_ITERS=N)
#   make fuzz-libfuzzer  coverage-guided fuzzing (needs clang)
#   make golden-update   regenerate tests/golden/*.png (review the diff!)
#   make vendor   re-download the stb headers into third_party/

CC      ?= cc
OPT     ?= -O2 -g
WARN    := -Wall -Wextra -Wpedantic -Wshadow
CFLAGS  += -std=c11 $(OPT) -Iinclude -Ithird_party
LDLIBS  += -lm
B       := build

CORE_SRC := src/pm_image.c src/pm_file.c src/pm_exif.c src/pm_icc.c src/pm_draw.c src/pm_text.c src/pm_doc.c src/pm_session.c
CORE_OBJ := $(CORE_SRC:src/%.c=$(B)/%.o) $(B)/pm_stb.o
HDRS     := include/pmark.h src/pm_internal.h

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

.PHONY: all gui test test-gui asan vendor clean golden-update fuzz-smoke fuzz-libfuzzer
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

$(B)/imgdiff: tests/imgdiff.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $< $(B)/libpmark.a $(LDLIBS) -o $@

test: $(B)/test_core $(B)/pmark $(B)/imgdiff
	$(B)/test_core
	sh tests/cli_smoke.sh $(B)/pmark
	sh tests/golden.sh $(B)

test-gui: $(B)/pmark-gui
	@T=$$(mktemp -d) && cp tests/data/sample.png $$T/photo.png && cp tests/data/sample.png $$T/photo-marked.png && \
	SDL_VIDEODRIVER=dummy $(B)/pmark-gui $$T/photo.png --selftest $$T/renamed; rc=$$?; rm -rf $$T; exit $$rc

golden-update: $(B)/pmark $(B)/imgdiff
	sh tests/golden.sh $(B) update

FUZZ_ITERS ?= 20000
FUZZ_SEEDS := tests/data/sample.png tests/data/sample_exif6.jpg $(wildcard tests/fuzz_seeds/*)
FUZZ_SRC   := $(CORE_SRC) src/pm_stb.c tests/fuzz.c
fuzz-smoke:
	@mkdir -p $(B)
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude -Ithird_party -Isrc -w $(FUZZ_SRC) -lm -o $(B)/fuzz
	$(B)/fuzz $(FUZZ_ITERS) $(FUZZ_SEEDS)

fuzz-libfuzzer:
	@mkdir -p $(B)/corpus
	clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined -DPM_LIBFUZZER \
	  -Iinclude -Ithird_party -Isrc -w $(FUZZ_SRC) -lm -o $(B)/fuzz-lf
	$(B)/fuzz-lf $(B)/corpus tests/data tests/fuzz_seeds -max_len=200000

asan:
	$(MAKE) clean
	$(MAKE) test OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
	@if [ -n "$(SDL_LIBS)" ]; then ASAN_OPTIONS=detect_leaks=0 $(MAKE) test-gui \
	  OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"; fi

STB := https://raw.githubusercontent.com/nothings/stb/master
vendor:
	for f in stb_image.h stb_image_write.h stb_truetype.h; do \
	  curl -sSfL $(STB)/$$f -o third_party/$$f || exit 1; done

clean:
	rm -rf $(B)
