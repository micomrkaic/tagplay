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

#include "track.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void track_init(track *t) {
    memset(t, 0, sizeof *t);
    vec_init(&t->tags, sizeof(tagkv));
}
void track_free(track *t) {
    free(t->path);
    for (size_t i = 0; i < t->tags.len; i++) {
        tagkv *kv = vec_at(&t->tags, i);
        free(kv->key);
        free(kv->value);
    }
    vec_free(&t->tags);
}
void track_add_tag(track *t, const char *key, const char *value) {
    if (!key || !value || !*value) return;
    tagkv kv;
    kv.key = xstrdup(key);
    for (char *p = kv.key; *p; p++) *p = (char)toupper((unsigned char)*p);
    /* Sanitize: tags come from arbitrary files and are printed raw into a
     * terminal. Control bytes (CR, LF, ESC, ...) would corrupt the display
     * or worse; map them to spaces, then trim. Tabs too. */
    kv.value = xstrdup(value);
    for (char *p = kv.value; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F) *p = ' ';
    size_t n = strlen(kv.value);
    while (n && kv.value[n - 1] == ' ') kv.value[--n] = 0;
    char *st = kv.value;
    while (*st == ' ') st++;
    if (st != kv.value) memmove(kv.value, st, strlen(st) + 1);
    if (!kv.value[0]) { free(kv.key); free(kv.value); return; }
    vec_push(&t->tags, &kv);
}
size_t track_get_tags(const track *t, const char *key, const char **out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < t->tags.len && n < max; i++) {
        tagkv *kv = vec_at((vec *)&t->tags, i);
        if (str_ieq(kv->key, key)) out[n++] = kv->value;
    }
    return n;
}
const char *track_first_tag(const track *t, const char *key) {
    for (size_t i = 0; i < t->tags.len; i++) {
        tagkv *kv = vec_at((vec *)&t->tags, i);
        if (str_ieq(kv->key, key)) return kv->value;
    }
    return NULL;
}
const char *fmt_name(audio_fmt f) {
    switch (f) {
    case FMT_FLAC: return "flac";
    case FMT_WAV:  return "wav";
    case FMT_MP3:  return "mp3";
    default:       return "?";
    }
}

void table_init(table *tb) { vec_init(&tb->tracks, sizeof(track)); }
void table_free(table *tb) {
    for (size_t i = 0; i < tb->tracks.len; i++) track_free(vec_at(&tb->tracks, i));
    vec_free(&tb->tracks);
}
track *table_add(table *tb) {
    track *t = vec_push(&tb->tracks, NULL);
    track_init(t);
    return t;
}
size_t table_len(const table *tb) { return tb->tracks.len; }
track *table_at(const table *tb, size_t i) { return vec_at((vec *)&tb->tracks, i); }
