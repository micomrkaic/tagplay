# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.

CC      ?= cc
CFLAGS  += -Isrc/core -Isrc/play
CFLAGS  += -std=c17 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
CFLAGS  += $(shell pkg-config --cflags flac libpcre2-8 sdl2 libcurl)
# optional AAC radio support, auto-detected (Debian/Ubuntu: libfaad-dev)
HAVE_FAAD := $(shell test -e /usr/include/neaacdec.h -o -e /usr/local/include/neaacdec.h && echo 1)
ifeq ($(HAVE_FAAD),1)
CFLAGS  += -DHAVE_FAAD
FAAD_LIB = -lfaad
endif

LDLIBS  += $(shell pkg-config --libs flac libpcre2-8 sdl2 libcurl) $(FAAD_LIB) -lpthread -lm

SRC := $(wildcard src/core/*.c) $(wildcard src/play/*.c)
OBJ := $(SRC:.c=.o)

tagplay: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f tagplay src/core/*.o src/play/*.o

install: tagplay
	install -m 755 tagplay $(HOME)/.local/bin/

.PHONY: clean install
