/* This file is part of tagplay/tagview.
 *
 * tagview -- search-driven image browser on the tagplay core
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
#include "browser.h"
#include "app.h"

static void default_cache_path(char *out, size_t sz) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) snprintf(out, sz, "%s/%s/%s", xdg, APP->name, APP->cache_file);
    else {
        const char *home = getenv("HOME");
        snprintf(out, sz, "%s/.cache/%s/%s", home ? home : ".",
                 APP->name, APP->cache_file);
    }
}

static void usage(void) {
    fprintf(stderr,
        "usage: tagview [options] PHOTO_DIR...\n"
        "  -q EXPR     one-shot query, print matching paths, exit\n"
        "  -s FIELDS   sort spec for -q (e.g. date,-width)\n"
        "  -t          with -q: print title/camera/date/dims columns\n"
        "  -C PATH     cache file (default ~/.cache/tagview/cache.bin)\n"
        "  -n          no cache (scan fresh, don't write)\n"
        "\n"
        "Interactive: type to filter (tag=alps & year>2000 & camera~nikon),\n"
        "Tab for the list, Enter or 'a' shows the image, t inspects tags.\n");
    exit(2);
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

    if (table_len(&tb) == 0) {
        fprintf(stderr, "tagview: no images found\n");
        return 1;
    }
    if (use_cache && parsed > 0) cache_save(cachepath, &tb);

    if (qexpr) {
        qnode *q = NULL;
        if (qexpr[0]) {
            q = query_parse(qexpr, 0 /* strict */);
            if (!q) {
                fprintf(stderr, "tagview: cannot parse query: %s\n", qexpr);
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
                char who[512];
                APP->identity(t, who, sizeof who);
                const char *cam = track_first_tag(t, "CAMERA");
                const char *da = track_first_tag(t, "DATE");
                const char *w = track_first_tag(t, "WIDTH");
                const char *h = track_first_tag(t, "HEIGHT");
                printf("%-32.32s\t%-24.24s\t%-20.20s\t%sx%s\t%s\n",
                       who, cam ? cam : "?", da ? da : "?",
                       w ? w : "?", h ? h : "?", t->path);
            } else {
                printf("%s\n", t->path);
            }
        }
        fprintf(stderr, "%zu images\n", idx.len);
        query_free(q);
        vec_free(&idx);
    } else {
        browser_run(&tb, NULL);
    }
    table_free(&tb);
    return 0;
}
