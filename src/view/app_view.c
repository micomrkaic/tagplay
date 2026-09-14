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

/* The image application's answers to the core's questions. */

#include "app.h"
#include "img_tags.h"
#include "browser.h"
#include "art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { VFMT_JPEG, VFMT_PNG };

static const char *view_fmt_name(int f) {
    switch (f) {
    case VFMT_JPEG: return "jpeg";
    case VFMT_PNG:  return "png";
    default:        return "?";
    }
}

static int view_want_path(const char *fname) {
    const char *dot = strrchr(fname, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") ||
           !strcasecmp(dot, ".png");
}

static int view_probe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t m[8] = { 0 };
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    if (n >= 2 && m[0] == 0xFF && m[1] == 0xD8) return VFMT_JPEG;
    static const uint8_t png[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (n >= 8 && !memcmp(m, png, 8)) return VFMT_PNG;
    return -1;
}

static int view_read_item(track *t, int fmt) {
    t->fmt = fmt;
    return fmt == VFMT_PNG ? img_read_png(t) : img_read_jpeg(t);
}

static int view_cacheable(const track *t) { (void)t; return 1; }

static double tagnum(const track *t, const char *k) {
    const char *v = track_first_tag(t, k);
    return v ? atof(v) : 0;
}
static double pf_width(const track *t)  { return tagnum(t, "WIDTH"); }
static double pf_height(const track *t) { return tagnum(t, "HEIGHT"); }
static double pf_mpix(const track *t) {
    return tagnum(t, "WIDTH") * tagnum(t, "HEIGHT") / 1e6;
}

static const pseudo_field view_fields[] = {
    { "width",  pf_width },
    { "height", pf_height },
    { "mpix",   pf_mpix },
};

static uint8_t *view_art_read(const track *t, size_t *len) {
    return img_file_read(t, len);
}

static void view_identity(const track *t, char *out, size_t sz) {
    const char *title = track_first_tag(t, "TITLE");
    if (title && *title) {
        snprintf(out, sz, "%s", title);
        return;
    }
    const char *base = strrchr(t->path, '/');
    snprintf(out, sz, "%s", base ? base + 1 : t->path);
}

static void view_detail_header(const track *t, char *out, size_t sz) {
    const char *w = track_first_tag(t, "WIDTH");
    const char *h = track_first_tag(t, "HEIGHT");
    const char *cam = track_first_tag(t, "CAMERA");
    if (w && h)
        snprintf(out, sz, "%s \xc2\xb7 %sx%s \xc2\xb7 %.1f MP%s%s",
                 view_fmt_name(t->fmt), w, h,
                 atof(w) * atof(h) / 1e6,
                 cam ? " \xc2\xb7 " : "", cam ? cam : "");
    else
        snprintf(out, sz, "%s", view_fmt_name(t->fmt));
}

/* Enter: show the image full-screen (cursored item in list mode,
 * first item otherwise), any key returns to the browser */
static void view_on_enter(void *ui, struct browser *b, const vec *items,
                          int from_sel) {
    (void)ui;
    if (!items->len) return;
    size_t pick = (!from_sel && b->focus == 1 && b->lcur < items->len)
                ? b->lcur : 0;
    const track *t = table_at(b->tb, *(size_t *)vec_at((vec *)items, pick));
    size_t len = 0;
    uint8_t *img = img_file_read(t, &len);
    if (!img) {
        snprintf(b->msg, sizeof b->msg, "can't read %s", t->path);
        return;
    }
    browser_raw_off();
    printf("\x1b[2J\x1b[H");
    char who[512];
    view_identity(t, who, sizeof who);
    int rows = term_rows(), cols = term_cols();
    art_render_ascii(img, len, cols, rows - 3);
    char hd[256];
    view_detail_header(t, hd, sizeof hd);
    printf("\n%s \xe2\x80\x94 %s\n[any key returns]", who, hd);
    fflush(stdout);
    free(img);
    browser_raw_on();
    getchar();
}

static const char *const VIEW_ID_KEYS[] = {
    "TITLE", "TAG", "CAPTION", "ARTIST", "DATE", "CAMERA",
    "CITY", "COUNTRY", "WIDTH", "HEIGHT", NULL
};

static const tp_app VIEW_APP = {
    .name          = "tagview",
    .cache_file    = "cache.bin",
    .cache_version = 100,             /* never collides with tagplay */
    .want_path     = view_want_path,
    .probe         = view_probe,
    .read_item     = view_read_item,
    .cacheable     = view_cacheable,
    .fmt_name      = view_fmt_name,
    .fields        = view_fields,
    .nfields       = sizeof view_fields / sizeof *view_fields,
    .art_read      = view_art_read,
    .id_keys       = VIEW_ID_KEYS,
    .identity      = view_identity,
    .detail_header = view_detail_header,
    .on_enter      = view_on_enter,
};

const tp_app *APP = &VIEW_APP;
