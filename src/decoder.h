/* This file is part of tagplay.
 *
 * tagplay -- search-driven music player with audiotard DSP
 * Copyright (C) 2026  Mico
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef TP_DECODER_H
#define TP_DECODER_H
#include "track.h"

/* Uniform decoder: all formats present interleaved float32 in [-1,1]. */
typedef struct decoder decoder;

decoder *decoder_open(const char *path, audio_fmt fmt); /* NULL on failure */
/* returns frames delivered (0 = end of stream, <0 = error) */
long decoder_read(decoder *d, float *buf, long max_frames);
int  decoder_seek(decoder *d, double seconds);           /* 0 ok */
void decoder_close(decoder *d);

int      decoder_rate(const decoder *d);
int      decoder_channels(const decoder *d);
double   decoder_duration(const decoder *d);             /* seconds, 0 unknown */
double   decoder_position(const decoder *d);             /* seconds */

#endif
