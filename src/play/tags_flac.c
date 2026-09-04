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

#include "tags.h"
#include <FLAC/metadata.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

int tags_read_flac(track *t) {
    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
    if (!chain) return -1;
    if (!FLAC__metadata_chain_read(chain, t->path)) {
        FLAC__metadata_chain_delete(chain);
        return -1;
    }
    FLAC__Metadata_Iterator *it = FLAC__metadata_iterator_new();
    FLAC__metadata_iterator_init(it, chain);
    do {
        FLAC__StreamMetadata *m = FLAC__metadata_iterator_get_block(it);
        if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
            t->sample_rate = m->data.stream_info.sample_rate;
            t->channels    = m->data.stream_info.channels;
            if (t->sample_rate)
                t->duration = (double)m->data.stream_info.total_samples / t->sample_rate;
        } else if (m->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            FLAC__StreamMetadata_VorbisComment *vc = &m->data.vorbis_comment;
            for (uint32_t i = 0; i < vc->num_comments; i++) {
                const char *e = (const char *)vc->comments[i].entry;
                const char *eq = strchr(e, '=');
                if (!eq) continue;
                char *key = xstrndup(e, (size_t)(eq - e));
                track_add_tag(t, key, eq + 1);
                free(key);
            }
        }
    } while (FLAC__metadata_iterator_next(it));
    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);
    t->fmt = FMT_FLAC;
    return 0;
}

void tags_fallback_from_path(track *t) {
    if (track_first_tag(t, "TITLE")) return;
    char *p1 = xstrdup(t->path), *p2 = xstrdup(t->path), *p3 = xstrdup(t->path);
    char *base = basename(p1);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    /* strip leading "NN " / "NN-" / "NN. " track numbers */
    char *title = base;
    if (strlen(title) > 3 &&
        title[0] >= '0' && title[0] <= '9' &&
        title[1] >= '0' && title[1] <= '9' &&
        (title[2] == ' ' || title[2] == '-' || title[2] == '.')) {
        char tno[3] = { title[0], title[1], 0 };
        track_add_tag(t, "TRACKNUMBER", tno);
        title += 3;
        while (*title == ' ' || *title == '-') title++;
    }
    track_add_tag(t, "TITLE", title);
    char *dir = dirname(p2);
    char *album = basename(dir);
    if (strcmp(album, ".") && strcmp(album, "/")) track_add_tag(t, "ALBUM", album);
    char *dir2 = dirname(p3);
    dir2 = dirname(dir2);
    char *artist = basename(dir2);
    if (strcmp(artist, ".") && strcmp(artist, "/")) track_add_tag(t, "ARTIST", artist);
    track_add_tag(t, "SOURCE", "path");
    free(p1); free(p2); free(p3);
}
