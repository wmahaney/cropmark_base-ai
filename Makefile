# cropmark (pmark for short) build. Targets:
#   make            build/cropmark: picture picker, editor window and
#                   command-line mode in one program (needs SDL2, via pkg-config)
#   make install    install under $(PREFIX) (~/.local by default): the program,
#                   a `pmark` alias, and an applications-menu entry with icon
#   make desktop-shortcut   also put a cropmark launcher on the desktop
#   make uninstall  remove all of the above
#   make test       unit tests, batch smoke test, golden images, window selftests
#   make headless   build/cropmark-headless: command-line mode only, no SDL2
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

.PHONY: all install uninstall desktop-shortcut headless test test-headless asan vendor clean golden-update \
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
	  "dnf install SDL2-devel, brew install sdl2), or run 'make headless' for command-line mode only"; exit 1; }
	$(CC) $(CFLAGS) $(WARN) -Icli -Igui $(SDL_CFLAGS) $(APP_SRC) $(GUI_SRC) $(B)/libpmark.a \
	  $(SDL_LIBS) $(LDLIBS) -o $@

headless: $(B)/cropmark-headless
$(B)/cropmark-headless: $(APP_SRC) $(B)/libpmark.a $(HDRS) cli/cropmark.h
	$(CC) $(CFLAGS) $(WARN) -DCM_NO_GUI -Icli $(APP_SRC) $(B)/libpmark.a $(LDLIBS) -o $@

# --------------------------------------------------------------- install
BINDIR  = $(PREFIX)/bin
APPDIR  = $(PREFIX)/share/applications
ICONDIR = $(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOP_DIR ?= $(shell xdg-user-dir DESKTOP 2>/dev/null || echo "$(HOME)/Desktop")

# The menu entry names the installed program and icon by absolute path, so
# it works even when $(BINDIR) is not on the desktop session's PATH.
$(B)/cropmark.desktop: data/cropmark.desktop.in Makefile | $(B)
	sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@ICON@|$(ICONDIR)/cropmark.svg|g' $< > $@

install: $(B)/cropmark $(B)/cropmark.desktop
	mkdir -p "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(APPDIR)" "$(DESTDIR)$(ICONDIR)"
	cp $(B)/cropmark "$(DESTDIR)$(BINDIR)/cropmark"
	ln -sf cropmark "$(DESTDIR)$(BINDIR)/pmark"
	cp data/cropmark.svg "$(DESTDIR)$(ICONDIR)/cropmark.svg"
	cp $(B)/cropmark.desktop "$(DESTDIR)$(APPDIR)/cropmark.desktop"
	@if [ -z "$(DESTDIR)" ] && command -v update-desktop-database >/dev/null; then \
	  update-desktop-database -q "$(APPDIR)" || true; fi
	@case ":$$PATH:" in *":$(BINDIR):"*) ;; \
	  *) echo "note: $(BINDIR) is not on your PATH, so type its full path to run cropmark";; esac
	@echo "Installed cropmark. It is in your applications menu;" \
	  "'make desktop-shortcut' also puts it on the desktop."

desktop-shortcut: install
	mkdir -p "$(DESKTOP_DIR)"
	cp $(B)/cropmark.desktop "$(DESKTOP_DIR)/cropmark.desktop"
	chmod +x "$(DESKTOP_DIR)/cropmark.desktop"
	@# GNOME's desktop icons only launch entries marked trusted
	@command -v gio >/dev/null && gio set "$(DESKTOP_DIR)/cropmark.desktop" \
	  metadata::trusted true 2>/dev/null || true

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/cropmark" "$(DESTDIR)$(BINDIR)/pmark" \
	  "$(DESTDIR)$(APPDIR)/cropmark.desktop" "$(DESTDIR)$(ICONDIR)/cropmark.svg" \
	  "$(DESKTOP_DIR)/cropmark.desktop"

$(B)/test_core: tests/test_core.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -Isrc $< $(B)/libpmark.a $(LDLIBS) -o $@

$(B)/imgdiff: tests/imgdiff.c $(B)/libpmark.a $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $< $(B)/libpmark.a $(LDLIBS) -o $@

test: $(B)/test_core $(B)/cropmark $(B)/imgdiff
	$(B)/test_core
	sh tests/cli_smoke.sh $(B)/cropmark
	sh tests/golden.sh $(B)/cropmark $(B)/imgdiff
	sh tests/gui_selftest.sh $(B)/cropmark
	sh tests/install_check.sh

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
