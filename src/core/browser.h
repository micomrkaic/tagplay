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
#include <stddef.h>
#include <stdint.h>

/* symbolic key codes delivered to app key hooks (printable keys are
 * their ASCII values; escape sequences are pre-parsed by the browser) */
enum {
    K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_SRIGHT, K_SLEFT,
    K_PGUP, K_PGDN, K_HOME, K_END, K_DEL, K_ESC
};

typedef struct browser {
    char   buf[1024];
    size_t len, cur;         /* query content length, cursor */
    vec    match;            /* vec of size_t, current result */
    vec    last_good;        /* last successfully parsed result */
    int    parse_ok;
    char   sortspec[128];
    const table *tb;
    void  *ui;               /* the app's UI context (opaque to core) */
    vec    sel;              /* size_t table indices, insertion order */
    int    focus;            /* 0 = query, 1 = list, 2 = alt (app) view */
    size_t lcur, loff;       /* list cursor + scroll offset */
    char   msg[160];         /* transient feedback line */
    vec    qview;            /* app-filled snapshot for the alt view */
    size_t qcur, qoff;       /* alt view cursor + scroll */
    char   group[32];        /* tag key to group by; "" = off */
    int    sel_view;         /* :sel mode: match mirrors the selection */
    unsigned cols_on;        /* COL_* bitmask */
    /* partial-refresh bookkeeping */
    int    vu_row;           /* 1-based terminal row of the status region */
    int    sig_valid;
    int    sig_focus;
    uint64_t sig_app;        /* app layout signature at last full redraw */
    size_t sig_rows;
    int    sig_trows, sig_tcols;
} browser;

void browser_run(const table *tb, void *ui);

/* terminal helpers shared with app view/status code */
int  term_rows(void);
int  term_cols(void);
int  u8clip(const char *s, int bytes);
long now_ms(void);
void browser_raw_off(void);
void browser_raw_on(void);

#endif
