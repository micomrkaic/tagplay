/* This file is part of tagplay.
 *
 * tagplay-gui -- the SDL2 face of the tagplay core
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

/* A FACE, not a fork: this GUI owns pixels and nothing else. Every
 * keystroke -- typing, Tab between views, j/k, Space, transport keys,
 * ':' commands -- is translated to the same symbolic codes the
 * terminal sends and fed to browser_key(), the one shared controller.
 * ':dsp am 0.7' typed in the query bar runs exactly the TUI code
 * path; the queue view behind Tab has the TUI's keys (Space pause,
 * arrows seek, J/K reorder, Enter jump). Requests (t inspector, a
 * art, :help) render as overlays. Parity is by construction: there is
 * no second implementation to diverge.
 *
 * --selftest drives the real loop headless under SDL's dummy driver. */

#include <SDL.h>
#include <unistd.h>
#include <limits.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "track.h"
#include "scan.h"
#include "cache.h"
#include "query.h"
#include "browser.h"
#include "app.h"
#include "player.h"
#include "dsp.h"
#include "tags.h"
#include "console.h"
#include "stb_image.h"
#include "viz.h"
#include "fft.h"
#include "text.h"

#define WIN_W 1100
#define WIN_H 720
/* cell metrics come from the baked font (monospaced, so column
 * arithmetic stays exact) */
#define CH    (text_ch())
#define CW    (text_cw())

typedef struct {
    SDL_Renderer *r;
    SDL_Surface *st_surf;      /* selftest software-renderer target */
    SDL_Window   *win;
    browser  b;              /* THE state: shared with the TUI's loop */
    player  *pl;
    const table *tb;
    int      w, h, quit;
    int      overlay;        /* BREQ_* rendered until any key, or 0 */
    const char *overlay_text;
    size_t   overlay_ti;
    SDL_Texture *overlay_art;
    int      art_w, art_h;
    SDL_Rect seek_r, vol_r, btn_prev, btn_play, btn_next, btn_stop;
    int      list_top, row_h;
    /* ---- instrument panel (MG-c) ---- */
    viz      v;
    dsp_meas meas;
    int      meas_ok;
    Uint32   meas_dirty_at;  /* 0 = clean */
    int      instr_on;       /* F2 */
    float    tap_c[8192], tap_p[8192];
    int      tap_rate;
    SDL_Rect wf_r, sp_r, fx_r;
    SDL_Rect fx_btn[8];
    SDL_Rect fx_sl[12];      /* live slider rects this frame */
    int      fx_sl_n;
    int      fx_mode;        /* index into fx_names */
    float    fx_amt[8];      /* remembered amount per mode */
    double   eq_db[10];
    double   bass_db, treble_db;
    int      drag;           /* 0=-, 1 vol, 2 seek, 3 wf, 9 avg, 10+i fx */
    int      sp_mode;        /* VIZ_EMA or VIZ_BOX */
    double   sp_avg;         /* EMA slider frac (0 raw .. 1 slow) */
    double   sp_nfrac;       /* BOX slider frac -> N in 2..64 */
    double   sp_top;         /* auto-ranged axis top, dB */
    SDL_Rect avg_r, sp_mode_r;
} gui;

static const char *const fx_names[8] =
    { "off", "tube", "tape", "vinyl", "shellac", "am", "eq", "tone" };
static const char *const eq_lbl[10] =
    { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };

static void draw_text(SDL_Renderer *r, int x, int y, const char *s,
                      SDL_Color col) {
    text_draw(r, x, y, s, col);
}
static void draw_textn(SDL_Renderer *r, int x, int y, const char *s,
                       int maxch, SDL_Color col) {
    text_drawn(r, x, y, s, maxch, col);
}

static const SDL_Color FG  = { 220, 220, 214, 255 };
static const SDL_Color DIM = { 128, 128, 124, 255 };
static const SDL_Color HL  = { 255, 200,  80, 255 };
static const SDL_Color ACC = { 120, 190, 255, 255 };
static const SDL_Color BG  = {  16,  16,  20, 255 };
static const SDL_Color ROW = {  30,  30,  38, 255 };
static const SDL_Color GRP = { 110, 150, 116, 255 };

static void fill(SDL_Renderer *r, SDL_Rect q, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(r, &q);
}

static void overlay_load_art(gui *g, size_t ti) {
    if (g->overlay_art) {
        SDL_DestroyTexture(g->overlay_art);
        g->overlay_art = NULL;
    }
    const track *t = table_at(g->tb, ti);
    size_t alen = 0;
    uint8_t *img = APP->art_read ? APP->art_read(t, &alen) : NULL;
    if (!img) return;
    int w, h, comp;
    unsigned char *px = stbi_load_from_memory(img, (int)alen, &w, &h,
                                              &comp, 4);
    free(img);
    if (!px) return;
    SDL_Texture *tx = SDL_CreateTexture(g->r, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STATIC, w, h);
    if (tx) {
        SDL_UpdateTexture(tx, NULL, px, w * 4);
        g->overlay_art = tx;
        g->art_w = w;
        g->art_h = h;
    }
    stbi_image_free(px);
}

/* ---- effects panel plumbing ---- */

static void meas_mark(gui *g) { g->meas_dirty_at = SDL_GetTicks(); }

static void fx_apply(gui *g) {
    meas_mark(g);
    dsp_chain *c = player_dsp(g->pl);
    const char *m = fx_names[g->fx_mode];
    if (g->fx_mode == 6)      dsp_set_eq(c, g->eq_db, 10);
    else if (g->fx_mode == 7) dsp_set_tone(c, g->bass_db, g->treble_db);
    else                      dsp_set_mode(c, m, g->fx_amt[g->fx_mode]);
}

static void fx_slide(gui *g, int i, double f) {
    if (g->fx_mode >= 1 && g->fx_mode <= 5) {
        const char *md = fx_names[g->fx_mode];
        if (i == 0) {                 /* the macro knob re-derives */
            g->fx_amt[g->fx_mode] = (float)f;
            dsp_set_mode(player_dsp(g->pl), md,
                         g->fx_amt[g->fx_mode]);
            meas_mark(g);
            return;
        }
        meas_mark(g);
        int pi = i - 1;
        const char *nm; const char *un;
        double lo, hi;
        int ii, il;
        if (dsp_param_info(md, pi, &nm, &un, &lo, &hi, &ii, &il))
            return;
        double v = il ? lo * pow(hi / lo, f) : lo + f * (hi - lo);
        dsp_param_set(player_dsp(g->pl), md, pi, v);
        return;
    }
    if (g->fx_mode == 6) {
        g->eq_db[i] = f * 36.0 - 18.0;
        fx_apply(g);
    } else if (g->fx_mode == 7) {
        if (i == 0) g->bass_db = f * 24.0 - 12.0;
        else        g->treble_db = f * 24.0 - 12.0;
        fx_apply(g);
    }
}

static void slider(gui *g, SDL_Rect r, const char *lbl, double frac,
                   char *vtxt) {
    /* reserve exactly what the label needs and a fixed value field,
     * so the track can never paint over either */
    int lw = ((int)strlen(lbl) + 1) * CW;
    int vw = (vtxt ? (int)strlen(vtxt) + 1 : 0) * CW;
    if (vw < 8 * CW) vw = 8 * CW;
    SDL_Rect tr = { r.x + lw, r.y + CH / 2 - 2, r.w - lw - vw, 6 };
    if (tr.w < 3 * CW) tr.w = 3 * CW;   /* degenerate-width guard */
    draw_text(g->r, r.x, r.y - 2, lbl, DIM);
    fill(g->r, tr, ROW);
    SDL_Rect f = tr;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    f.w = (int)(tr.w * frac);
    fill(g->r, f, ACC);
    if (vtxt) draw_text(g->r, tr.x + tr.w + CW / 2, r.y - 2, vtxt, FG);
    g->fx_sl[g->fx_sl_n] = tr;
    g->fx_sl_n++;
}

static double slider_frac(SDL_Rect tr, int mx) {
    double f = (mx - tr.x) / (double)tr.w;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void draw_instruments(gui *g, const player_status *ps) {
    SDL_Renderer *r = g->r;
    int pad = 10;

    /* waveform strip: full-file peaks, played part accented */
    fill(r, g->wf_r, (SDL_Color){ 22, 22, 28, 255 });
    int mid = g->wf_r.y + g->wf_r.h / 2;
    double played = (ps->playing && ps->dur > 0)
                  ? ps->pos / ps->dur : -1.0;
    for (int i = 0; i < g->v.pk_filled; i++) {
        int x = g->wf_r.x + i * g->wf_r.w / VIZ_PK;
        int y0 = mid - (int)(g->v.pkmax[i] * (g->wf_r.h / 2 - 2));
        int y1 = mid - (int)(g->v.pkmin[i] * (g->wf_r.h / 2 - 2));
        SDL_Color c = (played >= 0 &&
                       i < (int)(played * VIZ_PK)) ? ACC : DIM;
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        SDL_RenderDrawLine(r, x, y0, x, y1);
    }
    if (!g->v.pk_filled)
        draw_text(r, g->wf_r.x + pad, mid - CH / 2,
                  ps->playing ? "(stream: no waveform)" : "(idle)",
                  DIM);
    if (played >= 0) {
        int cx = g->wf_r.x + (int)(played * g->wf_r.w);
        SDL_SetRenderDrawColor(r, HL.r, HL.g, HL.b, 255);
        SDL_RenderDrawLine(r, cx, g->wf_r.y, cx,
                           g->wf_r.y + g->wf_r.h);
    }

    /* spectrum: clean (dim) under processed (accent); log-x; the dB
     * axis auto-ranges to the material (slow peak follower), fixed
     * 80 dB span, gridlines every 20 dB. Controls live in a lane
     * under the plot, not on it. */
    fill(r, g->sp_r, (SDL_Color){ 22, 22, 28, 255 });
    int lane_h = CH + 8;
    SDL_Rect plot = { g->sp_r.x, g->sp_r.y, g->sp_r.w,
                      g->sp_r.h - lane_h };
    if (g->v.sp_primed && g->tap_rate > 0) {
        double f_lo = 30.0, f_hi = g->tap_rate / 2.0;
        double lr = log(f_hi / f_lo);
        /* follow the peak */
        double mxdb = -120.0;
        for (int i = 1; i < VIZ_FFT / 2; i++) {
            double d1 = 10.0 * log10(g->v.sp_clean[i] + 1e-12);
            double d2 = 10.0 * log10(g->v.sp_proc[i] + 1e-12);
            if (d1 > mxdb) mxdb = d1;
            if (d2 > mxdb) mxdb = d2;
        }
        double want = mxdb + 4.0;
        if (want < -40.0) want = -40.0;
        if (want > 5.0) want = 5.0;
        if (g->sp_top == 0.0) g->sp_top = want;
        g->sp_top += (want > g->sp_top ? 0.5 : 0.05) *
                     (want - g->sp_top);
        double top = g->sp_top, bot_db = top - 80.0;
        /* grid */
        for (double gv = floor(top / 20.0) * 20.0; gv > bot_db;
             gv -= 20.0) {
            int gy = plot.y +
                     (int)((top - gv) / 80.0 * plot.h);
            if (gy <= plot.y || gy >= plot.y + plot.h) continue;
            SDL_SetRenderDrawColor(r, 44, 44, 54, 255);
            SDL_RenderDrawLine(r, plot.x, gy, plot.x + plot.w, gy);
            char gl[12];
            snprintf(gl, sizeof gl, "%+.0f", gv);
            draw_text(r, plot.x + plot.w - 4 * CW - 4, gy + 1, gl,
                      (SDL_Color){ 80, 80, 92, 255 });
        }
        int pxc = -1, pyc = 0, pxp = -1, pyp = 0;
        for (int i = 1; i < VIZ_FFT / 2; i++) {
            double fq = (double)i * g->tap_rate / VIZ_FFT;
            if (fq < f_lo) continue;
            int x = plot.x + (int)(log(fq / f_lo) / lr * plot.w);
            double dbc = 10.0 * log10(g->v.sp_clean[i] + 1e-12);
            double dbp = 10.0 * log10(g->v.sp_proc[i] + 1e-12);
            int yc = plot.y + (int)((top - dbc) / 80.0 * plot.h);
            int yp = plot.y + (int)((top - dbp) / 80.0 * plot.h);
            if (yc < plot.y) yc = plot.y;
            if (yc > plot.y + plot.h) yc = plot.y + plot.h;
            if (yp < plot.y) yp = plot.y;
            if (yp > plot.y + plot.h) yp = plot.y + plot.h;
            if (pxc >= 0) {
                SDL_SetRenderDrawColor(r, DIM.r, DIM.g, DIM.b, 255);
                SDL_RenderDrawLine(r, pxc, pyc, x, yc);
            }
            if (pxp >= 0) {
                SDL_SetRenderDrawColor(r, ACC.r, ACC.g, ACC.b, 255);
                SDL_RenderDrawLine(r, pxp, pyp, x, yp);
            }
            pxc = x; pyc = yc; pxp = x; pyp = yp;
        }
    } else
        draw_text(r, plot.x + pad, plot.y + plot.h / 2,
                  "(spectrum: play something)", DIM);
    /* control lane */
    int ly = g->sp_r.y + g->sp_r.h - lane_h + 3;
    g->sp_mode_r = (SDL_Rect){ g->sp_r.x + 4, ly, 5 * CW, CH + 2 };
    fill(r, g->sp_mode_r, ROW);
    draw_text(r, g->sp_mode_r.x + CW / 2, ly + 1,
              g->sp_mode == VIZ_BOX ? "avgN" : "ema", HL);
    g->avg_r = (SDL_Rect){ g->sp_mode_r.x + g->sp_mode_r.w + 2 * CW,
                           ly + CH / 2 - 2, 14 * CW, 6 };
    fill(r, g->avg_r, ROW);
    double lf = g->sp_mode == VIZ_BOX ? g->sp_nfrac : g->sp_avg;
    SDL_Rect af = g->avg_r;
    af.w = (int)(af.w * lf);
    fill(r, af, HL);
    char lv[24];
    if (g->sp_mode == VIZ_BOX)
        snprintf(lv, sizeof lv, "N=%d",
                 2 + (int)(g->sp_nfrac * 62.0 + 0.5));
    else
        snprintf(lv, sizeof lv, "a=%.2f", 1.0 - 0.95 * g->sp_avg);
    draw_text(r, g->avg_r.x + g->avg_r.w + CW, ly + 1, lv, DIM);

    /* effects: mode buttons + the active mode's controls */
    fill(r, g->fx_r, (SDL_Color){ 20, 20, 26, 255 });
    const char *live = dsp_mode_name(player_dsp(g->pl));
    int bw = g->fx_r.w / 4 - 6;
    for (int i = 0; i < 8; i++) {
        int row = i / 4, col = i % 4;
        g->fx_btn[i] = (SDL_Rect){ g->fx_r.x + 4 + col * (bw + 6),
                                   g->fx_r.y + 4 + row * (CH + 12),
                                   bw, CH + 8 };
        int active = !strcmp(live, fx_names[i]);
        fill(r, g->fx_btn[i], active ? ROW : (SDL_Color){ 26, 26, 34,
                                                          255 });
        draw_text(r, g->fx_btn[i].x + 6, g->fx_btn[i].y + 4,
                  fx_names[i], active ? HL : FG);
    }
    int sy = g->fx_r.y + 2 * (CH + 12) + 10;
    if (g->meas_ok && strcmp(live, "off")) {
        char ms[128];
        snprintf(ms, sizeof ms,
                 "THD %.2f%%  H2 %.0fdB  H3 %.0fdB  N %.0fdBFS",
                 g->meas.thd_pct, g->meas.h2_db, g->meas.h3_db,
                 g->meas.noise_dbfs);
        draw_textn(g->r, g->fx_r.x + 6,
                   g->fx_r.y + g->fx_r.h - CH - 4, ms,
                   (g->fx_r.w - 12) / CW, HL);
    }
    g->fx_sl_n = 0;
    char vt[24];
    if (g->fx_mode >= 1 && g->fx_mode <= 5) {
        /* the macro knob, then the mode's true parameters (live from
         * the chain, so an amount morph moves every slider) */
        snprintf(vt, sizeof vt, "%.2f", g->fx_amt[g->fx_mode]);
        slider(g, (SDL_Rect){ g->fx_r.x + 6, sy, g->fx_r.w - 12, CH },
               "amt", g->fx_amt[g->fx_mode], vt);
        const char *md = fx_names[g->fx_mode];
        int np = dsp_param_count(md);
        int colw = (g->fx_r.w - 12) / 2;
        int py0 = sy + CH + 8;
        for (int i = 0; i < np && i < 10; i++) {
            const char *nm, *un;
            double lo, hi, cur = 0;
            int ii, il;
            dsp_param_info(md, i, &nm, &un, &lo, &hi, &ii, &il);
            dsp_param_get(player_dsp(g->pl), md, i, &cur);
            double fr = il
                ? log(cur / lo) / log(hi / lo)
                : (cur - lo) / (hi - lo);
            if (ii) snprintf(vt, sizeof vt, "%d%s", (int)cur, un);
            else if (fabs(cur) >= 100)
                snprintf(vt, sizeof vt, "%.0f%s", cur, un);
            else snprintf(vt, sizeof vt, "%.2g%s", cur, un);
            /* 5 rows per column: row 5 would land on the THD strip
             * at the panel foot (am's 8th, tape's 10th parameter) */
            int col = i / 5, row = i % 5;
            slider(g, (SDL_Rect){ g->fx_r.x + 6 + col * colw,
                                  py0 + row * (CH + 6),
                                  colw - 6, CH },
                   nm, fr, vt);
        }
    } else if (g->fx_mode == 6) {
        int colw = (g->fx_r.w - 12) / 2;
        for (int i = 0; i < 10; i++) {
            int col = i / 5, row = i % 5;
            snprintf(vt, sizeof vt, "%+.0f", g->eq_db[i]);
            slider(g, (SDL_Rect){ g->fx_r.x + 6 + col * colw,
                                  sy + row * (CH + 6),
                                  colw - 6, CH },
                   eq_lbl[i], (g->eq_db[i] + 18.0) / 36.0, vt);
        }
    } else if (g->fx_mode == 7) {
        snprintf(vt, sizeof vt, "%+.0f", g->bass_db);
        slider(g, (SDL_Rect){ g->fx_r.x + 6, sy, g->fx_r.w - 12, CH },
               "bass", (g->bass_db + 12.0) / 24.0, vt);
        snprintf(vt, sizeof vt, "%+.0f", g->treble_db);
        slider(g, (SDL_Rect){ g->fx_r.x + 6, sy + CH + 6,
                              g->fx_r.w - 12, CH },
               "treb", (g->treble_db + 12.0) / 24.0, vt);
    }
}

static void frame(gui *g) {
    SDL_Renderer *r = g->r;
    SDL_GetRendererOutputSize(r, &g->w, &g->h);
    fill(r, (SDL_Rect){ 0, 0, g->w, g->h }, BG);
    int pad = 10;

    player_status ps;
    player_get_status(g->pl, &ps);
    const vec *shown = bmodel_shown(&g->b.m);

    if (g->overlay == BREQ_HELP || g->overlay == BREQ_PAGE ||
        g->overlay == BREQ_DETAIL) {
        int y = pad;
        if (g->overlay == BREQ_HELP || g->overlay == BREQ_PAGE) {
            const char *p = g->overlay == BREQ_HELP ? BROWSER_HELP
                          : (g->overlay_text ? g->overlay_text : "");
            char line[256];
            while (*p && y < g->h - CH) {
                size_t n = strcspn(p, "\n");
                if (n > sizeof line - 1) n = sizeof line - 1;
                memcpy(line, p, n);
                line[n] = 0;
                draw_textn(r, pad, y, line, (g->w - 2 * pad) / CW, FG);
                y += CH + 2;
                p += n + (p[n] == '\n');
            }
        } else {
            const track *t = table_at(g->tb, g->overlay_ti);
            draw_textn(r, pad, y, t->path, (g->w - 2 * pad) / CW, DIM);
            y += CH + 6;
            char hd[192];
            if (APP->detail_header) APP->detail_header(t, hd, sizeof hd);
            else snprintf(hd, sizeof hd, "%s", APP->fmt_name(t->fmt));
            draw_text(r, pad, y, hd, ACC);
            y += CH + 10;
            int txt_w = g->overlay_art ? g->w * 62 / 100 : g->w;
            for (size_t i = 0; i < t->tags.len && y < g->h - 3 * CH;
                 i++) {
                tagkv *kv = vec_at((vec *)&t->tags, i);
                char ln[300];
                snprintf(ln, sizeof ln, "%-14s %s", kv->key, kv->value);
                draw_textn(r, pad, y, ln, (txt_w - 2 * pad) / CW, FG);
                y += CH + 2;
            }
            if (g->overlay_art) {
                int aw = g->w - txt_w - pad;
                int ah = aw * g->art_h / (g->art_w ? g->art_w : 1);
                SDL_Rect dst = { txt_w, pad, aw, ah };
                SDL_RenderCopy(r, g->overlay_art, NULL, &dst);
            }
        }
        draw_text(r, pad, g->h - CH - 6, "[any key returns]", DIM);
        SDL_RenderPresent(r);
        return;
    }
    if (g->overlay == BREQ_ART) {
        if (g->overlay_art) {
            int aw = g->w - 2 * pad, ah = g->h - 3 * CH;
            double s = (double)aw / g->art_w;
            if (s * g->art_h > ah) s = (double)ah / g->art_h;
            SDL_Rect dst = { (g->w - (int)(g->art_w * s)) / 2, pad,
                             (int)(g->art_w * s), (int)(g->art_h * s) };
            SDL_RenderCopy(r, g->overlay_art, NULL, &dst);
        } else draw_text(r, pad, pad, "(no embedded art)", DIM);
        draw_text(r, pad, g->h - CH - 6, "[any key returns]", DIM);
        SDL_RenderPresent(r);
        return;
    }

    const char *focus_tag = g->b.focus == 0 ? "QUERY"
                          : g->b.focus == 1 ? "LIST" : "QUEUE";
    draw_text(r, pad, pad + 2, ">", ACC);
    draw_text(r, pad + 2 * CW, pad + 2, g->b.m.buf, FG);
    if (g->b.focus == 0)
        fill(r, (SDL_Rect){ pad + 2 * CW + (int)g->b.m.cur * CW,
                            pad + 2 + CH, CW, 2 }, HL);
    char right[96];
    snprintf(right, sizeof right, "%zu track%s%s  [%s]", shown->len,
             shown->len == 1 ? "" : "s",
             g->b.m.parse_ok ? "" : " ?", focus_tag);
    draw_text(r, g->w - pad - (int)strlen(right) * CW, pad + 2, right,
              g->b.m.parse_ok ? DIM : HL);

    int top = pad + CH + 14;
    int instr_h = g->instr_on ? 56 + 224 + 12 : 0;
    int bot = g->h - 4 * CH - 26 - instr_h;
    g->list_top = top;
    g->row_h = CH + 6;
    int rows = (bot - top) / g->row_h;
    if (rows < 1) rows = 1;

    if (g->b.focus == 2) {
        draw_text(r, pad, top,
            "QUEUE  Space pause * arrows seek * J/K move * Enter jump",
            ACC);
        if (g->b.qcur < g->b.qoff) g->b.qoff = g->b.qcur;
        if (g->b.qcur >= g->b.qoff + (size_t)(rows - 1))
            g->b.qoff = g->b.qcur - (size_t)(rows - 1) + 1;
        for (int i = 0; i < rows - 1; i++) {
            size_t qi = g->b.qoff + (size_t)i;
            if (qi >= g->b.qview.len) break;
            size_t ti = *(size_t *)vec_at(&g->b.qview, qi);
            const track *t = table_at(g->tb, ti);
            int y = top + (i + 1) * g->row_h;
            if (qi == g->b.qcur)
                fill(r, (SDL_Rect){ 0, y - 2, g->w, CH + 4 }, ROW);
            char who[256];
            APP->identity(t, who, sizeof who);
            if (ps.playing && qi == ps.queue_pos)
                draw_text(r, pad, y, ">", HL);
            char nl[24];
            snprintf(nl, sizeof nl, "%4zu", qi + 1);
            draw_text(r, pad + 2 * CW, y, nl, DIM);
            draw_textn(r, pad + 8 * CW, y, who, (g->w - pad) / CW - 16,
                       qi == ps.queue_pos && ps.playing ? FG : DIM);
            char dur[16];
            fmt_duration(t->duration, dur, sizeof dur);
            draw_text(r, g->w - pad - (int)strlen(dur) * CW, y, dur,
                      DIM);
        }
    } else {
        if (g->b.lcur >= shown->len)
            g->b.lcur = shown->len ? shown->len - 1 : 0;
        if (g->b.lcur < g->b.loff) g->b.loff = g->b.lcur;
        if (g->b.lcur >= g->b.loff + (size_t)rows)
            g->b.loff = g->b.lcur - (size_t)rows + 1;
        char prevgrp[128] = "";
        int have_grp = 0;
        int y = top;
        for (size_t idx = g->b.loff; idx < shown->len; idx++) {
            if (y > bot - g->row_h) break;
            size_t ti = *(size_t *)vec_at((vec *)shown, idx);
            const track *t = table_at(g->tb, ti);
            if (g->b.m.group[0]) {
                const char *gv = track_first_tag(t, g->b.m.group);
                if (!gv) gv = "";
                if (!have_grp || strcmp(gv, prevgrp)) {
                    char gh[160];
                    snprintf(gh, sizeof gh, "-- %s",
                             *gv ? gv : "(none)");
                    draw_textn(r, pad, y, gh, (g->w - pad) / CW, GRP);
                    y += g->row_h;
                    snprintf(prevgrp, sizeof prevgrp, "%s", gv);
                    have_grp = 1;
                    if (y > bot - g->row_h) break;
                }
            }
            if (idx == g->b.lcur && g->b.focus == 1)
                fill(r, (SDL_Rect){ 0, y - 2, g->w, CH + 4 }, ROW);
            int sel = bmodel_sel_find(&g->b.m, ti) >= 0;
            draw_text(r, pad, y, sel ? "[x]" : "[ ]", sel ? HL : DIM);
            char who[256];
            APP->identity(t, who, sizeof who);
            draw_textn(r, pad + 4 * CW, y, who,
                       (g->w - pad) / CW - 18, FG);
            char dur[16];
            fmt_duration(t->duration, dur, sizeof dur);
            char meta[40];
            snprintf(meta, sizeof meta, "%s %s",
                     APP->fmt_name(t->fmt), dur);
            draw_text(r, g->w - pad - (int)strlen(meta) * CW, y, meta,
                      DIM);
            y += g->row_h;
        }
    }

    if (g->instr_on) {
        int iy = g->h - 4 * CH - 14 - instr_h + 4;
        g->wf_r = (SDL_Rect){ pad, iy, g->w - 2 * pad, 56 };
        int sw = (g->w - 2 * pad) * 58 / 100;
        g->sp_r = (SDL_Rect){ pad, iy + 60, sw, 220 };
        g->fx_r = (SDL_Rect){ pad + sw + 8, iy + 60,
                              g->w - 2 * pad - sw - 8, 220 };
        draw_instruments(g, &ps);
    }

    int ty = g->h - 4 * CH - 14;
    if (g->b.msg[0])
        draw_textn(r, pad, ty, g->b.msg, (g->w - 2 * pad) / CW, HL);
    else if (ps.playing && ps.track_index < table_len(g->tb)) {
        const track *t = table_at(g->tb, ps.track_index);
        char who[256];
        APP->identity(t, who, sizeof who);
        char p1[16], p2[16];
        fmt_duration(ps.pos, p1, sizeof p1);
        fmt_duration(ps.dur, p2, sizeof p2);
        char np[340];
        snprintf(np, sizeof np, "%s %s  %s/%s  [%zu/%zu]",
                 ps.playing == 2 ? "||" : ">", who, p1,
                 ps.dur > 0 ? p2 : "~", ps.queue_pos + 1,
                 ps.queue_len);
        draw_textn(r, pad, ty, np, (g->w - 2 * pad) / CW, FG);
    }
    g->seek_r = (SDL_Rect){ pad, ty + CH + 6, g->w - 2 * pad, 8 };
    fill(r, g->seek_r, ROW);
    if (ps.playing && ps.dur > 0) {
        SDL_Rect done = g->seek_r;
        done.w = (int)((double)done.w * (ps.pos / ps.dur));
        fill(r, done, ACC);
    }
    int by = ty + CH + 20;
    const char *labels[4] = { "|<", ps.playing == 2 ? " >" : "||",
                              ">|", "[]" };
    SDL_Rect *btns[4] = { &g->btn_prev, &g->btn_play, &g->btn_next,
                          &g->btn_stop };
    int bx = pad;
    for (int i = 0; i < 4; i++) {
        *btns[i] = (SDL_Rect){ bx, by, 4 * CW, CH + 8 };
        fill(r, *btns[i], ROW);
        draw_text(r, bx + CW, by + 4, labels[i], FG);
        bx += 4 * CW + 8;
    }
    char dl[40];
    snprintf(dl, sizeof dl, "dsp:%s",
             dsp_mode_name(player_dsp(g->pl)));
    draw_text(r, bx + 12, by + 4, dl, DIM);
    {
        player_status nps;
        player_get_status(g->pl, &nps);
        if (nps.null_output)
            draw_text(r, bx + 12 + (int)(strlen(dl) + 2) * CW, by + 4,
                      "(NO AUDIO)", HL);
    }
    g->vol_r = (SDL_Rect){ g->w - pad - 22 * CW, by + CH / 2 - 2,
                           16 * CW, 8 };
    fill(r, g->vol_r, ROW);
    double gn = dsp_gain(player_dsp(g->pl));
    SDL_Rect vf = g->vol_r;
    vf.w = (int)(vf.w * (gn / 2.0));
    fill(r, vf, HL);
    char vl[16];
    snprintf(vl, sizeof vl, "%d%%", (int)(gn * 100 + 0.5));
    draw_text(r, g->vol_r.x + g->vol_r.w + CW, by + 4, vl, DIM);

    SDL_RenderPresent(r);
}

/* ---- feeding the shared controller ---- */

static void service(gui *g) {
    switch (g->b.req) {
    case BREQ_DETAIL:
    case BREQ_ART:
        g->overlay = g->b.req;
        g->overlay_ti = g->b.req_ti;
        overlay_load_art(g, g->b.req_ti);
        break;
    case BREQ_HELP:  g->overlay = BREQ_HELP; break;
    case BREQ_PAGE:
        g->overlay = BREQ_PAGE;
        g->overlay_text = g->b.req_text;
        break;
    case BREQ_STATS:
        snprintf(g->b.msg, sizeof g->b.msg, ":stats is terminal-only");
        break;
    case BREQ_LS:
        snprintf(g->b.msg, sizeof g->b.msg, ":ls is terminal-only");
        break;
    case BREQ_QUIT:  g->quit = 1; break;
    default: break;
    }
    g->b.req = BREQ_NONE;
}

static void key(gui *g, int sym) {
    if (g->overlay) {           /* any key returns, like the TUI */
        g->overlay = 0;
        return;
    }
    browser_key(&g->b, sym);
    service(g);
}

static void handle(gui *g, const SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT: g->quit = 1; break;
    case SDL_TEXTINPUT:
        for (const char *s = e->text.text; *s; s++)
            if ((unsigned char)*s >= 32 && (unsigned char)*s < 127)
                key(g, (unsigned char)*s);
        break;
    case SDL_KEYDOWN: {
        SDL_Keycode k = e->key.keysym.sym;
        int ctrl = e->key.keysym.mod & KMOD_CTRL;
        int shift = e->key.keysym.mod & KMOD_SHIFT;
        if (ctrl && k == SDLK_p) key(g, 16);
        else if (ctrl && k == SDLK_n) key(g, 14);
        else if (ctrl && k == SDLK_b) key(g, 2);
        else if (ctrl && k == SDLK_u) key(g, 21);
        else if (ctrl && k == SDLK_w) key(g, 23);
        else if (k == SDLK_RETURN) key(g, '\r');
        else if (k == SDLK_TAB) key(g, '\t');
        else if (k == SDLK_BACKSPACE) key(g, 127);
        else if (k == SDLK_ESCAPE) key(g, K_ESC);
        else if (k == SDLK_UP) key(g, K_UP);
        else if (k == SDLK_DOWN) key(g, K_DOWN);
        else if (k == SDLK_LEFT) key(g, shift ? K_SLEFT : K_LEFT);
        else if (k == SDLK_RIGHT) key(g, shift ? K_SRIGHT : K_RIGHT);
        else if (k == SDLK_PAGEUP) key(g, K_PGUP);
        else if (k == SDLK_PAGEDOWN) key(g, K_PGDN);
        else if (k == SDLK_HOME) key(g, K_HOME);
        else if (k == SDLK_END) key(g, K_END);
        else if (k == SDLK_DELETE) key(g, K_DEL);
        else if (k == SDLK_F2) g->instr_on = !g->instr_on;
        break;
    }
    case SDL_MOUSEBUTTONUP:
        g->drag = 0;
        break;
    case SDL_MOUSEMOTION: {
        if (!g->drag) break;
        int mx = e->motion.x;
        if (g->drag == 9) {
            if (g->sp_mode == VIZ_BOX)
                g->sp_nfrac = slider_frac(g->avg_r, mx);
            else
                g->sp_avg = slider_frac(g->avg_r, mx);
        }
        else if (g->drag == 1)
            dsp_set_gain(player_dsp(g->pl),
                         2.0 * (mx - g->vol_r.x) /
                         (double)g->vol_r.w);
        else if (g->drag == 3) {
            player_status ps;
            player_get_status(g->pl, &ps);
            if (ps.playing && ps.dur > 0) {
                double f = (mx - g->wf_r.x) / (double)g->wf_r.w;
                if (f < 0) f = 0;
                if (f > 1) f = 1;
                player_seek(g->pl, ps.dur * f);
            }
        } else if (g->drag >= 10) {
            int i = g->drag - 10;
            if (i < g->fx_sl_n)
                fx_slide(g, i, slider_frac(g->fx_sl[i], mx));
        }
        break;
    }
    case SDL_MOUSEWHEEL:
        if (g->b.focus == 2) {
            if (e->wheel.y > 0 && g->b.qoff) g->b.qoff--;
            else if (e->wheel.y < 0 &&
                     g->b.qoff + 1 < g->b.qview.len) g->b.qoff++;
        } else {
            if (e->wheel.y > 0 && g->b.loff) g->b.loff--;
            else if (e->wheel.y < 0) g->b.loff++;
        }
        break;
    case SDL_MOUSEBUTTONDOWN: {
        int mx = e->button.x, my = e->button.y;
        SDL_Point p = { mx, my };
        if (g->overlay) { g->overlay = 0; break; }
        if (g->instr_on && SDL_PointInRect(&p, &g->sp_mode_r)) {
            g->sp_mode = g->sp_mode == VIZ_BOX ? VIZ_EMA : VIZ_BOX;
        } else if (g->instr_on && SDL_PointInRect(&p, &g->avg_r)) {
            if (g->sp_mode == VIZ_BOX)
                g->sp_nfrac = slider_frac(g->avg_r, mx);
            else
                g->sp_avg = slider_frac(g->avg_r, mx);
            g->drag = 9;
        } else if (g->instr_on && SDL_PointInRect(&p, &g->wf_r)) {
            player_status ps;
            player_get_status(g->pl, &ps);
            if (ps.playing && ps.dur > 0)
                player_seek(g->pl, ps.dur * (mx - g->wf_r.x) /
                                   (double)g->wf_r.w);
            g->drag = 3;
        } else if (g->instr_on && SDL_PointInRect(&p, &g->fx_r)) {
            for (int i = 0; i < 8; i++)
                if (SDL_PointInRect(&p, &g->fx_btn[i])) {
                    g->fx_mode = i;
                    fx_apply(g);
                    return;
                }
            for (int i = 0; i < g->fx_sl_n; i++)
                if (mx >= g->fx_sl[i].x - CW &&
                    mx <= g->fx_sl[i].x + g->fx_sl[i].w + CW &&
                    my >= g->fx_sl[i].y - 6 &&
                    my <= g->fx_sl[i].y + g->fx_sl[i].h + 6) {
                    g->drag = 10 + i;
                    fx_slide(g, i, slider_frac(g->fx_sl[i], mx));
                    return;
                }
        } else if (SDL_PointInRect(&p, &g->btn_play))
            key(g, g->b.focus == 2 ? ' ' : 16);
        else if (SDL_PointInRect(&p, &g->btn_next)) key(g, 14);
        else if (SDL_PointInRect(&p, &g->btn_prev)) key(g, 2);
        else if (SDL_PointInRect(&p, &g->btn_stop)) {
            player_stop(g->pl);
            snprintf(g->b.msg, sizeof g->b.msg,
                     "stopped (queue kept)");
        } else if (SDL_PointInRect(&p, &g->seek_r)) {
            player_status ps;
            player_get_status(g->pl, &ps);
            if (ps.playing && ps.dur > 0)
                player_seek(g->pl, ps.dur * (mx - g->seek_r.x) /
                                   (double)g->seek_r.w);
        } else if (SDL_PointInRect(&p, &g->vol_r)) {
            dsp_set_gain(player_dsp(g->pl),
                         2.0 * (mx - g->vol_r.x) /
                         (double)g->vol_r.w);
            g->drag = 1;
        } else if (my >= g->list_top && g->b.focus != 2) {
            const vec *shown = bmodel_shown(&g->b.m);
            int y = g->list_top;
            char prevgrp[128] = "";
            int have_grp = 0;
            for (size_t idx = g->b.loff; idx < shown->len; idx++) {
                size_t ti = *(size_t *)vec_at((vec *)shown, idx);
                const track *t = table_at(g->tb, ti);
                if (g->b.m.group[0]) {
                    const char *gv = track_first_tag(t, g->b.m.group);
                    if (!gv) gv = "";
                    if (!have_grp || strcmp(gv, prevgrp)) {
                        y += g->row_h;
                        snprintf(prevgrp, sizeof prevgrp, "%s", gv);
                        have_grp = 1;
                    }
                }
                if (my >= y && my < y + g->row_h) {
                    g->b.focus = 1;
                    g->b.lcur = idx;
                    if (e->button.clicks >= 2) key(g, '\r');
                    else if (mx < 10 + 4 * CW)
                        bmodel_sel_toggle(&g->b.m, ti);
                    break;
                }
                y += g->row_h;
            }
        } else if (my >= g->list_top && g->b.focus == 2) {
            int row = (my - g->list_top) / g->row_h - 1;
            if (row >= 0) {
                size_t qi = g->b.qoff + (size_t)row;
                if (qi < g->b.qview.len) {
                    g->b.qcur = qi;
                    if (e->button.clicks >= 2) key(g, '\r');
                }
            }
        }
        break;
    }
    default: break;
    }
}

/* the TUI's tick, GUI edition: player notes + live queue refresh */
static void tick(gui *g) {
    if (APP->poll_msg)
        APP->poll_msg(g->b.ui, g->b.msg, sizeof g->b.msg);
    if (g->b.focus == 2 && APP->alt_snapshot)
        APP->alt_snapshot(g->b.ui, &g->b);
    player_status ps;
    player_get_status(g->pl, &ps);
    if (ps.playing && ps.track_index < table_len(g->tb)) {
        const track *t = table_at(g->tb, ps.track_index);
        if (t->duration > 0)
            viz_want_peaks(&g->v, g->tb, ps.track_index);
        else if (g->v.pk_track != (long)ps.track_index) {
            viz_no_peaks(&g->v);
            g->v.pk_track = (long)ps.track_index;
        }
    }
    if (g->meas_dirty_at &&
        SDL_GetTicks() - g->meas_dirty_at > 400) {
        g->meas_ok = dsp_measure(player_dsp(g->pl), &g->meas) == 0;
        g->meas_dirty_at = 0;
    }
    int rate = player_viz(g->pl, g->tap_c, g->tap_p, VIZ_FFT);
    if (rate > 0) {
        g->tap_rate = rate;
        double gn = dsp_gain(player_dsp(g->pl));
        double comp = gn > 1e-4 ? 1.0 / gn : 1.0;
        double prm = g->sp_mode == VIZ_BOX
                   ? 2.0 + g->sp_nfrac * 62.0
                   : 1.0 - 0.95 * g->sp_avg;
        viz_fold_spectrum(&g->v, g->tap_c, g->tap_p, VIZ_FFT,
                          g->sp_mode, prm, comp);
    }
}

/* ---- selftest ---- */

static void push_text(const char *s) {
    for (; *s; s++) {
        SDL_Event e = { 0 };
        e.type = SDL_TEXTINPUT;
        e.text.text[0] = *s;
        SDL_PushEvent(&e);
    }
}
static void push_key(SDL_Keycode k, Uint16 mod) {
    SDL_Event e = { 0 };
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = k;
    e.key.keysym.mod = mod;
    SDL_PushEvent(&e);
}
static void pump(gui *g) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle(g, &e);
    tick(g);
}

static int selftest(gui *g) {
    int fails = 0;
#define CHK(name, cond) do { \
        if (cond) printf("PASS %s\n", name); \
        else { printf("FAIL %s\n", name); fails++; } } while (0)
    push_text("radio station");
    pump(g);
    CHK("spaces in query + stations loaded",
        bmodel_shown(&g->b.m)->len == 32);
    push_key(SDLK_u, KMOD_LCTRL);
    push_text("bach");
    pump(g);
    CHK("query filters", bmodel_shown(&g->b.m)->len == 4);
    push_key(SDLK_TAB, 0);
    pump(g);
    CHK("Tab -> list focus", g->b.focus == 1);
    push_text(" ");
    push_key(SDLK_DOWN, 0);
    push_text(" ");
    pump(g);
    CHK("Space toggles marks (TUI semantics)", g->b.m.sel.len == 2);
    push_key(SDLK_RETURN, 0);
    pump(g);
    SDL_Delay(300);
    player_status ps;
    player_get_status(g->pl, &ps);
    CHK("Enter -> queue view, playing, marks consumed",
        g->b.focus == 2 && ps.queue_len == 2 && g->b.m.sel.len == 0);
    push_text(" ");
    pump(g);
    SDL_Delay(150);
    player_get_status(g->pl, &ps);
    CHK("Space pauses in queue view", ps.playing == 2);
    push_text("J");
    pump(g);
    CHK("J reorders + cursor follows", g->b.qcur == 1);
    push_key(SDLK_TAB, 0);
    pump(g);
    CHK("Tab cycles back, query kept",
        g->b.focus == 0 && !strcmp(g->b.m.buf, "bach"));
    push_key(SDLK_u, KMOD_LCTRL);   /* clear "bach" first */
    push_text(":dsp am 0.7");
    push_key(SDLK_RETURN, 0);
    pump(g);
    CHK("':dsp am' via the one code path",
        !strcmp(dsp_mode_name(player_dsp(g->pl)), "am"));
    push_key(SDLK_u, KMOD_LCTRL);
    push_text(":vol 80");
    push_key(SDLK_RETURN, 0);
    pump(g);
    CHK(":vol via the one code path",
        (int)(dsp_gain(player_dsp(g->pl)) * 100 + 0.5) == 80);
    push_key(SDLK_u, KMOD_LCTRL);
    push_text("bach");
    push_key(SDLK_TAB, 0);          /* query -> list */
    push_key(SDLK_TAB, 0);          /* list -> queue */
    pump(g);
    CHK("Tab Tab reaches queue view", g->b.focus == 2);
    push_text("t");                 /* queue-view inspector path */
    pump(g);
    CHK("t inspector overlay", g->overlay == BREQ_DETAIL);
    push_key(SDLK_ESCAPE, 0);
    pump(g);
    CHK("overlay dismissed", g->overlay == 0);
    /* ---- MG-c instruments ---- */
    {
        /* fft sanity: 1 kHz sine at 48 kHz peaks in the right bin */
        static double x[VIZ_FFT], db[VIZ_FFT / 2];
        for (int i = 0; i < VIZ_FFT; i++)
            x[i] = sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        fft_spectrum_db(x, VIZ_FFT, db);
        int mx = 1;
        for (int i = 2; i < VIZ_FFT / 2; i++)
            if (db[i] > db[mx]) mx = i;
        double fpk = (double)mx * 48000.0 / VIZ_FFT;
        CHK("fft: 1 kHz sine lands on 1 kHz",
            fpk > 950.0 && fpk < 1050.0 && db[mx] > -3.0);
    }
    {
        /* live tap: something in the rings while playing */
        SDL_Delay(400);
        tick(g);
        int rate = g->tap_rate;
        double e = 0;
        for (int i = 0; i < VIZ_FFT; i++)
            e += g->tap_p[i] * g->tap_p[i];
        CHK("viz tap alive during playback", rate > 0 && e > 1e-6);
        /* am at full strength: processed must differ from clean.
         * The fixtures are one second long, so restart the current
         * queue entry (Enter in the queue view) and set the mode
         * through the panel, then wait for fresh frames. */
        g->fx_mode = 5;                     /* am */
        g->fx_amt[5] = 1.0f;
        fx_apply(g);
        push_key(SDLK_RETURN, 0);           /* queue view: jump/restart */
        pump(g);
        double d = 0;
        int dwait = 0;
        while (d < 1e-6 && dwait < 4000) {
            SDL_Delay(100);
            dwait += 100;
            tick(g);
            d = 0;
            for (int i = 0; i < VIZ_FFT; i++) {
                double t = g->tap_p[i] - g->tap_c[i];
                d += t * t;
            }
        }
        CHK("clean vs processed diverge under am", d > 1e-6);
        /* audio-fail witness + unlatch: force a device refusal, restart
         * the track (out_open runs per track open), expect null_output
         * and a loud note; clear the force, restart, expect recovery. */
        player_test_audiofail = 1;
        push_key(SDLK_RETURN, 0);
        pump(g);
        int nwait = 0; player_status fs; fs.null_output = 0;
        while (nwait < 4000) {
            SDL_Delay(100); nwait += 100; tick(g);
            player_get_status(g->pl, &fs);
            if (fs.null_output) break;
        }
        CHK("audio failure is witnessed",
            fs.null_output && strstr(fs.note, "refused") != NULL);
        player_test_audiofail = 0;
        push_key(SDLK_RETURN, 0);
        pump(g);
        nwait = 0;
        while (nwait < 4000) {
            SDL_Delay(100); nwait += 100; tick(g);
            player_get_status(g->pl, &fs);
            if (!fs.null_output) break;
        }
        CHK("audio failure does not latch", !fs.null_output);
        /* full-file peaks build for a real file */
        player_status ps;
        player_get_status(g->pl, &ps);
        int waited = 0;
        while (g->v.pk_filled < 4 && waited < 4000) {
            tick(g);
            SDL_Delay(50);
            waited += 50;
        }
        float pkm = 0;
        for (int i = 0; i < g->v.pk_filled; i++)
            if (g->v.pkmax[i] > pkm) pkm = g->v.pkmax[i];
        CHK("waveform peaks built in background",
            g->v.pk_filled >= 4 && pkm > 0.01f);
        /* effect panel state drives the real chain */
        g->fx_mode = 6;
        g->eq_db[0] = 9.0;
        fx_apply(g);
        CHK("eq panel drives dsp",
            !strcmp(dsp_mode_name(player_dsp(g->pl)), "eq"));
        /* volume must NOT split the overlay: dsp off, volume 40%,
         * restart, fold fresh -- clean and processed spectra agree */
        g->fx_mode = 0;
        fx_apply(g);
        dsp_set_gain(player_dsp(g->pl), 0.4);
        g->v.sp_primed = 0;
        g->sp_mode = VIZ_EMA;
        g->sp_avg = 0.0;                /* raw frames for the check */
        push_key(SDLK_RETURN, 0);       /* queue view: restart track */
        pump(g);
        SDL_Delay(400);
        tick(g);
        double dsum = 0;
        int nb = 0;
        for (int i = 8; i < 400; i++) {
            double dbc = 10.0 * log10(g->v.sp_clean[i] + 1e-12);
            double dbp = 10.0 * log10(g->v.sp_proc[i] + 1e-12);
            if (dbc > -70.0) { dsum += fabs(dbp - dbc); nb++; }
        }
        CHK("volume does not split the overlay (dsp off)",
            nb > 4 && dsum / nb < 2.0);
        /* a simulated amount drag: many rapid updates on one mode;
         * the chain must keep its mode and match gain (no re-measure
         * churn -- the click fix) */
        dsp_set_mode(player_dsp(g->pl), "vinyl", 0.30);
        for (int i = 0; i <= 40; i++)
            dsp_set_mode(player_dsp(g->pl), "vinyl",
                         0.30 + 0.01 * i);
        CHK("amount drag keeps the chain live",
            !strcmp(dsp_mode_name(player_dsp(g->pl)), "vinyl"));
        push_key(SDLK_F2, 0);
        pump(g);
        CHK("F2 hides instruments", g->instr_on == 0);
        push_key(SDLK_F2, 0);
        pump(g);
        /* measurements: tube must distort with even harmonics, off
         * must be clean, vinyl must hiss; ':dsp show' pages in both
         * faces */
        {
            dsp_chain *dc = dsp_create();
            dsp_on_format(dc, 44100, 2);
            dsp_meas mm;
            dsp_set_mode(dc, "tube", 0.8);
            dsp_measure(dc, &mm);
            CHK("meas: tube distorts, H2 present",
                mm.thd_pct > 0.5 && mm.h2_db > -60.0);
            dsp_set_mode(dc, "off", 0.0);
            dsp_measure(dc, &mm);
            CHK("meas: off is clean",
                mm.thd_pct < 0.05 && mm.noise_dbfs <= -110.0);
            dsp_set_mode(dc, "vinyl", 0.7);
            dsp_measure(dc, &mm);
            CHK("meas: vinyl has a noise floor",
                mm.noise_dbfs > -100.0 && mm.noise_dbfs < -10.0);
            dsp_destroy(dc);
        }
        {
            push_key(SDLK_u, KMOD_LCTRL);
            push_text(":fft ema 0.25");
            push_key(SDLK_RETURN, 0);
            pump(g);
            CHK("':fft' routes through the shared path",
                strstr(g->b.msg, "ema weight 0.25") != NULL);
            push_key(SDLK_u, KMOD_LCTRL);
            push_text(":dsp vinyl 0.5");
            push_key(SDLK_RETURN, 0);
            push_key(SDLK_u, KMOD_LCTRL);
            push_text(":dsp show");
            push_key(SDLK_RETURN, 0);
            pump(g);
            CHK("':dsp show' pages in the GUI",
                g->overlay == BREQ_PAGE && g->overlay_text &&
                strstr(g->overlay_text, "wow") &&
                strstr(g->overlay_text, "THD"));
            push_key(SDLK_ESCAPE, 0);
            pump(g);
        }
        /* low-rate streams: vinyl/tape corners above Nyquist used to
         * design NaN filters -- finite, audible output at 22.05 kHz
         * is the regression fence */
        {
            dsp_chain *dc = dsp_create();
            dsp_on_format(dc, 22050, 2);
            dsp_set_mode(dc, "vinyl", 0.5);
            float blk[1024 * 2];
            double last = 0;
            int bad = 0;
            for (int b2 = 0; b2 < 40; b2++) {
                for (int i = 0; i < 1024; i++) {
                    float s = 0.4f *
                        (float)sin(2.0 * M_PI * 220.0 *
                                   (b2 * 1024 + i) / 22050.0);
                    blk[2 * i] = blk[2 * i + 1] = s;
                }
                dsp_process(dc, blk, 1024);
                double e = 0;
                for (int i = 0; i < 2048; i++) {
                    if (!isfinite(blk[i])) bad++;
                    e += (double)blk[i] * blk[i];
                }
                last = e / 2048;
            }
            dsp_destroy(dc);
            CHK("vinyl at 22.05 kHz: finite and audible",
                bad == 0 && last > 1e-4);
        }
        /* the rewritten AM (13-param broadcast chain) through the same
         * fence at full night amount: whistle_hz 10 kHz > Nyquist/2
         * exercises the oscillator clamp; output must stay finite and
         * audible, and night must differ from day */
        {
            dsp_chain *dc = dsp_create();
            dsp_on_format(dc, 22050, 2);
            dsp_set_mode(dc, "am", 1.0);
            float blk[1024 * 2];
            double last = 0, eday = 0, enight = 0;
            int bad = 0;
            for (int pass = 0; pass < 2; pass++) {
                dsp_set_mode(dc, "am", pass ? 1.0 : 0.25);
                for (int b2 = 0; b2 < 40; b2++) {
                    for (int i = 0; i < 1024; i++) {
                        float s = 0.4f *
                            (float)sin(2.0 * M_PI * 220.0 *
                                       (b2 * 1024 + i) / 22050.0);
                        blk[2 * i] = blk[2 * i + 1] = s;
                    }
                    dsp_process(dc, blk, 1024);
                    double e = 0;
                    for (int i = 0; i < 2048; i++) {
                        if (!isfinite(blk[i])) bad++;
                        e += (double)blk[i] * blk[i];
                    }
                    last = e / 2048;
                }
                if (pass) enight = last; else eday = last;
            }
            dsp_destroy(dc);
            CHK("am night at 22.05 kHz: finite and audible",
                bad == 0 && enight > 1e-4);
            CHK("am day vs night differ",
                fabs(eday - enight) / (eday + 1e-12) > 0.01 ||
                eday > 1e-4);
        }
        /* live mode switches must not stall the stream: prime a
         * bare chain with a sine, flip modes mid-stream, and demand
         * audio in EVERY post-switch block (the old reset emitted
         * ~0.37 s of silence while re-priming) */
        {
            dsp_chain *dc = dsp_create();
            dsp_on_format(dc, 44100, 2);
            dsp_set_mode(dc, "tube", 0.3);
            float blk[1024 * 2];
            int primed = 0, silent_after = 0;
            for (int b2 = 0; b2 < 80 && !primed; b2++) {
                for (int i = 0; i < 1024; i++) {
                    float s = 0.4f *
                        (float)sin(2.0 * M_PI * 220.0 *
                                   (b2 * 1024 + i) / 44100.0);
                    blk[2 * i] = blk[2 * i + 1] = s;
                }
                dsp_process(dc, blk, 1024);
                double e = 0;
                for (int i = 0; i < 2048; i++)
                    e += (double)blk[i] * blk[i];
                if (e / 2048 > 1e-4) primed = 1;
            }
            dsp_set_mode(dc, "vinyl", 0.6);
            for (int b2 = 0; b2 < 24; b2++) {
                for (int i = 0; i < 1024; i++) {
                    float s = 0.4f *
                        (float)sin(2.0 * M_PI * 220.0 *
                                   ((80 + b2) * 1024 + i) / 44100.0);
                    blk[2 * i] = blk[2 * i + 1] = s;
                }
                dsp_process(dc, blk, 1024);
                double e = 0;
                for (int i = 0; i < 2048; i++)
                    e += (double)blk[i] * blk[i];
                if (e / 2048 < 1e-5) silent_after++;
            }
            dsp_destroy(dc);
            CHK("mode switch keeps the stream hot (no dropout)",
                primed && silent_after == 0);
        }
        /* granular registry: names round-trip, values clamp, the
         * command language reaches the same knobs */
        {
            dsp_set_mode(player_dsp(g->pl), "vinyl", 0.5);
            CHK("registry: vinyl exposes its true parameters",
                dsp_param_count("vinyl") >= 8);
            dsp_param_set_name(player_dsp(g->pl), "wow", 12.5);
            double wv = 0;
            const char *nm;
            for (int i = 0; i < dsp_param_count("vinyl"); i++)
                if (!dsp_param_info("vinyl", i, &nm, NULL, NULL,
                                    NULL, NULL, NULL) &&
                    !strcmp(nm, "wow"))
                    dsp_param_get(player_dsp(g->pl), "vinyl", i,
                                  &wv);
            CHK("registry: set-by-name round-trips",
                wv > 12.49 && wv < 12.51);
            dsp_param_set_name(player_dsp(g->pl), "wow", 9999.0);
            for (int i = 0; i < dsp_param_count("vinyl"); i++)
                if (!dsp_param_info("vinyl", i, &nm, NULL, NULL,
                                    NULL, NULL, NULL) &&
                    !strcmp(nm, "wow"))
                    dsp_param_get(player_dsp(g->pl), "vinyl", i,
                                  &wv);
            CHK("registry: values clamp to range", wv <= 30.0);
            /* the shared command path reaches the same knob */
            push_key(SDLK_TAB, 0);      /* queue -> query */
            push_key(SDLK_u, KMOD_LCTRL);
            push_text(":dsp set hiss -42");
            push_key(SDLK_RETURN, 0);
            pump(g);
            double hv = 0;
            for (int i = 0; i < dsp_param_count("vinyl"); i++)
                if (!dsp_param_info("vinyl", i, &nm, NULL, NULL,
                                    NULL, NULL, NULL) &&
                    !strcmp(nm, "hiss"))
                    dsp_param_get(player_dsp(g->pl), "vinyl", i,
                                  &hv);
            CHK("':dsp set' drives the same registry",
                hv > -42.01 && hv < -41.99);
            /* the macro knob re-derives over manual tweaks */
            dsp_set_mode(player_dsp(g->pl), "vinyl", 1.0);
            for (int i = 0; i < dsp_param_count("vinyl"); i++)
                if (!dsp_param_info("vinyl", i, &nm, NULL, NULL,
                                    NULL, NULL, NULL) &&
                    !strcmp(nm, "hiss"))
                    dsp_param_get(player_dsp(g->pl), "vinyl", i,
                                  &hv);
            CHK("amount is the macro: rederives manual tweaks",
                hv > -42.0 + 0.5 || hv < -42.0 - 0.5);
        }
        /* boxcar: mean of the last N frames, verified on synthetic
         * sines of two amplitudes (power ratio 1 : 0.25) */
        {
            static float s1[VIZ_FFT], s2[VIZ_FFT];
            for (int i = 0; i < VIZ_FFT; i++) {
                s1[i] = (float)sin(2.0 * M_PI * 100.0 * i / VIZ_FFT);
                s2[i] = 0.5f * s1[i];
            }
            viz w;
            viz_init(&w);
            for (int k = 0; k < 3; k++)
                viz_fold_spectrum(&w, s1, s1, VIZ_FFT, VIZ_BOX, 4,
                                  1.0);
            viz_fold_spectrum(&w, s2, s2, VIZ_FFT, VIZ_BOX, 4, 1.0);
            int pk = 1;
            for (int i = 2; i < VIZ_FFT / 2; i++)
                if (w.sp_clean[i] > w.sp_clean[pk]) pk = i;
            /* mean over {1,1,1,0.25} of full-scale power = 0.8125 */
            CHK("boxcar mean is the plain mean",
                pk == 100 && w.sp_clean[pk] > 0.77 &&
                w.sp_clean[pk] < 0.86);
            viz_shutdown(&w);
        }
    }
    push_key(SDLK_TAB, 0);          /* back to query focus */
    push_key(SDLK_u, KMOD_LCTRL);
    push_text(":q");
    push_key(SDLK_RETURN, 0);
    pump(g);
    CHK(":q quits", g->quit == 1);
    g->quit = 0;
    frame(g);
    CHK("frame renders", 1);
    return fails;
}

#ifdef __EMSCRIPTEN__
/* ---- the browser face plumbing ----------------------------------
 * The page's drop handler writes files into MEMFS under /music and
 * calls web_rescan(); the main loop is a browser callback. */
static gui *WG;
static table *WTB;

static void em_frame(void) {
    gui *g = WG;
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle(g, &e);
    static Uint32 last_tick;
    Uint32 now = SDL_GetTicks();
    if (now - last_tick > 200) {
        tick(g);
        last_tick = now;
    }
    frame(g);
    if (g->quit) emscripten_cancel_main_loop();
}

EMSCRIPTEN_KEEPALIVE
void web_rescan(void) {
    if (!WG || !WTB) return;
    table none;
    table_init(&none);
    size_t added = scan_dir("/music", WTB, &none);
    table_free(&none);
    bmodel_rerun(&WG->b.m);
    snprintf(WG->b.msg, sizeof WG->b.msg,
             "%zu file%s added -- %zu total", added,
             added == 1 ? "" : "s", table_len(WTB));
}
#endif

/* ---- main ---- */

int main(int argc, char **argv) {
    int st = argc > 1 && !strcmp(argv[1], "--selftest");
    if (st && argc > 2) {
        /* the selftest is a scripted play against the fixture library;
         * running it on a real collection asserts nonsense and is
         * refused outright (tests/make_fixtures.sh DIR makes one) */
        char sent[PATH_MAX];
        snprintf(sent, sizeof sent, "%s/.tagplay-fixtures", argv[2]);
        if (access(sent, F_OK) != 0) {
            fprintf(stderr, "tagplay-gui: --selftest needs the fixture "
                    "library, not a music collection.\n"
                    "  ./tests/make_fixtures.sh /tmp/testlib && "
                    "./tagplay-gui --selftest /tmp/testlib\n");
            return 2;
        }
    }
    if (argc <= (st ? 2 : 1)) {
        fprintf(stderr,
                "usage: tagplay-gui [--selftest] MUSIC_DIR...\n");
        return 2;
    }
    if (st) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);

    table cached, tb;
    table_init(&cached);
    table_init(&tb);
    char cp[4096];
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) snprintf(cp, sizeof cp, "%s/%s/%s", xdg,
                              APP->name, APP->cache_file);
    else snprintf(cp, sizeof cp, "%s/.cache/%s/%s",
                  getenv("HOME") ? getenv("HOME") : ".",
                  APP->name, APP->cache_file);
    cache_load(cp, &cached);
    size_t parsed = 0;
    for (int i = st ? 2 : 1; i < argc; i++)
        parsed += scan_dir(argv[i], &tb, &cached);
    table_free(&cached);
    if (!table_len(&tb)) {
#ifdef __EMSCRIPTEN__
        /* the browser starts empty: files arrive by drop */
#else
        fprintf(stderr, "tagplay-gui: nothing found\n");
        return 1;
#endif
    }
    if (parsed) cache_save(cp, &tb);
#ifndef __EMSCRIPTEN__
    stations_load(&tb);      /* the dial rides along, as in the TUI */
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    gui g;
    memset(&g, 0, sizeof g);
    g.win = SDL_CreateWindow("tagplay",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!g.win) {
        fprintf(stderr, "tagplay-gui: window: %s\n", SDL_GetError());
        return 1;
    }
    g.r = SDL_CreateRenderer(g.win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g.r) g.r = SDL_CreateRenderer(g.win, -1, 0);
    if (!g.r && st) {
        /* the dummy video driver backs no GPU renderer on some
         * platforms (macOS); the selftest draws into a software
         * renderer on a plain surface instead */
        g.st_surf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H,
                        32, SDL_PIXELFORMAT_ARGB8888);
        if (g.st_surf) g.r = SDL_CreateSoftwareRenderer(g.st_surf);
    }
    if (!g.r) {
        fprintf(stderr, "tagplay-gui: renderer: %s\n", SDL_GetError());
        return 1;
    }
    if (text_init(g.r, 17.0f)) {
        fprintf(stderr, "tagplay-gui: font atlas failed\n");
        return 1;
    }
    g.tb = &tb;
    g.pl = player_create(&tb);
    browser_init(&g.b, &tb, console_init(g.pl));
    viz_init(&g.v);
    g.instr_on = 1;
    g.sp_avg = 0.68;   /* EMA default: the old fixed feel */
    g.sp_nfrac = 0.22; /* boxcar default: N = 16 */
    for (int i = 0; i < 8; i++) g.fx_amt[i] = 0.5f;
    SDL_StartTextInput();

    int rc = 0;
    if (st) rc = selftest(&g);
    else {
#ifdef __EMSCRIPTEN__
        WG = &g;
        WTB = &tb;
        emscripten_set_main_loop(em_frame, 0, 1);
#else
        Uint32 last_tick = 0;
        while (!g.quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) handle(&g, &e);
            Uint32 now = SDL_GetTicks();
            if (now - last_tick > 200) {
                tick(&g);
                last_tick = now;
            }
            frame(&g);
            SDL_Delay(33);
        }
#endif
    }
    viz_shutdown(&g.v);
    if (g.overlay_art) SDL_DestroyTexture(g.overlay_art);
    player_destroy(g.pl);
    browser_free(&g.b);
    text_shutdown();
    SDL_DestroyRenderer(g.r);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
    table_free(&tb);
    return rc;
}
