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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ---- FLAC: PICTURE block, front cover (type 3) preferred ---- */
/* ---- rendering ---- */
/* If the chafa binary is installed, use it (best-in-class terminal
 * graphics: optimal symbol selection, dithering, and native pixel
 * protocols on capable terminals). libchafa is deliberately NOT linked
 * -- it would pull in GLib. Fallback: built-in truecolor half-blocks
 * (U+2580 with fg+bg = two pixels per cell). */
#include <unistd.h>

static int chafa_available(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *path = getenv("PATH");
        cached = 0;
        if (path) {
            char buf[4096];
            snprintf(buf, sizeof buf, "%s", path);
            for (char *dir = strtok(buf, ":"); dir; dir = strtok(NULL, ":")) {
                char cand[4300];
                snprintf(cand, sizeof cand, "%s/chafa", dir);
                if (access(cand, X_OK) == 0) { cached = 1; break; }
            }
        }
    }
    return cached;
}

static int render_chafa(const uint8_t *img, size_t len, int cols, int rows) {
    char tmpl[] = "/tmp/tagplay-art-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    ssize_t wr = write(fd, img, len);
    close(fd);
    if (wr != (ssize_t)len) { unlink(tmpl); return -1; }
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "chafa -s %dx%d --animate off '%s' 2>>/dev/null", cols, rows, tmpl);
    fflush(stdout);
    int rc = system(cmd);
    unlink(tmpl);
    return rc == 0 ? 0 : -1;
}

static int render_halfblocks(const uint8_t *img, size_t len,
                             int max_cols, int max_rows) {
    int w, h, comp;
    unsigned char *px = stbi_load_from_memory(img, (int)len, &w, &h, &comp, 3);
    if (!px) return -1;
    /* half-blocks: each cell is 1 px wide, 2 px tall */
    int cols = max_cols, prows = (int)((double)h / w * cols + 0.5);
    if (prows > max_rows * 2) {
        prows = max_rows * 2;
        cols = (int)((double)w / h * prows + 0.5);
        if (cols > max_cols) cols = max_cols;
    }
    if (prows % 2) prows++;
    if (cols < 2 || prows < 2) { stbi_image_free(px); return -1; }
    for (int r = 0; r < prows; r += 2) {
        for (int c = 0; c < cols; c++) {
            int rgb[2][3];
            for (int half = 0; half < 2; half++) {
                int x0 = c * w / cols, x1 = (c + 1) * w / cols;
                int y0 = (r + half) * h / prows, y1 = (r + half + 1) * h / prows;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                long R = 0, G = 0, B = 0, cnt = 0;
                for (int y = y0; y < y1 && y < h; y++)
                    for (int x = x0; x < x1 && x < w; x++) {
                        unsigned char *sp = px + 3 * (y * w + x);
                        R += sp[0]; G += sp[1]; B += sp[2]; cnt++;
                    }
                if (!cnt) cnt = 1;
                rgb[half][0] = (int)(R / cnt);
                rgb[half][1] = (int)(G / cnt);
                rgb[half][2] = (int)(B / cnt);
            }
            /* upper pixel = fg on U+2580, lower pixel = bg */
            printf("\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80",
                   rgb[0][0], rgb[0][1], rgb[0][2],
                   rgb[1][0], rgb[1][1], rgb[1][2]);
        }
        printf("\x1b[0m\n");
    }
    stbi_image_free(px);
    return 0;
}

int art_render_ascii(const uint8_t *img, size_t len, int max_cols, int max_rows) {
    if (chafa_available() && render_chafa(img, len, max_cols, max_rows) == 0)
        return 0;
    return render_halfblocks(img, len, max_cols, max_rows);
}
