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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "track.h"
#include "scan.h"
#include "cache.h"
#include "query.h"
#include "repl.h"
#include "player.h"
#include "decoder.h"

static void default_cache_path(char *out, size_t sz) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) snprintf(out, sz, "%s/tagplay/cache.bin", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, sz, "%s/.cache/tagplay/cache.bin", home ? home : ".");
    }
}

static void usage(void) {
    fprintf(stderr,
        "usage: tagplay [options] MUSIC_DIR...\n"
        "  -q EXPR     one-shot query, print matching paths, exit\n"
        "  -s FIELDS   sort spec for -q (e.g. year,album,track; -field = desc)\n"
        "  -t          with -q: print artist/title/album/length columns\n"
        "  -C PATH     cache file (default ~/.cache/tagplay/cache.bin)\n"
        "  -n          no cache (scan fresh, don't write)\n"
        "  -D FILE     debug: decode FILE fully, print stats, exit\n"
        "  -T FILE     debug: dump FILE's tags (escaped), duration, exit\n");
    exit(2);
}

static int debug_decode(const char *path) {
    audio_fmt f = FMT_UNKNOWN;
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".flac")) f = FMT_FLAC;
    else if (dot && !strcasecmp(dot, ".wav")) f = FMT_WAV;
    else if (dot && !strcasecmp(dot, ".mp3")) f = FMT_MP3;
    decoder *d = decoder_open(path, f);
    if (!d) { fprintf(stderr, "decode open failed: %s\n", path); return 1; }
    float buf[4096 * 8];
    long total = 0, n;
    float peak = 0;
    while ((n = decoder_read(d, buf, 4096)) > 0) {
        for (long i = 0; i < n * decoder_channels(d); i++) {
            float a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        total += n;
    }
    printf("%s: %d Hz, %d ch, %ld frames = %.3f s (hdr said %.3f), peak %.3f\n",
           path, decoder_rate(d), decoder_channels(d), total,
           (double)total / decoder_rate(d), decoder_duration(d), peak);
    /* seek test: to 0.5 s, read one chunk */
    if (decoder_seek(d, 0.5) == 0) {
        n = decoder_read(d, buf, 4096);
        printf("  seek(0.5s) ok, next read %ld frames, pos now %.3f s\n",
               n, decoder_position(d));
    } else printf("  seek failed\n");
    decoder_close(d);
    return 0;
}

#include "tags.h"
static int debug_tags(const char *path) {
    track t;
    track_init(&t);
    t.path = xstrdup(path);
    FILE *f = fopen(path, "rb");
    uint8_t m[12] = { 0 };
    if (f) { (void)!fread(m, 1, sizeof m, f); fclose(f); }
    int rc;
    if (!memcmp(m, "fLaC", 4)) rc = tags_read_flac(&t);
    else if (!memcmp(m, "RIFF", 4)) rc = tags_read_wav(&t);
    else rc = tags_read_mp3(&t);
    printf("magic: %02x %02x %02x %02x  ('%c%c%c')   parse rc=%d\n",
           m[0], m[1], m[2], m[3],
           m[0] >= 32 && m[0] < 127 ? m[0] : '.',
           m[1] >= 32 && m[1] < 127 ? m[1] : '.',
           m[2] >= 32 && m[2] < 127 ? m[2] : '.', rc);
    printf("format=%s rate=%u ch=%u duration=%.3f s\n",
           fmt_name(t.fmt), t.sample_rate, t.channels, t.duration);
    for (size_t i = 0; i < t.tags.len; i++) {
        tagkv *kv = vec_at(&t.tags, i);
        printf("  %s = ", kv->key);
        for (const char *p = kv->value; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 32 || c == 127) printf("\\x%02x", c);
            else putchar(c);
        }
        printf("\n");
    }
    track_free(&t);
    return 0;
}

int main(int argc, char **argv) {
    const char *qexpr = NULL, *sortspec = NULL;
    char cachepath[4096];
    int use_cache = 1, tabular = 0;
    default_cache_path(cachepath, sizeof cachepath);

    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-q") && i + 1 < argc) qexpr = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sortspec = argv[++i];
        else if (!strcmp(argv[i], "-C") && i + 1 < argc)
            snprintf(cachepath, sizeof cachepath, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-n")) use_cache = 0;
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) return debug_decode(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) return debug_tags(argv[++i]);
        else if (!strcmp(argv[i], "-t")) tabular = 1;
        else usage();
    }
    if (i >= argc) usage();

    table cached, tb;
    table_init(&cached);
    table_init(&tb);
    if (use_cache) cache_load(cachepath, &cached);

    size_t parsed = 0;
    for (; i < argc; i++) parsed += scan_dir(argv[i], &tb, &cached);
    table_free(&cached);

    size_t stations_load(table *tb);
    stations_load(&tb);

    if (table_len(&tb) == 0) {
        fprintf(stderr, "tagplay: no audio files found\n");
        return 1;
    }
    if (use_cache && parsed > 0) cache_save(cachepath, &tb);

    if (qexpr) {
        qnode *q = NULL;
        if (qexpr[0]) {
            q = query_parse(qexpr, 0 /* strict */);
            if (!q) {
                fprintf(stderr, "tagplay: cannot parse query: %s\n", qexpr);
                return 2;
            }
        }
        vec idx;
        vec_init(&idx, sizeof(size_t));
        query_run(q, &tb, &idx);
        if (sortspec) query_sort(&tb, &idx, sortspec);
        for (size_t k = 0; k < idx.len; k++) {
            const track *t = table_at(&tb, *(size_t *)vec_at(&idx, k));
            if (tabular) {
                char dur[16];
                fmt_duration(t->duration, dur, sizeof dur);
                const char *a = track_first_tag(t, "ARTIST");
                const char *ti = track_first_tag(t, "TITLE");
                const char *al = track_first_tag(t, "ALBUM");
                printf("%-24.24s\t%-36.36s\t%-24.24s\t%s\t%s\n",
                       a ? a : "?", ti ? ti : "?", al ? al : "?", dur, t->path);
            } else {
                printf("%s\n", t->path);
            }
        }
        fprintf(stderr, "%zu tracks\n", idx.len);
        query_free(q);
        vec_free(&idx);
    } else {
        player *pl = player_create(&tb);
        repl_run(&tb, pl);
        player_destroy(pl);
    }
    table_free(&tb);
    return 0;
}
