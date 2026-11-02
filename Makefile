# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.

.DEFAULT_GOAL := all
CC      ?= cc
CFLAGS  += -Isrc/core -Isrc/play
CFLAGS  += -std=c17 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
CFLAGS  += $(shell pkg-config --cflags flac libpcre2-8 sdl2 libcurl)
# optional AAC radio support, auto-detected
# (Debian/Ubuntu: libfaad-dev; macOS: brew install faad2)
FAAD_HDR := $(firstword $(wildcard /usr/include/neaacdec.h \
                                   /usr/local/include/neaacdec.h \
                                   /opt/homebrew/include/neaacdec.h))
ifneq ($(FAAD_HDR),)
CFLAGS  += -DHAVE_FAAD -I$(dir $(FAAD_HDR))
FAAD_LIB = -L$(dir $(FAAD_HDR))../lib -lfaad
endif

LDLIBS  += $(shell pkg-config --libs flac libpcre2-8 sdl2 libcurl) $(FAAD_LIB) -lpthread -lm

CORE_SRC := $(wildcard src/core/*.c)
PLAY_SRC := $(wildcard src/play/*.c)
VIEW_SRC := $(wildcard src/view/*.c)
CORE_OBJ := $(CORE_SRC:.c=.o)
PLAY_OBJ := $(PLAY_SRC:.c=.o)
VIEW_OBJ := $(VIEW_SRC:.c=.o)
GUI_SRC := $(wildcard src/gui/*.c)
GUI_OBJ := $(GUI_SRC:.c=.o)
PLAY_NOMAIN := $(filter-out src/play/main.o,$(PLAY_OBJ))


all: tagplay tagview tagplay-gui

tagplay: $(CORE_OBJ) $(PLAY_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(PLAY_OBJ) $(LDLIBS)

# the viewer links only the lean core deps: pcre2, pthread, m
tagplay-gui: $(CORE_OBJ) $(PLAY_NOMAIN) $(GUI_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(PLAY_NOMAIN) $(GUI_OBJ) $(LDLIBS)

tagview: $(CORE_OBJ) $(VIEW_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(VIEW_OBJ) $(shell pkg-config --libs libpcre2-8) -lpthread -lm

HDRS := $(wildcard src/core/*.h src/play/*.h src/view/*.h src/gui/*.h)
src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f tagplay tagview tagplay-gui src/core/*.o src/play/*.o src/view/*.o src/gui/*.o

# ---- install ---------------------------------------------------------
# Binaries are self-contained (font embedded; stations and cache live
# under XDG dirs), so an install is the three executables plus the man
# pages. Default PREFIX is per-user; override for a system install:
#   sudo make install PREFIX=/usr/local
PREFIX ?= $(HOME)/.local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
MANDIR  = $(DESTDIR)$(PREFIX)/share/man/man1

install: all
	install -d $(BINDIR) $(MANDIR)
	install -m 755 tagplay tagview tagplay-gui $(BINDIR)/
	install -m 644 man/tagplay.1 man/tagview.1 man/tagplay-gui.1 $(MANDIR)/
	@echo "installed: tagplay, tagview, tagplay-gui -> $(BINDIR)"
	@echo "man pages -> $(MANDIR)"
	@command -v tagplay >/dev/null 2>&1 || \
	  echo "note: $(PREFIX)/bin is not on your PATH"
	@man -w tagplay >/dev/null 2>&1 || \
	  echo "note: if 'man tagplay' fails, add: export MANPATH=\"$(PREFIX)/share/man:$$MANPATH\""

uninstall:
	rm -f $(BINDIR)/tagplay $(BINDIR)/tagview $(BINDIR)/tagplay-gui
	rm -f $(MANDIR)/tagplay.1 $(MANDIR)/tagview.1 $(MANDIR)/tagplay-gui.1
	@echo "removed from $(BINDIR) and $(MANDIR)"

.PHONY: clean install uninstall

# ---- the browser build (MG-d) --------------------------------------
# Needs emscripten (apt install emscripten) and the wasm deps:
#   ./tools/build_wasm_deps.sh
# Then:  make wasm  ->  web/tagplay.html (+ .js, .wasm, .worker.js)
# Serve with COOP/COEP headers:  python3 tools/serve_wasm.py
WASM_SRC := $(filter-out src/play/main.c src/play/radio.c, \
              $(wildcard src/core/*.c) $(wildcard src/play/*.c)) \
            $(wildcard src/gui/*.c)
# Debian's emscripten freezes its system cache; give the build its own
# writable one (ports land there on first use).
# our private EM_CONFIG exists only when the deps script decided the
# frozen Debian emscripten needs it; an emsdk install runs bare
EMENV := $(if $(wildcard third_party/wasm/emconfig),EM_CONFIG=$(PWD)/third_party/wasm/emconfig,)

wasm:
	$(EMENV) emcc -std=gnu17 -O2 -Isrc/core -Isrc/play -Isrc/gui \
	  -Ithird_party/wasm/include -D_GNU_SOURCE \
	  -pthread -sUSE_SDL=2 \
	  $(WASM_SRC) \
	  -Lthird_party/wasm/lib -lFLAC -lpcre2-8 \
	  -sUSE_PTHREADS=1 -sPTHREAD_POOL_SIZE=8 \
	  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 \
	  -sMAXIMUM_MEMORY=1073741824 \
	  -sEXPORTED_RUNTIME_METHODS=ccall,FS \
	  -o web/tagplay.js

.PHONY: wasm
