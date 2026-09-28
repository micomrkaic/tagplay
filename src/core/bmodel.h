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

/* The browse MODEL: query text, live result, selection, grouping,
 * sorting -- pure state and operations, no terminal, no rendering.
 * The TUI browser and the SDL GUI are two views of this one struct. */

#ifndef TP_BMODEL_H
#define TP_BMODEL_H

#include "track.h"
#include "util.h"
#include <stddef.h>

typedef struct bmodel {
    char   buf[1024];        /* query text */
    size_t len, cur;         /* content length, cursor */
    vec    match;            /* size_t table indices, current result */
    vec    last_good;        /* last successfully parsed result */
    int    parse_ok;
    char   sortspec[128];
    char   group[32];        /* tag key to group by; "" = off */
    const table *tb;
    vec    sel;              /* selection, insertion order */
    int    sel_view;         /* :sel mode: match mirrors the marks */
} bmodel;

void bmodel_init(bmodel *m, const table *tb);
void bmodel_free(bmodel *m);
/* re-evaluate the query (tolerant); respects sel_view/sort/group */
void bmodel_rerun(bmodel *m);
long bmodel_sel_find(const bmodel *m, size_t ti);
void bmodel_sel_toggle(bmodel *m, size_t ti);
/* what a view should display: match if the parse succeeded, else the
 * last good result */
static inline const vec *bmodel_shown(const bmodel *m) {
    return m->parse_ok ? &m->match : &m->last_good;
}

#endif
