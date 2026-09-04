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

#include "scan.h"
#include "app.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- sorted index over cached tracks for O(log n) path lookup --- */
typedef struct { const table *tb; vec idx; } cache_idx;

static int cmp_pathidx(const void *a, const void *b, void *arg) {
    const table *tb = arg;
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    return strcmp(table_at(tb, ia)->path, table_at(tb, ib)->path);
}

static void cidx_build(cache_idx *ci, const table *tb) {
    ci->tb = tb;
    vec_init(&ci->idx, sizeof(size_t));
    if (!tb) return;
    for (size_t i = 0; i < table_len(tb); i++) vec_push(&ci->idx, &i);
    tp_sort(ci->idx.data, ci->idx.len, sizeof(size_t), cmp_pathidx, (void *)tb);
}

static const track *cidx_find(const cache_idx *ci, const char *path) {
    if (!ci->tb) return NULL;
    size_t lo = 0, hi = ci->idx.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t ti = *(size_t *)vec_at((vec *)&ci->idx, mid);
        int c = strcmp(table_at(ci->tb, ti)->path, path);
        if (c == 0) return table_at(ci->tb, ti);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static void track_copy(track *dst, const track *src) {
    dst->path = xstrdup(src->path);
    dst->mtime = src->mtime;
    dst->fsize = src->fsize;
    dst->fmt = src->fmt;
    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->duration = src->duration;
    for (size_t i = 0; i < src->tags.len; i++) {
        tagkv *kv = vec_at((vec *)&src->tags, i);
        track_add_tag(dst, kv->key, kv->value);
    }
}


static size_t scan_rec(const char *dir, table *out, const cache_idx *ci) {
    size_t parsed = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            parsed += scan_rec(path, out, ci);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;

        /* cheap app pre-filter to avoid probing every file */
        if (!APP->want_path(e->d_name)) continue;

        const track *c = cidx_find(ci, path);
        if (c && c->mtime == (int64_t)st.st_mtime && c->fsize == (int64_t)st.st_size) {
            track_copy(table_add(out), c);
            continue;
        }
        int f = APP->probe(path);
        if (f < 0) continue;
        track *t = table_add(out);
        t->path = xstrdup(path);
        t->mtime = (int64_t)st.st_mtime;
        t->fsize = (int64_t)st.st_size;
        int rc = APP->read_item(t, f);
        if (rc) { /* unreadable: drop the slot */
            track_free(t);
            out->tracks.len--;
            continue;
        }
        parsed++;
    }
    closedir(d);
    return parsed;
}

size_t scan_dir(const char *root, table *out, const table *cached) {
    cache_idx ci;
    cidx_build(&ci, cached);
    size_t n = scan_rec(root, out, &ci);
    vec_free(&ci.idx);
    return n;
}
