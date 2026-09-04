/* This file is part of tagplay.
 * Copyright (C) 2026  Mico
 * GPL-3.0-or-later; see COPYING.
 */
#ifndef TP_ART_H
#define TP_ART_H
#include <stddef.h>
#include <stdint.h>
#include "track.h"

/* embedded cover art (FLAC PICTURE / ID3 APIC), malloc'd; NULL if none */
uint8_t *art_extract(const char *path, audio_fmt fmt, size_t *len);
/* colored-ASCII render to stdout; 0 ok, -1 undecodable */
int art_render_ascii(const uint8_t *img, size_t len, int max_cols, int max_rows);

#endif
