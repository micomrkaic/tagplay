# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.

CC      ?= cc
CFLAGS  += -std=c17 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE
CFLAGS  += $(shell pkg-config --cflags flac libpcre2-8 sdl2 libcurl)
LDLIBS  += $(shell pkg-config --libs flac libpcre2-8 sdl2 libcurl) -lpthread -lm

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

tagplay: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f tagplay src/*.o

install: tagplay
	install -m 755 tagplay $(HOME)/.local/bin/

.PHONY: clean install
