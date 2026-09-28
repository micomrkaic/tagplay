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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "font8x8.h"

#define WIN_W 1100
#define WIN_H 720
#define FS    2
#define CH    (8 * FS)
#define CW    (8 * FS)

typedef struct {
    SDL_Renderer *r;
    SDL_Window   *win;
    browser  b;              /* THE state: shared with the TUI's loop */
    player  *pl;
    const table *tb;
    int      w, h, quit;
    int      overlay;        /* BREQ_* rendered until any key, or 0 */
    size_t   overlay_ti;
    SDL_Texture *overlay_art;
    int      art_w, art_h;
    SDL_Rect seek_r, vol_r, btn_prev, btn_play, btn_next, btn_stop;
    int      list_top, row_h;
} gui;

static void draw_char(SDL_Renderer *r, int x, int y, unsigned char c,
                      SDL_Color col) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *g = (const unsigned char *)font8x8_basic[c];
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
    for (int row = 0; row < 8; row++)
        for (int bit = 0; bit < 8; bit++)
            if (g[row] & (1 << bit)) {
                SDL_Rect px = { x + bit * FS, y + row * FS, FS, FS };
                SDL_RenderFillRect(r, &px);
            }
}
static void draw_textn(SDL_Renderer *r, int x, int y, const char *s,
                       int maxch, SDL_Color col) {
    int n = 0;
    while (*s && n < maxch) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0xC0) {                 /* UTF-8 lead: one cell */
            while ((s[1] & 0xC0) == 0x80) s++;
            c = '~';
        } else if (c >= 0x80) { s++; continue; }
        draw_char(r, x, y, c, col);
        s++; n++; x += CW;
    }
}
static void draw_text(SDL_Renderer *r, int x, int y, const char *s,
                      SDL_Color col) {
    draw_textn(r, x, y, s, 4096, col);
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

static void frame(gui *g) {
    SDL_Renderer *r = g->r;
    SDL_GetRendererOutputSize(r, &g->w, &g->h);
    fill(r, (SDL_Rect){ 0, 0, g->w, g->h }, BG);
    int pad = 10;

    player_status ps;
    player_get_status(g->pl, &ps);
    const vec *shown = bmodel_shown(&g->b.m);

    if (g->overlay == BREQ_HELP || g->overlay == BREQ_DETAIL) {
        int y = pad;
        if (g->overlay == BREQ_HELP) {
            const char *p = BROWSER_HELP;
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
    int bot = g->h - 4 * CH - 26;
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
        if (SDL_PointInRect(&p, &g->btn_play))
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

/* ---- main ---- */

int main(int argc, char **argv) {
    int st = argc > 1 && !strcmp(argv[1], "--selftest");
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
        fprintf(stderr, "tagplay-gui: nothing found\n");
        return 1;
    }
    if (parsed) cache_save(cp, &tb);
    stations_load(&tb);      /* the dial rides along, as in the TUI */

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    gui g;
    memset(&g, 0, sizeof g);
    g.win = SDL_CreateWindow("tagplay",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    g.r = SDL_CreateRenderer(g.win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g.r) g.r = SDL_CreateRenderer(g.win, -1, 0);
    g.tb = &tb;
    g.pl = player_create(&tb);
    browser_init(&g.b, &tb, console_init(g.pl));
    SDL_StartTextInput();

    int rc = 0;
    if (st) rc = selftest(&g);
    else {
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
    }
    if (g.overlay_art) SDL_DestroyTexture(g.overlay_art);
    player_destroy(g.pl);
    browser_free(&g.b);
    SDL_DestroyRenderer(g.r);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
    table_free(&tb);
    return rc;
}
