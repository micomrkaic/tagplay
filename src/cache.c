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

#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define CACHE_MAGIC   "TGP1"
#define CACHE_VERSION 3u /* v3: CP1252 decoding, C1 stripped */

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void wf8(FILE *f, double v)   { fwrite(&v, 8, 1, f); }
static void wstr(FILE *f, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    w32(f, n);
    fwrite(s, 1, n, f);
}
static int r32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1 ? 0 : -1; }
static int r64(FILE *f, uint64_t *v) { return fread(v, 8, 1, f) == 1 ? 0 : -1; }
static int rf8(FILE *f, double *v)   { return fread(v, 8, 1, f) == 1 ? 0 : -1; }
static char *rstr(FILE *f) {
    uint32_t n;
    if (r32(f, &n) || n > (1u << 20)) return NULL;
    char *s = xmalloc(n + 1);
    if (fread(s, 1, n, f) != n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

int cache_save(const char *path, const table *tb) {
    util_mkdirs_for(path);
    char tmppath[4096];
    snprintf(tmppath, sizeof tmppath, "%s.tmp", path);
    FILE *f = fopen(tmppath, "wb");
    if (!f) return -1;
    fwrite(CACHE_MAGIC, 1, 4, f);
    w32(f, CACHE_VERSION);
    w32(f, (uint32_t)table_len(tb));
    for (size_t i = 0; i < table_len(tb); i++) {
        const track *t = table_at(tb, i);
        wstr(f, t->path);
        w64(f, (uint64_t)t->mtime);
        w64(f, (uint64_t)t->fsize);
        w32(f, (uint32_t)t->fmt);
        w32(f, t->sample_rate);
        w32(f, t->channels);
        wf8(f, t->duration);
        w32(f, (uint32_t)t->tags.len);
        for (size_t j = 0; j < t->tags.len; j++) {
            tagkv *kv = vec_at((vec *)&t->tags, j);
            wstr(f, kv->key);
            wstr(f, kv->value);
        }
    }
    if (fclose(f)) { remove(tmppath); return -1; }
    return rename(tmppath, path);
}

int cache_load(const char *path, table *tb) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4];
    uint32_t ver, count;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, CACHE_MAGIC, 4) ||
        r32(f, &ver) || ver != CACHE_VERSION || r32(f, &count)) {
        fclose(f);
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        track *t = table_add(tb);
        uint64_t u;
        uint32_t x, ntags;
        if (!(t->path = rstr(f))) goto corrupt;
        if (r64(f, &u)) goto corrupt;
        t->mtime = (int64_t)u;
        if (r64(f, &u)) goto corrupt;
        t->fsize = (int64_t)u;
        if (r32(f, &x)) goto corrupt;
        t->fmt = (audio_fmt)x;
        if (r32(f, &t->sample_rate)) goto corrupt;
        if (r32(f, &t->channels)) goto corrupt;
        if (rf8(f, &t->duration)) goto corrupt;
        if (r32(f, &ntags) || ntags > 4096) goto corrupt;
        for (uint32_t j = 0; j < ntags; j++) {
            char *k = rstr(f), *v = k ? rstr(f) : NULL;
            if (!k || !v) { free(k); free(v); goto corrupt; }
            track_add_tag(t, k, v);
            free(k); free(v);
        }
    }
    fclose(f);
    return 0;
corrupt:
    fclose(f);
    /* keep whatever loaded cleanly; caller rescans anyway */
    return -1;
}
