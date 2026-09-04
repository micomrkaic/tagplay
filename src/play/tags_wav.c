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
#include <string.h>
#include <stdlib.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* RIFF INFO id -> canonical tag key */
static const struct { const char *id, *key; } info_map[] = {
    { "IART", "ARTIST" }, { "INAM", "TITLE" }, { "IPRD", "ALBUM" },
    { "ICRD", "DATE" },   { "IGNR", "GENRE" }, { "ITRK", "TRACKNUMBER" },
    { "ICMT", "COMMENT" },{ "ICOP", "COPYRIGHT" }, { NULL, NULL }
};

int tags_read_wav(track *t) {
    size_t len;
    uint8_t *buf = read_file(t->path, &len);
    if (!buf) return -1;
    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        free(buf);
        return -1;
    }
    uint32_t byte_rate = 0;
    uint64_t data_len = 0;
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *ck = buf + pos;
        uint32_t cksz = rd32(ck + 4);
        const uint8_t *body = ck + 8;
        size_t avail = len - pos - 8;
        if (cksz > avail) cksz = (uint32_t)avail;

        if (!memcmp(ck, "fmt ", 4) && cksz >= 16) {
            t->channels    = rd16(body + 2);
            t->sample_rate = rd32(body + 4);
            byte_rate      = rd32(body + 8);
        } else if (!memcmp(ck, "data", 4)) {
            data_len = cksz;
        } else if (!memcmp(ck, "LIST", 4) && cksz >= 4 && !memcmp(body, "INFO", 4)) {
            size_t ipos = 4;
            while (ipos + 8 <= cksz) {
                const uint8_t *sub = body + ipos;
                uint32_t ssz = rd32(sub + 4);
                if (ssz > cksz - ipos - 8) break;
                for (int i = 0; info_map[i].id; i++) {
                    if (!memcmp(sub, info_map[i].id, 4)) {
                        /* values are NUL-padded; xstrndup terminates and
                         * strlen naturally stops at first NUL */
                        char *v = xstrndup((const char *)sub + 8, ssz);
                        track_add_tag(t, info_map[i].key, v);
                        free(v);
                        break;
                    }
                }
                ipos += 8 + ssz + (ssz & 1);
            }
        }
        pos += 8 + cksz + (cksz & 1);
    }
    free(buf);
    if (byte_rate && data_len) t->duration = (double)data_len / byte_rate;
    t->fmt = FMT_WAV;
    tags_fallback_from_path(t);
    return 0;
}
