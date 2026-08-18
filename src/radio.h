/* This file is part of tagplay.
 * Copyright (C) 2026  Mico
 * GPL-3.0-or-later; see COPYING.
 */
#ifndef TP_RADIO_H
#define TP_RADIO_H
#include <stddef.h>
#include <stdint.h>

typedef struct radio_stream radio_stream;

radio_stream *radio_open(const char *url);
/* de-ICY'd audio bytes; >0 = bytes, 0 = nothing yet (retry), -1 = ended */
long radio_read(radio_stream *r, uint8_t *buf, size_t max, int timeout_ms);
int  radio_title(radio_stream *r, char *out, size_t sz); /* 1 if present */
void radio_close(radio_stream *r);

#endif
