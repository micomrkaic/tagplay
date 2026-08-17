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

#ifndef TP_UTIL_H
#define TP_UTIL_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* growable byte/pointer vector */
typedef struct {
    void  *data;
    size_t len;    /* elements used */
    size_t cap;    /* elements allocated */
    size_t esz;    /* element size */
} vec;

void  vec_init(vec *v, size_t esz);
void *vec_push(vec *v, const void *elem);   /* returns slot */
void *vec_at(const vec *v, size_t i);
void  vec_free(vec *v);

char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *str_lower(const char *s);             /* malloc'd lowercase copy */
int   str_ieq(const char *a, const char *b);
const char *str_icasestr(const char *hay, const char *needle);

/* read whole file; returns malloc'd buffer, sets *len; NULL on error */
uint8_t *read_file(const char *path, size_t *len);

/* portable qsort_r: identical behavior on glibc, musl, macOS. Uses a
 * thread-local trampoline, so don't nest psort calls inside comparators. */
void psort(void *base, size_t n, size_t esz,
           int (*cmp)(const void *, const void *, void *), void *arg);

void util_mkdirs_for(const char *path); /* mkdir -p dirname(path) */

void fmt_duration(double secs, char *out, size_t outsz);      /* 3:07 or 1:02:03 */
void fmt_duration_long(double secs, char *out, size_t outsz); /* 3h12m */

#endif
