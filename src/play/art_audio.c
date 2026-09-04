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

/* Embedded album-art extraction from audio containers (FLAC PICTURE
 * blocks, ID3v2 APIC frames). Moved out of core: knowing what a FLAC
 * file is, is the audio app's business. */

#include "art_audio.h"
#include "tags.h"
#include "util.h"
#include <FLAC/metadata.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *flac_pic(const char *path, size_t *len) {
    FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
    if (!chain) return NULL;
    uint8_t *best = NULL;
    int best_type = -1;
    if (FLAC__metadata_chain_read(chain, path)) {
        FLAC__Metadata_Iterator *it = FLAC__metadata_iterator_new();
        FLAC__metadata_iterator_init(it, chain);
        do {
            FLAC__StreamMetadata *m = FLAC__metadata_iterator_get_block(it);
            if (m->type != FLAC__METADATA_TYPE_PICTURE) continue;
            int ty = (int)m->data.picture.type;
            int score = (ty == 3) ? 2 : (best == NULL ? 1 : 0);
            if (score > 0 && (best == NULL || (ty == 3 && best_type != 3))) {
                free(best);
                *len = m->data.picture.data_length;
                best = xmalloc(*len);
                memcpy(best, m->data.picture.data, *len);
                best_type = ty;
            }
        } while (FLAC__metadata_iterator_next(it));
        FLAC__metadata_iterator_delete(it);
    }
    FLAC__metadata_chain_delete(chain);
    return best;
}

/* ---- ID3v2 APIC: minimal independent scan ---- */
static uint32_t syncsafe4(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}
static uint32_t be4(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t *id3_apic(const char *path, size_t *outlen) {
    size_t len;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return NULL;
    uint8_t *best = NULL;
    int best_type = -1;
    if (len < 10 || memcmp(buf, "ID3", 3)) { free(buf); return NULL; }
    uint8_t ver = buf[3];
    uint32_t tagsz = syncsafe4(buf + 6);
    if (10 + (size_t)tagsz > len) tagsz = (uint32_t)(len - 10);
    uint8_t *p = buf + 10;
    size_t n = tagsz;
    while (n >= 10 && p[0]) {
        uint32_t fsz = (ver == 4) ? syncsafe4(p + 4) : be4(p + 4);
        int is_apic = !memcmp(p, "APIC", 4);
        p += 10; n -= 10;
        if (fsz > n) break;
        if (is_apic && fsz > 8) {
            const uint8_t *q = p, *end = p + fsz;
            uint8_t enc = *q++;
            while (q < end && *q) q++;          /* mime */
            if (q < end) q++;
            if (q >= end) goto next;
            int ptype = *q++;
            if (enc == 1 || enc == 2) {          /* UTF-16 desc: 00 00 term */
                while (q + 1 < end && (q[0] || q[1])) q += 2;
                q += 2;
            } else {
                while (q < end && *q) q++;
                q++;
            }
            if (q < end) {
                int score_new = (ptype == 3) ? 2 : 1;
                int score_old = (best_type == 3) ? 2 : (best ? 1 : 0);
                if (score_new > score_old) {
                    free(best);
                    *outlen = (size_t)(end - q);
                    best = xmalloc(*outlen);
                    memcpy(best, q, *outlen);
                    best_type = ptype;
                }
            }
        }
next:
        p += fsz;
        n -= fsz;
    }
    free(buf);
    return best;
}

uint8_t *art_extract(const char *path, audio_fmt fmt, size_t *len) {
    if (fmt == FMT_FLAC) return flac_pic(path, len);
    if (fmt == FMT_MP3)  return id3_apic(path, len);
    return NULL;
}


uint8_t *audio_art_read(const track *t, size_t *len) {
    if (t->fmt == FMT_FLAC) return flac_pic(t->path, len);
    if (t->fmt == FMT_MP3)  return id3_apic(t->path, len);
    return NULL;
}
