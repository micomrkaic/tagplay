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

/* The app descriptor: everything the shared core (scanner, cache,
 * query engine, browser) must ask the application instead of assuming.
 * tagplay fills this with audio answers; tagview will fill it with
 * photo answers. One binary, one descriptor, set once at startup. */

#ifndef TP_APP_H
#define TP_APP_H

#include "track.h"
#include <stddef.h>
#include <stdint.h>

/* app-defined numeric pseudo-fields for the query language
 * (play: length, rate, channels; view: width, height, ...) */
typedef struct pseudo_field {
    const char *name;
    double    (*value)(const track *t);
} pseudo_field;

typedef struct tp_app {
    const char *name;            /* "tagplay" / "tagview" */
    const char *cache_file;      /* basename under ~/.cache/<name>/ */
    unsigned    cache_version;   /* app salt on top of core version */

    /* -- scanning -- */
    int  (*want_path)(const char *fname);       /* extension pre-filter */
    int  (*probe)(const char *path);            /* app format id, or -1 */
    int  (*read_item)(track *t, int fmt);       /* fill tags; 0 = ok */

    /* -- item semantics -- */
    int         (*cacheable)(const track *t);   /* persist in the cache? */
    const char *(*fmt_name)(int fmt);           /* "flac", "jpeg", ... */
    const pseudo_field *fields;                 /* numeric pseudo-fields */
    size_t               nfields;

    /* -- embedded art (inspector / 'a' view); NULL if none -- */
    uint8_t *(*art_read)(const track *t, size_t *len);
} tp_app;

/* set by the binary's main() before any core machinery runs */
extern const tp_app *APP;

#endif
