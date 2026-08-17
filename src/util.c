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

#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "tagplay: out of memory\n"); exit(1); }
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "tagplay: out of memory\n"); exit(1); }
    return q;
}
char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void vec_init(vec *v, size_t esz) { v->data = NULL; v->len = v->cap = 0; v->esz = esz; }
void *vec_push(vec *v, const void *elem) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->data = xrealloc(v->data, v->cap * v->esz);
    }
    void *slot = (char *)v->data + v->len * v->esz;
    if (elem) memcpy(slot, elem, v->esz);
    else memset(slot, 0, v->esz);
    v->len++;
    return slot;
}
void *vec_at(const vec *v, size_t i) { return (char *)v->data + i * v->esz; }
void vec_free(vec *v) { free(v->data); v->data = NULL; v->len = v->cap = 0; }

char *str_lower(const char *s) {
    char *p = xstrdup(s);
    for (char *q = p; *q; q++) *q = (char)tolower((unsigned char)*q);
    return p;
}
int str_ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
const char *str_icasestr(const char *hay, const char *needle) {
    return strcasestr(hay, needle);
}

uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = xmalloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static _Thread_local struct {
    int (*cmp)(const void *, const void *, void *);
    void *arg;
} psort_ctx;
static int psort_tramp(const void *a, const void *b) {
    return psort_ctx.cmp(a, b, psort_ctx.arg);
}
void psort(void *base, size_t n, size_t esz,
           int (*cmp)(const void *, const void *, void *), void *arg) {
    psort_ctx.cmp = cmp;
    psort_ctx.arg = arg;
    qsort(base, n, esz, psort_tramp);
}

#include <sys/stat.h>
void util_mkdirs_for(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
}

/* CP1252 0x80-0x9F -> Unicode codepoints (0 = undefined -> space) */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};
char *cp1252_to_utf8(const uint8_t *p, size_t n) {
    char *out = xmalloc(n * 3 + 1);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = p[i];
        if (c >= 0x80 && c <= 0x9F) c = cp1252_hi[c - 0x80];
        if (c == 0) { if (p[i] == 0) out[w++] = 0; else out[w++] = ' '; continue; }
        if (c < 0x80) out[w++] = (char)c;
        else if (c < 0x800) {
            out[w++] = (char)(0xC0 | c >> 6);
            out[w++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[w++] = (char)(0xE0 | c >> 12);
            out[w++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[w++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[w] = 0;
    return out;
}

void fmt_duration(double secs, char *out, size_t outsz) {
    long t = (long)(secs + 0.5);
    long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h) snprintf(out, outsz, "%ld:%02ld:%02ld", h, m, s);
    else   snprintf(out, outsz, "%ld:%02ld", m, s);
}
void fmt_duration_long(double secs, char *out, size_t outsz) {
    long t = (long)(secs + 0.5);
    long h = t / 3600, m = (t % 3600) / 60;
    if (h)      snprintf(out, outsz, "%ldh%02ldm", h, m);
    else if (m) snprintf(out, outsz, "%ldm", m);
    else        snprintf(out, outsz, "%lds", t);
}
