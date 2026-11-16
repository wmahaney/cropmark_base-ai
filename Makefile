# cropmark build. Targets:
#   make            build/cropmark: picture picker, editor window and batch
#                   mode in one program (needs SDL2, found with pkg-config)
#   make install    copy cropmark to $(PREFIX)/bin   (PREFIX=~/.local by default)
#   make uninstall  remove it again
#   make test       unit tests, batch smoke test, golden images, window selftests
#   make headless   build/cropmark-headless: batch mode only, no SDL2 needed
#   make test-headless   the tests that need no SDL2, against cropmark-headless
#   make asan       rebuild everything under ASan/UBSan and run the tests
#   make fuzz-smoke      mutation-fuzz the parsers (gcc + ASan, FUZZ_ITERS=N)
#   make fuzz-libfuzzer  coverage-guided fuzzing (needs clang)
#   make golden-update   regenerate tests/golden/*.png (review the diff!)
#   make vendor     re-download the stb headers into third_party/

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

.PHONY: all install uninstall headless test test-headless asan vendor clean golden-update \
        fuzz-smoke fuzz-libfuzzer
all: $(B)/cropmark

PREFIX ?= $(HOME)/.local
APP_SRC  := cli/main.c cli/batch.c
GUI_SRC  := gui/gui.c gui/picker.c
APP_HDRS := $(HDRS) cli/cropmark.h gui/picker.h

$(B):
	mkdir -p $(B)

$(B)/pm_stb.o: src/pm_stb.c | $(B)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(B)/%.o: src/%.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@

$(B)/libpmark.a: $(CORE_OBJ)
	$(AR) rcs $@ $^

$(B)/cropmark: $(APP_SRC) $(GUI_SRC) $(B)/libpmark.a $(APP_HDRS)
	@test -n "$(SDL_LIBS)" || { echo "SDL2 not found: install it (apt install libsdl2-dev," \
	  "dnf install SDL2-devel, brew install sdl2), or run 'make headless' for batch mode only"; exit 1; }
	$(CC) $(CFLAGS) $(WARN) -Icli -Igui $(SDL_CFLAGS) $(APP_SRC) $(GUI_SRC) $(B)/libpmark.a \
	  $(SDL_LIBS) $(LDLIBS) -o $@

headless: $(B)/cropmark-headless
$(B)/cropmark-headless: $(APP_SRC) $(B)/libpmark.a $(HDRS) cli/cropmark.h
	$(CC) $(CFLAGS) $(WARN) -DCM_NO_GUI -Icli $(APP_SRC) $(B)/libpmark.a $(LDLIBS) -o $@

install: $(B)/cropmark
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(B)/cropmark $(DESTDIR)$(PREFIX)/bin/cropmark
	@case ":$$PATH:" in *":$(PREFIX)/bin:"*) ;; \
	  *) echo "note: $(PREFIX)/bin is not on your PATH";; esac

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cropmark

$(B)/test_core: tests/test_core.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -Isrc $< $(B)/libpmark.a $(LDLIBS) -o $@

$(B)/imgdiff: tests/imgdiff.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $< $(B)/libpmark.a $(LDLIBS) -o $@

test: $(B)/test_core $(B)/cropmark $(B)/imgdiff
	$(B)/test_core
	sh tests/cli_smoke.sh $(B)/cropmark
	sh tests/golden.sh $(B)/cropmark $(B)/imgdiff
	sh tests/gui_selftest.sh $(B)/cropmark

test-headless: $(B)/test_core $(B)/cropmark-headless $(B)/imgdiff
	$(B)/test_core
	sh tests/cli_smoke.sh $(B)/cropmark-headless
	sh tests/golden.sh $(B)/cropmark-headless $(B)/imgdiff

golden-update: $(B)/cropmark $(B)/imgdiff
	sh tests/golden.sh $(B)/cropmark $(B)/imgdiff update

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

STB := https://raw.githubusercontent.com/nothings/stb/master
vendor:
	for f in stb_image.h stb_image_write.h stb_truetype.h; do \
	  curl -sSfL $(STB)/$$f -o third_party/$$f || exit 1; done

clean:
	rm -rf $(B)
