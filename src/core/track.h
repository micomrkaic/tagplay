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

#ifndef TP_TRACK_H
#define TP_TRACK_H
#include <stdint.h>
#include <stddef.h>
#include "util.h"

typedef struct {
    char *key;    /* uppercased canonical key, e.g. "ARTIST" */
    char *value;  /* UTF-8 */
} tagkv;

/* format is an app-defined id (see app.h); core treats it opaquely */

typedef struct {
    char     *path;
    int64_t   mtime;
    int64_t   fsize;
    int fmt;
    uint32_t  sample_rate;
    uint32_t  channels;
    double    duration;   /* seconds; 0 if unknown */
    vec       tags;       /* vec of tagkv, multi-valued: keys may repeat */
} track;

void track_init(track *t);
void track_free(track *t);
void track_add_tag(track *t, const char *key, const char *value);
/* all values for key (case-insensitive); returns count, fills out[] up to max */
size_t track_get_tags(const track *t, const char *key, const char **out, size_t max);
const char *track_first_tag(const track *t, const char *key); /* or NULL */

typedef struct {
    vec tracks; /* vec of track */
} table;

void   table_init(table *tb);
void   table_free(table *tb);
track *table_add(table *tb);            /* returns new zeroed+init'd track */
size_t table_len(const table *tb);
track *table_at(const table *tb, size_t i);

#endif
