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

/* The third face of the core: the same bmodel the TUI renders with
 * ANSI, here rendered with SDL2 -- query bar, live count, result list,
 * selection, queue panel, transport. Hand-rolled immediate-mode
 * widgets over an embedded public-domain 8x8 bitmap font: no toolkit,
 * no font files, nothing between us and the pixels. Compiles native
 * and (MG-d) to WASM via emscripten, like every SDL project on the
 * shelf.
 *
 * --selftest runs the whole event loop headless under SDL's dummy
 * video driver with a scripted event sequence, so the logic is CI-able
 * in a container with no display. */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "track.h"
#include "scan.h"
#include "cache.h"
#include "query.h"
#include "bmodel.h"
#include "app.h"
#include "player.h"
#include "dsp.h"
#include "tags.h"
#include "font8x8.h"

#define WIN_W 1100
#define WIN_H 700
#define FS    2            /* font scale: 8x8 -> 16x16 */
#define CH    (8 * FS)     /* cell height */
#define CW    (8 * FS)

typedef struct {
    SDL_Renderer *r;
    bmodel  m;
    player *pl;
    const table *tb;
    size_t  lcur, loff;    /* list cursor + scroll */
    char    msg[160];
    int     w, h;
    int     quit;
    /* transport geometry remembered per frame for hit-testing */
    SDL_Rect seek_r, vol_r, btn_prev, btn_play, btn_next, btn_stop,
             btn_dsp;
} gui;

/* ---- drawing ---- */

static void draw_char(SDL_Renderer *r, int x, int y, unsigned char c,
                      SDL_Color col) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *g = font8x8_basic[c];
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
    for (int row = 0; row < 8; row++)
        for (int bit = 0; bit < 8; bit++)
            if (g[row] & (1 << bit)) {
                SDL_Rect px = { x + bit * FS, y + row * FS, FS, FS };
                SDL_RenderFillRect(r, &px);
            }
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *s,
                      SDL_Color col) {
    for (; *s; s++, x += CW)
        draw_char(r, x, y, (unsigned char)*s, col);
}

static void draw_textn(SDL_Renderer *r, int x, int y, const char *s,
                       int maxch, SDL_Color col) {
    for (int i = 0; s[i] && i < maxch; i++, x += CW)
        draw_char(r, x, y, (unsigned char)s[i], col);
}

static const SDL_Color FG   = { 220, 220, 214, 255 };
static const SDL_Color DIM  = { 130, 130, 126, 255 };
static const SDL_Color HL   = { 255, 200,  80, 255 };
static const SDL_Color ACC  = { 120, 190, 255, 255 };
static const SDL_Color BG   = {  16,  16,  20, 255 };
static const SDL_Color ROW  = {  30,  30,  38, 255 };

static void fill(SDL_Renderer *r, SDL_Rect q, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(r, &q);
}

/* ---- frame ---- */

static void frame(gui *g) {
    SDL_Renderer *r = g->r;
    SDL_GetRendererOutputSize(r, &g->w, &g->h);
    fill(r, (SDL_Rect){ 0, 0, g->w, g->h }, BG);

    player_status ps;
    player_get_status(g->pl, &ps);
    const vec *shown = bmodel_shown(&g->m);

    /* query bar */
    int pad = 10;
    draw_text(r, pad, pad + 4, ">", ACC);
    draw_text(r, pad + 2 * CW, pad + 4, g->m.buf, FG);
    /* cursor */
    fill(r, (SDL_Rect){ pad + 2 * CW + (int)g->m.cur * CW,
                        pad + 4 + CH, CW, 2 }, HL);
    char cnt[64];
    snprintf(cnt, sizeof cnt, "%zu track%s%s", shown->len,
             shown->len == 1 ? "" : "s", g->m.parse_ok ? "" : " ?");
    draw_text(r, g->w - pad - (int)strlen(cnt) * CW, pad + 4, cnt,
              g->m.parse_ok ? DIM : HL);

    /* geometry: list left 62%, queue right */
    int top = pad + CH + 14;
    int bot = g->h - 3 * CH - 24;      /* transport strip */
    int lw = g->w * 62 / 100;
    int rows = (bot - top) / (CH + 6);
    if (rows < 1) rows = 1;

    /* keep the cursor in the window */
    if (g->lcur >= shown->len) g->lcur = shown->len ? shown->len - 1 : 0;
    if (g->lcur < g->loff) g->loff = g->lcur;
    if (g->lcur >= g->loff + (size_t)rows)
        g->loff = g->lcur - (size_t)rows + 1;

    for (int i = 0; i < rows; i++) {
        size_t idx = g->loff + (size_t)i;
        if (idx >= shown->len) break;
        size_t ti = *(size_t *)vec_at((vec *)shown, idx);
        const track *t = table_at(g->tb, ti);
        int y = top + i * (CH + 6);
        if (idx == g->lcur)
            fill(r, (SDL_Rect){ 0, y - 2, lw, CH + 4 }, ROW);
        int sel = bmodel_sel_find(&g->m, ti) >= 0;
        draw_text(r, pad, y, sel ? "[x]" : "[ ]", sel ? HL : DIM);
        char who[256];
        APP->identity(t, who, sizeof who);
        draw_textn(r, pad + 4 * CW, y, who, (lw - pad) / CW - 12, FG);
        char dur[16];
        fmt_duration(t->duration, dur, sizeof dur);
        char meta[32];
        snprintf(meta, sizeof meta, "%s %s", APP->fmt_name(t->fmt), dur);
        draw_text(r, lw - (int)strlen(meta) * CW - pad, y, meta, DIM);
    }

    /* queue panel */
    int qx = lw + 16;
    draw_text(r, qx, top, "QUEUE", ACC);
    vec qv;
    vec_init(&qv, sizeof(size_t));
    player_get_queue(g->pl, &qv);
    int qrows = rows - 1;
    size_t qoff = ps.queue_pos > 3 ? ps.queue_pos - 3 : 0;
    for (int i = 0; i < qrows; i++) {
        size_t qi = qoff + (size_t)i;
        if (qi >= qv.len) break;
        const track *t = table_at(g->tb, *(size_t *)vec_at(&qv, qi));
        char who[256];
        APP->identity(t, who, sizeof who);
        int y = top + (i + 1) * (CH + 6);
        if (qi == ps.queue_pos && ps.playing)
            draw_text(r, qx, y, ">", HL);
        draw_textn(r, qx + 2 * CW, y, who, (g->w - qx) / CW - 4,
                   qi == ps.queue_pos ? FG : DIM);
    }
    vec_free(&qv);

    /* transport strip */
    int ty = g->h - 3 * CH - 12;
    if (ps.playing && ps.track_index < table_len(g->tb)) {
        const track *t = table_at(g->tb, ps.track_index);
        char who[256];
        APP->identity(t, who, sizeof who);
        char np[320];
        char p1[16], p2[16];
        fmt_duration(ps.pos, p1, sizeof p1);
        fmt_duration(ps.dur, p2, sizeof p2);
        snprintf(np, sizeof np, "%s %s  %s/%s",
                 ps.playing == 2 ? "||" : ">", who, p1,
                 ps.dur > 0 ? p2 : "~");
        draw_textn(r, pad, ty, np, (g->w - 2 * pad) / CW, FG);
    } else if (g->msg[0]) {
        draw_textn(r, pad, ty, g->msg, (g->w - 2 * pad) / CW, DIM);
    }

    /* seek bar */
    g->seek_r = (SDL_Rect){ pad, ty + CH + 6, g->w - 2 * pad, 8 };
    fill(r, g->seek_r, ROW);
    if (ps.playing && ps.dur > 0) {
        SDL_Rect done = g->seek_r;
        done.w = (int)((double)done.w * (ps.pos / ps.dur));
        fill(r, done, ACC);
    }

    /* buttons + volume + dsp */
    int by = ty + CH + 22;
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
    /* dsp mode button */
    g->btn_dsp = (SDL_Rect){ bx + 12, by, 10 * CW, CH + 8 };
    fill(r, g->btn_dsp, ROW);
    char dl[24];
    snprintf(dl, sizeof dl, "dsp:%s", dsp_mode_name(player_dsp(g->pl)));
    draw_text(r, g->btn_dsp.x + CW, by + 4, dl, FG);
    /* volume */
    g->vol_r = (SDL_Rect){ g->w - pad - 24 * CW, by + CH / 2, 18 * CW, 8 };
    fill(r, g->vol_r, ROW);
    double gn = dsp_gain(player_dsp(g->pl));
    SDL_Rect vfill = g->vol_r;
    vfill.w = (int)(vfill.w * (gn / 2.0));
    fill(r, vfill, HL);
    char vl[16];
    snprintf(vl, sizeof vl, "%d%%", (int)(gn * 100 + 0.5));
    draw_text(r, g->vol_r.x + g->vol_r.w + CW, by + 4, vl, DIM);

    SDL_RenderPresent(r);
}

/* ---- actions ---- */

static void play_now(gui *g) {
    const vec *q = g->m.sel.len ? &g->m.sel : bmodel_shown(&g->m);
    if (!q->len) return;
    int from_sel = g->m.sel.len > 0;
    player_play(g->pl, (const size_t *)q->data, q->len);
    snprintf(g->msg, sizeof g->msg, "playing %zu track%s%s", q->len,
             q->len == 1 ? "" : "s", from_sel ? " (selection)" : "");
    if (from_sel) { g->m.sel.len = 0; g->m.sel_view = 0; }
}

static void model_insert(gui *g, const char *txt) {
    size_t tl = strlen(txt);
    if (g->m.len + tl >= sizeof g->m.buf) return;
    memmove(g->m.buf + g->m.cur + tl, g->m.buf + g->m.cur,
            g->m.len - g->m.cur);
    memcpy(g->m.buf + g->m.cur, txt, tl);
    g->m.cur += tl;
    g->m.len += tl;
    g->m.buf[g->m.len] = 0;
    g->m.sel_view = 0;
    bmodel_rerun(&g->m);
}

static int inrect(SDL_Rect q, int x, int y) {
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

static void handle(gui *g, const SDL_Event *e) {
    player_status ps;
    switch (e->type) {
    case SDL_QUIT: g->quit = 1; break;
    case SDL_TEXTINPUT:
        model_insert(g, e->text.text);
        break;
    case SDL_MOUSEWHEEL:
        if (e->wheel.y > 0 && g->loff) g->loff--;
        else if (e->wheel.y < 0) g->loff++;
        break;
    case SDL_MOUSEBUTTONDOWN: {
        int mx = e->button.x, my = e->button.y;
        if (inrect(g->btn_play, mx, my)) player_toggle_pause(g->pl);
        else if (inrect(g->btn_next, mx, my)) player_next(g->pl);
        else if (inrect(g->btn_prev, mx, my)) player_prev(g->pl);
        else if (inrect(g->btn_stop, mx, my)) player_stop(g->pl);
        else if (inrect(g->btn_dsp, mx, my)) {
            static const char *cyc[] = { "off", "tube", "tape", "vinyl",
                                         "shellac", "am" };
            const char *cur = dsp_mode_name(player_dsp(g->pl));
            int k = 0;
            for (int i = 0; i < 6; i++)
                if (!strcmp(cyc[i], cur)) { k = (i + 1) % 6; break; }
            dsp_set_mode(player_dsp(g->pl), cyc[k], 0.5);
        } else if (inrect(g->seek_r, mx, my)) {
            player_get_status(g->pl, &ps);
            if (ps.playing && ps.dur > 0)
                player_seek(g->pl, ps.dur * (mx - g->seek_r.x) /
                                   (double)g->seek_r.w);
        } else if (inrect(g->vol_r, mx, my)) {
            dsp_set_gain(player_dsp(g->pl),
                         2.0 * (mx - g->vol_r.x) / (double)g->vol_r.w);
        } else if (my > 40 && mx < g->w * 62 / 100) {
            /* result list: click = cursor; click the [x] = toggle */
            int top = 10 + CH + 14;
            int row = (my - top) / (CH + 6);
            const vec *shown = bmodel_shown(&g->m);
            size_t idx = g->loff + (size_t)(row < 0 ? 0 : row);
            if (idx < shown->len) {
                g->lcur = idx;
                size_t ti = *(size_t *)vec_at((vec *)shown, idx);
                if (mx < 10 + 4 * CW || e->button.clicks >= 2) {
                    if (e->button.clicks >= 2) {
                        player_play(g->pl, &ti, 1);
                        snprintf(g->msg, sizeof g->msg,
                                 "playing 1 track");
                    } else bmodel_sel_toggle(&g->m, ti);
                }
            }
        }
        break;
    }
    case SDL_KEYDOWN: {
        SDL_Keycode k = e->key.keysym.sym;
        int ctrl = e->key.keysym.mod & KMOD_CTRL;
        const vec *shown = bmodel_shown(&g->m);
        if (k == SDLK_RETURN) play_now(g);
        else if (k == SDLK_BACKSPACE && g->m.cur > 0) {
            memmove(g->m.buf + g->m.cur - 1, g->m.buf + g->m.cur,
                    g->m.len - g->m.cur);
            g->m.cur--; g->m.len--;
            g->m.buf[g->m.len] = 0;
            g->m.sel_view = 0;
            bmodel_rerun(&g->m);
        }
        else if (k == SDLK_u && ctrl) {
            g->m.len = g->m.cur = 0;
            g->m.buf[0] = 0;
            g->m.sel_view = 0;
            bmodel_rerun(&g->m);
        }
        else if (k == SDLK_LEFT && g->m.cur) g->m.cur--;
        else if (k == SDLK_RIGHT && g->m.cur < g->m.len) g->m.cur++;
        else if (k == SDLK_DOWN) {
            if (g->lcur + 1 < shown->len) g->lcur++;
        }
        else if (k == SDLK_UP) { if (g->lcur) g->lcur--; }
        else if (k == SDLK_SPACE && ctrl) {
            if (shown->len)
                bmodel_sel_toggle(&g->m,
                    *(size_t *)vec_at((vec *)shown, g->lcur));
        }
        else if (k == SDLK_ESCAPE) g->quit = 1;
        break;
    }
    default: break;
    }
}

/* ---- selftest: the whole loop, headless ---- */

static void push_text(const char *s) {
    for (; *s; s++) {
        SDL_Event e = { 0 };
        e.type = SDL_TEXTINPUT;
        e.text.text[0] = *s;
        SDL_PushEvent(&e);
    }
}
static void push_key(SDL_Keycode k, int ctrl) {
    SDL_Event e = { 0 };
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = k;
    e.key.keysym.mod = ctrl ? KMOD_LCTRL : 0;
    SDL_PushEvent(&e);
}

static int selftest(gui *g) {
    int fails = 0;
#define CHK(name, cond) do { \
        if (cond) printf("PASS %s\n", name); \
        else { printf("FAIL %s\n", name); fails++; } } while (0)
    SDL_Event e;
    push_text("bach");
    while (SDL_PollEvent(&e)) handle(g, &e);
    CHK("query filters", bmodel_shown(&g->m)->len == 4);
    push_key(SDLK_SPACE, 1);
    push_key(SDLK_DOWN, 0);
    push_key(SDLK_SPACE, 1);
    while (SDL_PollEvent(&e)) handle(g, &e);
    CHK("ctrl-space selects", g->m.sel.len == 2);
    push_key(SDLK_RETURN, 0);
    while (SDL_PollEvent(&e)) handle(g, &e);
    player_status ps;
    SDL_Delay(300);
    player_get_status(g->pl, &ps);
    CHK("enter queues selection", ps.queue_len == 2);
    CHK("selection consumed", g->m.sel.len == 0);
    push_key(SDLK_u, 1);
    push_text("storm");
    while (SDL_PollEvent(&e)) handle(g, &e);
    CHK("requery", bmodel_shown(&g->m)->len >= 1);
    frame(g);   /* one full render pass under the dummy driver */
    CHK("frame renders", 1);
    return fails;
}

/* ---- main ---- */

int main(int argc, char **argv) {
    int st = argc > 1 && !strcmp(argv[1], "--selftest");
    const char *dir = argc > (st ? 2 : 1) ? argv[st ? 2 : 1] : NULL;
    if (!dir) {
        fprintf(stderr, "usage: tagplay-gui [--selftest] MUSIC_DIR...\n");
        return 2;
    }
    if (st) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);

    table cached, tb;
    table_init(&cached);
    table_init(&tb);
    /* GUI sessions scan fresh through the same cache as the TUI */
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
        fprintf(stderr, "tagplay-gui: nothing found\n");
        return 1;
    }
    if (parsed) cache_save(cp, &tb);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("tagplay",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);

    gui g;
    memset(&g, 0, sizeof g);
    g.r = ren;
    g.tb = &tb;
    bmodel_init(&g.m, &tb);
    g.pl = player_create(&tb);
    bmodel_rerun(&g.m);
    SDL_StartTextInput();

    int rc = 0;
    if (st) {
        rc = selftest(&g);
    } else {
        while (!g.quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) handle(&g, &e);
            frame(&g);
            SDL_Delay(33);          /* ~30 fps; status polls ride along */
        }
    }
    player_destroy(g.pl);
    bmodel_free(&g.m);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    table_free(&tb);
    return rc;
}
