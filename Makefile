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


all: tagplay tagview

tagplay: $(CORE_OBJ) $(PLAY_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(PLAY_OBJ) $(LDLIBS)

# the viewer links only the lean core deps: pcre2, pthread, m
tagview: $(CORE_OBJ) $(VIEW_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) $(VIEW_OBJ) $(shell pkg-config --libs libpcre2-8) -lpthread -lm

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f tagplay tagview src/core/*.o src/play/*.o src/view/*.o

install: tagplay
	install -m 755 tagplay $(HOME)/.local/bin/

.PHONY: clean install
