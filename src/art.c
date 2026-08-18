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

/* art.c — embedded album art, extracted on demand and rendered as
 * colored ASCII (luminance ramp + truecolor foreground). Sources:
 * FLAC PICTURE metadata blocks and ID3v2 APIC frames. Nothing is
 * cached; covers are read only when the user asks to see one. */
#include "art.h"
#include "util.h"
#include <FLAC/metadata.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ---- FLAC: PICTURE block, front cover (type 3) preferred ---- */
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

/* ---- colored ASCII rendering ---- */
static const char RAMP[] = " .:-=+*#%@";

int art_render_ascii(const uint8_t *img, size_t len, int max_cols, int max_rows) {
    int w, h, comp;
    unsigned char *px = stbi_load_from_memory(img, (int)len, &w, &h, &comp, 3);
    if (!px) return -1;
    /* terminal cells are ~2x taller than wide */
    int cols = max_cols, rows = (int)((double)h / w * cols * 0.5 + 0.5);
    if (rows > max_rows) {
        rows = max_rows;
        cols = (int)((double)w / h * rows * 2.0 + 0.5);
        if (cols > max_cols) cols = max_cols;
    }
    if (cols < 2 || rows < 1) { stbi_image_free(px); return -1; }
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            /* average the source region this cell covers */
            int x0 = c * w / cols, x1 = (c + 1) * w / cols;
            int y0 = r * h / rows, y1 = (r + 1) * h / rows;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            long R = 0, G = 0, B = 0, cnt = 0;
            for (int y = y0; y < y1 && y < h; y++)
                for (int x = x0; x < x1 && x < w; x++) {
                    unsigned char *s = px + 3 * (y * w + x);
                    R += s[0]; G += s[1]; B += s[2]; cnt++;
                }
            if (!cnt) cnt = 1;
            int rr = (int)(R / cnt), gg = (int)(G / cnt), bb = (int)(B / cnt);
            double lum = (0.2126 * rr + 0.7152 * gg + 0.0722 * bb) / 255.0;
            int ci = (int)(lum * (sizeof RAMP - 2) + 0.5);
            printf("\x1b[38;2;%d;%d;%dm%c", rr, gg, bb, RAMP[ci]);
        }
        printf("\x1b[0m\n");
    }
    stbi_image_free(px);
    return 0;
}
