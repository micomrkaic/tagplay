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

#ifndef TP_QUERY_H
#define TP_QUERY_H
#include "track.h"

typedef struct qnode qnode;

/* Parse a query string. tolerant=1: auto-close quotes/parens (for live typing).
 * Returns NULL on unparseable input (caller keeps previous result). */
qnode *query_parse(const char *src, int tolerant);
void   query_free(qnode *q);
int    query_eval(const qnode *q, const track *t); /* 1 = match */

/* convenience: run over table, fill match indices (vec of size_t) */
void query_run(const qnode *q, const table *tb, vec *out_idx);

/* sort a vec of size_t indices by comma-separated fields, '-' prefix = desc */
void query_sort(const table *tb, vec *idx, const char *fields);

#endif
