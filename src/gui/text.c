/* This file is part of tagplay.
 *
 * tagplay-gui -- truetype text via a baked glyph atlas
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

/* Vendored single-header library: with STBTT_STATIC its unused
 * helpers trip -Wunused-function under our -Wall. Quarantine the
 * include; our own code stays fully warned. */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop
#include "font_jbm.h"
#include "text.h"
#include <string.h>

#define ATLAS_W 512
#define ATLAS_H 512
#define FIRST   32
#define NCHARS  95

static SDL_Texture   *atlas;
static stbtt_bakedchar cd[NCHARS];
static int cw_px, ch_px, ascent_px;

int text_init(SDL_Renderer *r, float px) {
    unsigned char *bmp = malloc(ATLAS_W * ATLAS_H);
    if (!bmp) return -1;
    if (stbtt_BakeFontBitmap(font_jbm_ttf, 0, px, bmp,
                             ATLAS_W, ATLAS_H, FIRST, NCHARS, cd) <= 0) {
        free(bmp);
        return -1;
    }
    /* alpha texture: white glyphs, coverage in alpha */
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC,
                                       ATLAS_W, ATLAS_H);
    if (!t) { free(bmp); return -1; }
    Uint32 *px32 = malloc(ATLAS_W * ATLAS_H * 4);
    if (!px32) { free(bmp); SDL_DestroyTexture(t); return -1; }
    for (int i = 0; i < ATLAS_W * ATLAS_H; i++)
        px32[i] = 0x00FFFFFFu | ((Uint32)bmp[i] << 24);
    SDL_UpdateTexture(t, NULL, px32, ATLAS_W * 4);
    free(px32);
    free(bmp);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    atlas = t;
    stbtt_fontinfo info;
    stbtt_InitFont(&info, font_jbm_ttf, 0);
    float scale = stbtt_ScaleForPixelHeight(&info, px);
    int asc, dsc, gap, adv;
    stbtt_GetFontVMetrics(&info, &asc, &dsc, &gap);
    stbtt_GetCodepointHMetrics(&info, 'M', &adv, NULL);
    ascent_px = (int)(asc * scale + 0.5f);
    ch_px = (int)((asc - dsc + gap) * scale + 0.5f);
    cw_px = (int)(adv * scale + 0.5f);
    return 0;
}

void text_shutdown(void) {
    if (atlas) SDL_DestroyTexture(atlas);
    atlas = NULL;
}

int text_cw(void) { return cw_px; }
int text_ch(void) { return ch_px; }

void text_drawn(SDL_Renderer *r, int x, int y, const char *s,
                int maxch, SDL_Color c) {
    if (!atlas) return;
    SDL_SetTextureColorMod(atlas, c.r, c.g, c.b);
    int n = 0;
    int base = y + ascent_px;
    while (*s && n < maxch) {
        unsigned char ch = (unsigned char)*s;
        if (ch >= 0xC0) {              /* UTF-8 lead: one '~' cell */
            while ((s[1] & 0xC0) == 0x80) s++;
            ch = '~';
        } else if (ch >= 0x80) { s++; continue; }
        if (ch < FIRST || ch >= FIRST + NCHARS) ch = '?';
        const stbtt_bakedchar *b = &cd[ch - FIRST];
        SDL_Rect src = { b->x0, b->y0, b->x1 - b->x0, b->y1 - b->y0 };
        SDL_Rect dst = { x + n * cw_px + (int)(b->xoff + 0.5f),
                         base + (int)(b->yoff + 0.5f),
                         src.w, src.h };
        SDL_RenderCopy(r, atlas, &src, &dst);
        s++; n++;
    }
}

void text_draw(SDL_Renderer *r, int x, int y, const char *s,
               SDL_Color c) {
    text_drawn(r, x, y, s, 1 << 20, c);
}
