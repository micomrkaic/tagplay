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

/* The shared interactive browser: query line, result list, selection,
 * grouping, the alternate app view, and the surgical-refresh terminal
 * machinery. App behavior arrives via the tp_app hooks (app.h). */

#ifndef TP_BROWSER_H
#define TP_BROWSER_H

#include "track.h"
#include "util.h"
#include "bmodel.h"
#include <stddef.h>
#include <stdint.h>

/* symbolic key codes delivered to app key hooks (printable keys are
 * their ASCII values; escape sequences are pre-parsed by the browser) */
enum {
    K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_SRIGHT, K_SLEFT,
    K_PGUP, K_PGDN, K_HOME, K_END, K_DEL, K_ESC
};

/* Requests: things a key handler wants SHOWN. The controller sets
 * them; each face (TUI, GUI) services them its own way after every
 * browser_key() call. */
enum {
    BREQ_NONE = 0, BREQ_DETAIL, BREQ_ART, BREQ_HELP, BREQ_STATS,
    BREQ_LS, BREQ_ENTER_VIEW, BREQ_QUIT
};

typedef struct browser {
    bmodel m;                /* the browse model (core/bmodel.h) */
    void  *ui;               /* the app's UI context (opaque to core) */
    int    focus;            /* 0 = query, 1 = list, 2 = alt (app) view */
    size_t lcur, loff;       /* list cursor + scroll offset */
    char   msg[160];         /* transient feedback line */
    vec    qview;            /* app-filled snapshot for the alt view */
    size_t qcur, qoff;       /* alt view cursor + scroll */
    unsigned cols_on;        /* COL_* bitmask */
    /* partial-refresh bookkeeping */
    int    vu_row;           /* 1-based terminal row of the status region */
    int    sig_valid;
    int    sig_focus;
    uint64_t sig_app;        /* app layout signature at last full redraw */
    size_t sig_rows;
    int    sig_trows, sig_tcols;
    /* controller -> face requests (serviced and cleared by the face) */
    int    req;              /* BREQ_* */
    size_t req_ti;           /* table index for DETAIL/ART */
    const vec *req_items;    /* item vec for ENTER_VIEW */
    int    req_from_sel;
} browser;

void browser_run(const table *tb, void *ui);

/* The face-independent controller: initialize state, feed symbolic
 * keys (K_* or ASCII), service the request left in b->req. Every
 * face -- the terminal loop and the SDL GUI -- drives this one
 * dispatcher, so keys, commands, and view semantics cannot diverge. */
void browser_init(browser *b, const table *tb, void *ui);
void browser_free(browser *b);
void browser_key(browser *b, int key);
extern const char *const BROWSER_HELP;

/* terminal helpers shared with app view/status code */
int  term_rows(void);
int  term_cols(void);
int  u8clip(const char *s, int bytes);
long now_ms(void);
void browser_raw_off(void);
void browser_raw_on(void);

#endif
