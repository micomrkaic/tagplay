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

/* Embedded photo metadata -> the core tag model.
 *
 * A JPEG can carry THREE metadata systems at once: EXIF (a TIFF
 * structure in APP1: camera, date, dimensions), legacy IPTC-IIM
 * (binary datasets inside the Photoshop APP13 segment: keywords,
 * title, byline), and XMP (an XML packet in APP1: dc:subject et al).
 * The read side merges all of them into multi-valued tags; keywords
 * union with case-insensitive dedup. PNG carries tEXt/iTXt chunks.
 *
 * Everything here parses UNTRUSTED bytes: every offset and length is
 * bounds-checked before use, counts are capped, and a malformed file
 * degrades to "fewer tags", never to a crash. */

#include "img_tags.h"
#include "track.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAX_META (8u * 1024 * 1024)   /* metadata read ceiling */
#define MAX_TAGS 256                  /* per-file keyword cap */

/* ---- small helpers ---- */

static unsigned rd16be(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rd32be(const uint8_t *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
           (unsigned)p[2] << 8 | p[3];
}

/* add a tag value unless an equal (case-insensitive) value exists */
static void add_unique(track *t, const char *key, const char *val) {
    if (!val || !*val) return;
    size_t have = 0;
    for (size_t i = 0; i < t->tags.len; i++) {
        tagkv *kv = vec_at(&t->tags, i);
        if (str_ieq(kv->key, key)) {
            have++;
            if (!strcasecmp(kv->value, val)) return;
        }
    }
    if (have >= MAX_TAGS) return;
    track_add_tag(t, key, val);
}

/* add a bounded, trimmed string tag */
static void add_str(track *t, const char *key, const uint8_t *p, size_t n) {
    if (!p) return;
    while (n && (p[0] == ' ' || p[0] == 0)) { p++; n--; }
    while (n && (p[n-1] == ' ' || p[n-1] == 0 ||
                 p[n-1] == '\r' || p[n-1] == '\n')) n--;
    if (!n || n > 1024) return;
    char buf[1025];
    memcpy(buf, p, n);
    buf[n] = 0;
    /* control chars have no business in tags */
    for (char *c = buf; *c; c++)
        if ((unsigned char)*c < 0x20 && *c != 0) *c = ' ';
    add_unique(t, key, buf);
}

/* ---- EXIF: a TIFF structure ---- */

typedef struct {
    const uint8_t *base;
    size_t len;
    int be;              /* big-endian byte order */
} tiffctx;

static unsigned t16(const tiffctx *c, size_t off) {
    if (off + 2 > c->len) return 0;
    const uint8_t *p = c->base + off;
    return c->be ? rd16be(p) : (unsigned)p[1] << 8 | p[0];
}
static unsigned t32(const tiffctx *c, size_t off) {
    if (off + 4 > c->len) return 0;
    const uint8_t *p = c->base + off;
    return c->be ? rd32be(p)
                 : (unsigned)p[3] << 24 | (unsigned)p[2] << 16 |
                   (unsigned)p[1] << 8 | p[0];
}

/* value location of an IFD entry (inline if it fits in 4 bytes) */
static size_t tval_off(const tiffctx *c, size_t entry, unsigned type,
                       unsigned count) {
    static const unsigned sz[] = { 0,1,1,2,4,8,1,1,2,4,8,4,8 };
    unsigned unit = type < 13 ? sz[type] : 0;
    if (!unit) return (size_t)-1;
    size_t total = (size_t)unit * count;
    if (total <= 4) return entry + 8;
    size_t off = t32(c, entry + 8);
    if (off + total > c->len || off < 8) return (size_t)-1;
    return off;
}

static unsigned tiff_uint(const tiffctx *c, size_t entry, unsigned type,
                          unsigned count) {
    if (count != 1) return 0;
    size_t off = tval_off(c, entry, type, count);
    if (off == (size_t)-1) return 0;
    if (type == 3) return t16(c, off);
    if (type == 4) return t32(c, off);
    return 0;
}

typedef struct { char make[128], model[128], date[64], odate[64];
                 unsigned w, h; } exifout;

static void walk_ifd(const tiffctx *c, size_t ifd, exifout *out,
                     int depth);

static void exif_entry(const tiffctx *c, size_t e, exifout *out,
                       int depth) {
    unsigned tag = t16(c, e), type = t16(c, e + 2), cnt = t32(c, e + 4);
    if (cnt > 65536) return;
    size_t off;
    switch (tag) {
    case 0x010F: /* Make */
        if (type == 2 && cnt && cnt < sizeof out->make &&
            (off = tval_off(c, e, type, cnt)) != (size_t)-1 &&
            off + cnt <= c->len) {
            memcpy(out->make, c->base + off, cnt);
            out->make[cnt] = 0;
        }
        break;
    case 0x0110: /* Model */
        if (type == 2 && cnt && cnt < sizeof out->model &&
            (off = tval_off(c, e, type, cnt)) != (size_t)-1 &&
            off + cnt <= c->len) {
            memcpy(out->model, c->base + off, cnt);
            out->model[cnt] = 0;
        }
        break;
    case 0x0132: /* DateTime */
        if (type == 2 && cnt && cnt < sizeof out->date &&
            (off = tval_off(c, e, type, cnt)) != (size_t)-1 &&
            off + cnt <= c->len) {
            memcpy(out->date, c->base + off, cnt);
            out->date[cnt] = 0;
        }
        break;
    case 0x9003: /* DateTimeOriginal */
        if (type == 2 && cnt && cnt < sizeof out->odate &&
            (off = tval_off(c, e, type, cnt)) != (size_t)-1 &&
            off + cnt <= c->len) {
            memcpy(out->odate, c->base + off, cnt);
            out->odate[cnt] = 0;
        }
        break;
    case 0xA002: out->w = tiff_uint(c, e, type, cnt); break;
    case 0xA003: out->h = tiff_uint(c, e, type, cnt); break;
    case 0x8769: /* EXIF sub-IFD pointer */
        if (depth < 3) walk_ifd(c, t32(c, e + 8), out, depth + 1);
        break;
    default: break;
    }
}

static void walk_ifd(const tiffctx *c, size_t ifd, exifout *out,
                     int depth) {
    if (ifd < 8 || ifd + 2 > c->len) return;
    unsigned n = t16(c, ifd);
    if (n > 512) return;
    for (unsigned i = 0; i < n; i++) {
        size_t e = ifd + 2 + (size_t)i * 12;
        if (e + 12 > c->len) return;
        exif_entry(c, e, out, depth);
    }
}

static void exif_parse(track *t, const uint8_t *p, size_t n) {
    if (n < 8) return;
    tiffctx c = { p, n, 0 };
    if (!memcmp(p, "MM", 2)) c.be = 1;
    else if (memcmp(p, "II", 2)) return;
    if (t16(&c, 2) != 42) return;
    exifout out = { {0}, {0}, {0}, {0}, 0, 0 };
    walk_ifd(&c, t32(&c, 4), &out, 0);
    if (out.make[0] || out.model[0]) {
        char cam[280];
        /* many Models repeat the Make; don't say "Canon Canon EOS" */
        if (out.make[0] && out.model[0] &&
            strncasecmp(out.model, out.make, strlen(out.make)) &&
            strstr(out.model, out.make) == NULL)
            snprintf(cam, sizeof cam, "%s %s", out.make, out.model);
        else
            snprintf(cam, sizeof cam, "%s",
                     out.model[0] ? out.model : out.make);
        add_str(t, "CAMERA", (const uint8_t *)cam, strlen(cam));
    }
    const char *d = out.odate[0] ? out.odate : out.date;
    if (d[0]) add_str(t, "DATE", (const uint8_t *)d, strlen(d));
    if (out.w && out.w < 100000) {
        char b[16];
        snprintf(b, sizeof b, "%u", out.w);
        add_unique(t, "EXIFW", b);
    }
    if (out.h && out.h < 100000) {
        char b[16];
        snprintf(b, sizeof b, "%u", out.h);
        add_unique(t, "EXIFH", b);
    }
}

/* ---- IPTC-IIM inside the Photoshop APP13 segment ---- */

static void iptc_parse(track *t, const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i + 5 <= n) {
        if (p[i] != 0x1C) { i++; continue; }
        unsigned rec = p[i + 1], set = p[i + 2];
        unsigned len = rd16be(p + i + 3);
        if (len & 0x8000) return;      /* extended datasets: not here */
        if (i + 5 + len > n) return;
        const uint8_t *v = p + i + 5;
        if (rec == 2) {
            switch (set) {
            case 25:  add_str(t, "TAG", v, len); break;      /* Keywords */
            case 5:   add_str(t, "TITLE", v, len); break;    /* ObjectName */
            case 80:  add_str(t, "ARTIST", v, len); break;   /* By-line */
            case 120: add_str(t, "CAPTION", v, len); break;
            case 90:  add_str(t, "CITY", v, len); break;
            case 101: add_str(t, "COUNTRY", v, len); break;
            default: break;
            }
        }
        i += 5 + len;
    }
}

static void app13_parse(track *t, const uint8_t *p, size_t n) {
    if (n < 14 || memcmp(p, "Photoshop 3.0\0", 14)) return;
    size_t i = 14;
    while (i + 12 <= n) {
        if (memcmp(p + i, "8BIM", 4)) return;
        unsigned id = rd16be(p + i + 4);
        size_t j = i + 6;
        unsigned nl = p[j];                       /* pascal name, padded even */
        j += 1 + nl;
        if (j & 1) j++;
        if (j + 4 > n) return;
        unsigned sz = rd32be(p + j);
        j += 4;
        if (j + sz > n) return;
        if (id == 0x0404) iptc_parse(t, p + j, sz);
        i = j + sz + (sz & 1);
    }
}

/* ---- XMP: bounded string extraction, no XML parser ---- */

static void xml_unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; }
        else if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; }
        else if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; }
        else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; }
        else if (!strncmp(r, "&#39;", 5)) { *w++ = '\''; r += 5; }
        else *w++ = *r++;
    }
    *w = 0;
}

/* collect <rdf:li ...>text</rdf:li> items within the element following
 * the given property name; each becomes a tag under key */
static void xmp_items(track *t, const char *xml, size_t n,
                      const char *prop, const char *key, int first_only) {
    const char *p = xml;
    const char *end = xml + n;
    const char *hit = NULL;
    for (const char *s = p; s + strlen(prop) < end; s++)
        if (!strncmp(s, prop, strlen(prop))) { hit = s; break; }
    if (!hit) return;
    const char *scope_end = hit + 4096 < end ? hit + 4096 : end;
    const char *s = hit;
    int got = 0;
    while (s < scope_end && got < 64) {
        const char *li = NULL;
        for (const char *q = s; q + 7 < scope_end; q++)
            if (!strncmp(q, "<rdf:li", 7)) { li = q; break; }
        if (!li) break;
        const char *gt = memchr(li, '>', (size_t)(scope_end - li));
        if (!gt) break;
        const char *close = NULL;
        for (const char *q = gt; q + 9 < scope_end; q++)
            if (!strncmp(q, "</rdf:li", 8)) { close = q; break; }
        if (!close || close <= gt + 1) { s = gt + 1; continue; }
        size_t vl = (size_t)(close - gt - 1);
        if (vl && vl < 512) {
            char buf[513];
            memcpy(buf, gt + 1, vl);
            buf[vl] = 0;
            xml_unescape(buf);
            add_str(t, key, (const uint8_t *)buf, strlen(buf));
            got++;
            if (first_only) return;
        }
        s = close + 8;
    }
}

static void xmp_parse(track *t, const uint8_t *p, size_t n) {
    const char *x = (const char *)p;
    xmp_items(t, x, n, "dc:subject", "TAG", 0);
    xmp_items(t, x, n, "dc:title", "TITLE", 1);
    xmp_items(t, x, n, "dc:creator", "ARTIST", 1);
    xmp_items(t, x, n, "dc:description", "CAPTION", 1);
}

/* ---- JPEG segment walk ---- */

int img_read_jpeg(track *t) {
    FILE *f = fopen(t->path, "rb");
    if (!f) return -1;
    uint8_t hdr[4];
    if (fread(hdr, 1, 2, f) != 2 || hdr[0] != 0xFF || hdr[1] != 0xD8) {
        fclose(f);
        return -1;
    }
    unsigned w = 0, h = 0;
    long fed = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (c != 0xFF) continue;
        int m;
        do { m = fgetc(f); } while (m == 0xFF);
        if (m == EOF || m == 0xD9 || m == 0xDA) break;   /* EOI / SOS */
        if (m >= 0xD0 && m <= 0xD7) continue;             /* RSTn: no len */
        uint8_t lb[2];
        if (fread(lb, 1, 2, f) != 2) break;
        unsigned seglen = rd16be(lb);
        if (seglen < 2) break;
        size_t body = seglen - 2;
        if ((m == 0xC0 || m == 0xC1 || m == 0xC2) && body >= 5) {
            uint8_t sof[5];
            if (fread(sof, 1, 5, f) != 5) break;
            h = rd16be(sof + 1);
            w = rd16be(sof + 3);
            if (fseek(f, (long)(body - 5), SEEK_CUR)) break;
            continue;
        }
        if ((m == 0xE1 || m == 0xED) && body > 4 &&
            body <= MAX_META && fed < (long)MAX_META) {
            uint8_t *buf = xmalloc(body);
            if (fread(buf, 1, body, f) != body) { free(buf); break; }
            fed += (long)body;
            if (m == 0xE1 && body > 6 && !memcmp(buf, "Exif\0\0", 6))
                exif_parse(t, buf + 6, body - 6);
            else if (m == 0xE1 && body > 29 &&
                     !memcmp(buf, "http://ns.adobe.com/xap/1.0/", 28))
                xmp_parse(t, buf + 29, body - 29);
            else if (m == 0xED)
                app13_parse(t, buf, body);
            free(buf);
            continue;
        }
        if (fseek(f, (long)body, SEEK_CUR)) break;
    }
    fclose(f);
    if (w && h) {
        char b[16];
        snprintf(b, sizeof b, "%u", w);
        add_unique(t, "WIDTH", b);
        snprintf(b, sizeof b, "%u", h);
        add_unique(t, "HEIGHT", b);
    } else {
        /* fall back to EXIF pixel dims if SOF never appeared */
        const char *ew = track_first_tag(t, "EXIFW");
        const char *eh = track_first_tag(t, "EXIFH");
        if (ew) add_unique(t, "WIDTH", ew);
        if (eh) add_unique(t, "HEIGHT", eh);
    }
    return 0;
}

/* ---- PNG chunks ---- */

int img_read_png(track *t) {
    FILE *f = fopen(t->path, "rb");
    if (!f) return -1;
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    uint8_t h8[8];
    if (fread(h8, 1, 8, f) != 8 || memcmp(h8, sig, 8)) {
        fclose(f);
        return -1;
    }
    long fed = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        unsigned len = rd32be(ch);
        char type[5] = { (char)ch[4], (char)ch[5], (char)ch[6],
                         (char)ch[7], 0 };
        if (len > MAX_META) break;
        if (!strcmp(type, "IHDR") && len >= 8) {
            uint8_t d[8];
            if (fread(d, 1, 8, f) != 8) break;
            char b[16];
            snprintf(b, sizeof b, "%u", rd32be(d));
            add_unique(t, "WIDTH", b);
            snprintf(b, sizeof b, "%u", rd32be(d + 4));
            add_unique(t, "HEIGHT", b);
            if (fseek(f, (long)(len - 8) + 4, SEEK_CUR)) break;
            continue;
        }
        if ((!strcmp(type, "tEXt") || !strcmp(type, "iTXt")) &&
            len > 1 && fed < (long)MAX_META) {
            uint8_t *buf = xmalloc(len);
            if (fread(buf, 1, len, f) != len) { free(buf); break; }
            fed += (long)len;
            const uint8_t *kw = buf;
            size_t kl = strnlen((const char *)buf, len);
            if (kl && kl + 1 < len) {
                const uint8_t *val = buf + kl + 1;
                size_t vl = len - kl - 1;
                if (!strcmp(type, "iTXt") && vl > 4) {
                    /* comp flag, comp method, then two NUL-terminated
                     * strings (language, translated keyword) */
                    if (val[0] != 0) { free(buf); goto skipcrc; }
                    const uint8_t *q = val + 2;
                    size_t left = vl - 2;
                    size_t l1 = strnlen((const char *)q, left);
                    if (l1 + 1 > left) { free(buf); goto skipcrc; }
                    q += l1 + 1; left -= l1 + 1;
                    size_t l2 = strnlen((const char *)q, left);
                    if (l2 + 1 > left) { free(buf); goto skipcrc; }
                    q += l2 + 1; left -= l2 + 1;
                    val = q; vl = left;
                }
                const char *key =
                    !strcasecmp((const char *)kw, "Title") ? "TITLE" :
                    !strcasecmp((const char *)kw, "Author") ? "ARTIST" :
                    !strcasecmp((const char *)kw, "Description") ? "CAPTION" :
                    !strcasecmp((const char *)kw, "Keywords") ? NULL : "";
                if (key && *key) add_str(t, key, val, vl);
                else if (!key) {
                    /* keyword list: split on comma/semicolon */
                    char tmp[1024];
                    size_t cl = vl < sizeof tmp - 1 ? vl : sizeof tmp - 1;
                    memcpy(tmp, val, cl);
                    tmp[cl] = 0;
                    for (char *tok = strtok(tmp, ",;"); tok;
                         tok = strtok(NULL, ",;")) {
                        while (*tok == ' ') tok++;
                        add_str(t, "TAG", (const uint8_t *)tok,
                                strlen(tok));
                    }
                }
            }
            free(buf);
            if (fseek(f, 4, SEEK_CUR)) break;   /* CRC */
            continue;
        }
        if (fseek(f, (long)len + 4, SEEK_CUR)) break;
        continue;
skipcrc:
        if (fseek(f, 4, SEEK_CUR)) break;
    }
    fclose(f);
    return 0;
}

/* whole-file bytes for display (chafa/stb decode from memory) */
uint8_t *img_file_read(const track *t, size_t *len) {
    FILE *f = fopen(t->path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256L * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *buf = xmalloc((size_t)sz);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *len = (size_t)sz;
    return buf;
}
