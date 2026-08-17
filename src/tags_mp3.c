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

/* ---------- minimal UTF-16 -> UTF-8 ---------- */
static void utf16_to_utf8(const uint8_t *p, size_t n, int be, vec *out) {
    size_t i = 0;
    while (i + 1 < n) {
        uint32_t c = be ? (uint32_t)(p[i] << 8 | p[i + 1])
                        : (uint32_t)(p[i + 1] << 8 | p[i]);
        i += 2;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            uint32_t lo = be ? (uint32_t)(p[i] << 8 | p[i + 1])
                             : (uint32_t)(p[i + 1] << 8 | p[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        char b[4]; int k = 0;
        if (c < 0x80) b[k++] = (char)c;
        else if (c < 0x800) {
            b[k++] = (char)(0xC0 | c >> 6);
            b[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            b[k++] = (char)(0xE0 | c >> 12);
            b[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            b[k++] = (char)(0xF0 | c >> 18);
            b[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            b[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[k++] = (char)(0x80 | (c & 0x3F));
        }
        for (int j = 0; j < k; j++) vec_push(out, &b[j]);
    }
}

/* decode an ID3 text payload (first byte = encoding) into UTF-8.
 * Embedded NULs (multi-value separators) are preserved; *outlen is the
 * decoded length excluding the final added NUL. Caller frees. */
static char *id3_text(const uint8_t *p, size_t n, size_t *outlen) {
    if (n < 1) { *outlen = 0; return xstrdup(""); }
    uint8_t enc = p[0];
    p++; n--;
    vec out; vec_init(&out, 1);
    if (enc == 0) { /* "latin-1", which in the wild means CP1252 */
        char *u = cp1252_to_utf8(p, n);
        /* cp1252_to_utf8 preserves embedded NULs (multi-value seps) but
         * returns a C buffer; re-walk the input to keep them aligned */
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            if (p[i] == 0) { char z = 0; vec_push(&out, &z); w += 1; continue; }
            /* decode this single byte alone to keep NUL alignment */
            char *one = cp1252_to_utf8(p + i, 1);
            for (char *q = one; *q; q++) vec_push(&out, q);
            free(one);
        }
        free(u);
        (void)w;
    } else if (enc == 1) { /* utf-16 with BOM */
        int be = 1;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = 0; p += 2; n -= 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = 1; p += 2; n -= 2; }
        utf16_to_utf8(p, n, be, &out);
    } else if (enc == 2) { /* utf-16be, no BOM */
        utf16_to_utf8(p, n, 1, &out);
    } else { /* 3 = utf-8 */
        for (size_t i = 0; i < n; i++) { char ch = (char)p[i]; vec_push(&out, &ch); }
    }
    *outlen = out.len;
    char z = 0; vec_push(&out, &z);
    char *s = xmalloc(out.len);
    memcpy(s, out.data, out.len);
    vec_free(&out);
    return s;
}

/* split decoded text of length n on NULs (id3v2.4 multi-value) */
static void add_multi(track *t, const char *key, const char *joined, size_t n) {
    const char *s = joined, *end = joined + n;
    while (s < end) {
        if (*s) track_add_tag(t, key, s);
        s += strlen(s) + 1;
    }
}

static uint32_t syncsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static const struct { const char *frame, *key; } frame_map[] = {
    { "TIT2", "TITLE" },  { "TPE1", "ARTIST" }, { "TPE2", "ALBUMARTIST" },
    { "TALB", "ALBUM" },  { "TDRC", "DATE" },   { "TYER", "DATE" },
    { "TRCK", "TRACKNUMBER" }, { "TPOS", "DISCNUMBER" },
    { "TCON", "GENRE" },  { "TCOM", "COMPOSER" }, { NULL, NULL }
};

/* remove unsynchronization (0xFF 0x00 -> 0xFF) in place, return new length */
static size_t deunsync(uint8_t *p, size_t n) {
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        p[w++] = p[r];
        if (p[r] == 0xFF && r + 1 < n && p[r + 1] == 0x00) r++;
    }
    return w;
}

static void parse_id3v2(track *t, uint8_t *buf, size_t len, size_t *audio_off) {
    *audio_off = 0;
    if (len < 10 || memcmp(buf, "ID3", 3)) return;
    uint8_t ver = buf[3], flags = buf[5];
    uint32_t tagsz = syncsafe(buf + 6);
    if (10 + (size_t)tagsz > len) tagsz = (uint32_t)(len - 10);
    uint8_t *p = buf + 10;
    size_t n = tagsz;
    *audio_off = 10 + tagsz + ((flags & 0x10) ? 10 : 0); /* footer */

    if (ver == 3 && (flags & 0x80)) n = deunsync(p, n); /* v2.3: whole tag */
    if (flags & 0x40) { /* extended header: skip */
        if (n >= 4) {
            uint32_t esz = (ver == 4) ? syncsafe(p) : be32(p);
            if (ver == 3) esz += 4;
            if (esz < n) { p += esz; n -= esz; } else n = 0;
        }
    }
    while (n >= 10 && p[0]) {
        char id[5] = { (char)p[0], (char)p[1], (char)p[2], (char)p[3], 0 };
        uint32_t fsz = (ver == 4) ? syncsafe(p + 4) : be32(p + 4);
        uint16_t fflags = (uint16_t)(p[8] << 8 | p[9]);
        p += 10; n -= 10;
        if (fsz > n) break;
        uint8_t *body = p;
        size_t bodylen = fsz;
        p += fsz; n -= fsz;

        if (ver == 4 && (fflags & 0x000F)) continue; /* compressed/encrypted/has-extras: skip */
        if (ver == 3 && (fflags & 0x00C0)) continue;
        uint8_t *tmp = NULL;
        if (ver == 4 && (fflags & 0x0002)) { /* per-frame unsync */
            tmp = xmalloc(bodylen);
            memcpy(tmp, body, bodylen);
            bodylen = deunsync(tmp, bodylen);
            body = tmp;
        }
        if (!strcmp(id, "TXXX")) {
            size_t tn;
            char *txt = id3_text(body, bodylen, &tn);
            /* decoded buffer = DESC NUL VALUE... */
            size_t dlen = strlen(txt);
            if (dlen + 1 < tn && txt[0])
                track_add_tag(t, txt, txt + dlen + 1);
            free(txt);
        } else if (id[0] == 'T') {
            for (int i = 0; frame_map[i].frame; i++) {
                if (!strcmp(id, frame_map[i].frame)) {
                    size_t tn;
                    char *txt = id3_text(body, bodylen, &tn);
                    add_multi(t, frame_map[i].key, txt, tn);
                    free(txt);
                    break;
                }
            }
        }
        free(tmp);
    }
}

/* ---------- MPEG audio duration ---------- */
static const int br_v1l1[] = { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0 };
static const int br_v1l2[] = { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0 };
static const int br_v1l3[] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
static const int br_v2l1[] = { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0 };
static const int br_v2l3[] = { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0 };
static const int sr_v1[] = { 44100, 48000, 32000, 0 };

static void mp3_duration(track *t, const uint8_t *buf, size_t len, size_t off) {
    /* find first frame sync */
    size_t i = off;
    while (i + 4 < len && !(buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0)) i++;
    if (i + 4 >= len) return;
    const uint8_t *h = buf + i;
    int ver   = (h[1] >> 3) & 3;  /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
    int layer = (h[1] >> 1) & 3;  /* 3=I, 2=II, 1=III */
    int brx   = (h[2] >> 4) & 0xF;
    int srx   = (h[2] >> 2) & 3;
    if (layer == 0 || srx == 3) return;
    int sr = sr_v1[srx];
    if (ver == 2) sr /= 2;
    else if (ver == 0) sr /= 4;
    const int *tbl;
    int spf;
    if (layer == 3) {        /* Layer I */
        tbl = (ver == 3) ? br_v1l1 : br_v2l1;
        spf = 384;
    } else if (layer == 2) { /* Layer II (MPEG-2 shares the LII/LIII table) */
        tbl = (ver == 3) ? br_v1l2 : br_v2l3;
        spf = 1152;
    } else {                 /* Layer III */
        tbl = (ver == 3) ? br_v1l3 : br_v2l3;
        spf = (ver == 3) ? 1152 : 576;
    }
    int br = tbl[brx] * 1000;
    t->sample_rate = (uint32_t)sr;
    t->channels    = (((h[3] >> 6) & 3) == 3) ? 1 : 2;

    /* Xing/Info VBR header? sits after side info */
    int sideinfo = (ver == 3) ? ((t->channels == 1) ? 17 : 32)
                              : ((t->channels == 1) ? 9  : 17);
    size_t xo = i + 4 + (size_t)sideinfo;
    if (xo + 16 < len &&
        (!memcmp(buf + xo, "Xing", 4) || !memcmp(buf + xo, "Info", 4))) {
        uint32_t xflags = be32(buf + xo + 4);
        if (xflags & 1) {
            uint32_t nframes = be32(buf + xo + 8);
            t->duration = (double)nframes * spf / sr;
            return;
        }
    }
    if (br > 0) t->duration = (double)(len - i) * 8.0 / br; /* CBR estimate */
}

#include "decoder.h"
int tags_read_mp3(track *t) {
    size_t len;
    uint8_t *buf = read_file(t->path, &len);
    if (!buf) return -1;
    size_t audio_off = 0;
    parse_id3v2(t, buf, len, &audio_off);
    mp3_duration(t, buf, len, audio_off);
    free(buf);
    if (t->duration <= 0 || !t->sample_rate) {
        /* header math failed (no Xing, odd framing, junk between tag and
         * audio): let minimp3 measure it properly. Costs a full parse,
         * but only for the pathological files. */
        decoder *d = decoder_open(t->path, FMT_MP3);
        if (d) {
            t->sample_rate = (uint32_t)decoder_rate(d);
            t->channels    = (uint32_t)decoder_channels(d);
            t->duration    = decoder_duration(d);
            decoder_close(d);
        }
    }
    t->fmt = FMT_MP3;
    tags_fallback_from_path(t);
    return 0;
}
