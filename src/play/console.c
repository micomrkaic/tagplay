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

/* The player's console: everything the shared browser asks the app to
 * do -- transport keys, the queue (alt) view, the VU/marquee status
 * region, play commands, and radio station management. */

#include "browser.h"
#include "app.h"
#include "player.h"
#include "dsp.h"
#include "tags.h"
#include "console.h"
#include <sys/select.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
    player  *pl;
    double   mute_saved;      /* pre-mute gain; 0 = not muted */
    unsigned seen_note_seq;
} console_ui;

static console_ui CTX;
#define CUI(b) ((console_ui *)(b)->ui)

void *console_init(player *pl) {
    CTX.pl = pl;
    return &CTX;
}

static void console_identity(const track *t, char *out, size_t sz);

static void console_identity(const track *t, char *out, size_t sz) {
    const char *artist   = track_first_tag(t, "ARTIST");
    const char *title    = track_first_tag(t, "TITLE");
    const char *composer = track_first_tag(t, "COMPOSER");
    if (composer && artist && !str_ieq(composer, artist) &&
        !str_icasestr(artist, composer))
        snprintf(out, sz, "%s — %s (%s)", composer,
                 title ? title : "?", artist);
    else
        snprintf(out, sz, "%s — %s", artist ? artist : "?",
                 title ? title : "?");
}

/* (5) 90s CD-player marquee: writes `s` into a width-limited field,
 * scrolling horizontally when it doesn't fit */
static void marquee(const char *s, int width) {
    int len = (int)strlen(s);
    if (len <= width) { printf("%s%*s", s, width - len, ""); return; }
    char loop[1024];
    snprintf(loop, sizeof loop, "%s  *  ", s);
    int llen = (int)strlen(loop);
    /* rotate by characters, not bytes: precompute UTF-8 char starts */
    int starts[1024], nch = 0;
    for (int i = 0; i < llen; i++)
        if (((unsigned char)loop[i] & 0xC0) != 0x80) starts[nch++] = i;
    if (!nch) return;
    int off = (int)((now_ms() / 300) % nch);
    char field[1024];
    int w = 0, k = off;
    while (w < width) {
        int b = starts[k];
        int e = (k + 1 < nch) ? starts[k + 1] : llen;
        int cl = e - b;
        if (w + cl > (int)sizeof field - 1) break;
        if (w + cl > width) break;      /* don't start a char we can't fit */
        memcpy(field + w, loop + b, (size_t)cl);
        w += cl;
        k = (k + 1) % nch;
    }
    field[w] = 0;
    /* pad with spaces to hold the field width steady */
    printf("%s%*s", field, width - w > 0 ? width - w : 0, "");
}

static void vu_line(double l, double r, int cols) {
    int bw = (cols - 12) / 2;          /* "L[..] R[..]" chrome */
    if (bw < 8) bw = 8;
    if (bw > 40) bw = 40;
    const double floor_db = -42.0;
    double v[2] = { l, r };
    printf("  ");
    for (int c = 0; c < 2; c++) {
        double db = v[c] > 1e-6 ? 20.0 * log10(v[c]) : floor_db;
        if (db > 0) db = 0;
        if (db < floor_db) db = floor_db;
        int fill = (int)((db - floor_db) / -floor_db * bw + 0.5);
        printf("%c[", c ? 'R' : 'L');
        for (int i = 0; i < bw; i++)
            putchar(i < fill ? (i >= bw - bw / 5 ? '!' : '#') : '-');
        printf("] ");
    }
}

static void stations_path(char *out, size_t sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, sz, "%s/tagplay/stations", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, sz, "%s/.config/tagplay/stations", home ? home : ".");
    }
}
static void station_add_track(table *tb, const char *name, const char *url) {
    track *t = table_add(tb);
    t->path = xstrdup(url);
    t->fmt = FMT_RADIO;
    track_add_tag(t, "TITLE", name);
    track_add_tag(t, "ARTIST", "Radio station");
    track_add_tag(t, "ALBUM", "Internet Radio");
}
static const char *SEED_STATIONS =
    "# tagplay stations: URL <TAB> Name. Lines starting with # ignored.\n"
    "# Seeded on first run; edit freely, or :radio add / :radio rm in-app.\n"
    "http://stream.srg-ssr.ch/m/rsc_de/mp3_128\tRadio Swiss Classic\n"
    "http://stream.srg-ssr.ch/m/rsj/mp3_128\tRadio Swiss Jazz\n"
    "https://icecast.radiofrance.fr/francemusique-midfi.mp3\tFrance Musique\n"
    "https://icecast.radiofrance.fr/fip-midfi.mp3\tFIP (Radio France)\n"
    "https://stream.wqxr.org/wqxr\tWQXR New York Classical\n"
    "http://stream.radioparadise.com/mp3-192\tRadio Paradise (Main Mix)\n"
    "https://ice1.somafm.com/groovesalad-128-mp3\tSomaFM Groove Salad\n"
    "https://npr-ice.streamguys1.com/live.mp3\tNPR Live (News)\n"
    "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service\tBBC World Service (News)\n"
    "https://mp3.rtvslo.si/ars\tRadio Slovenija ARS\n"
    "https://mp3.rtvslo.si/val202\tVal 202\n"
    "https://mp3.rtvslo.si/prvi\tRadio Slovenija Prvi (News)\n"
    "http://stream0.wfmu.org/freeform-128k.mp3\tWFMU Freeform\n"
    "http://kruljo.radiostudent.si:8000/ehiq\tRadio \xc5\xa0tudent Ljubljana\n"
    "https://wwoz-sc.streamguys1.com/wwoz-hi.mp3\tWWOZ New Orleans\n"
    "https://icecast.radiofrance.fr/fipworld-midfi.mp3\tFIP World\n"
    "https://icecast.radiofrance.fr/fipgroove-midfi.mp3\tFIP Groove\n"
    "https://icecast.radiofrance.fr/fiprock-midfi.mp3\tFIP Rock\n"
    "https://icecast.radiofrance.fr/fipjazz-midfi.mp3\tFIP Jazz\n"
    "https://knkx-live-a.edge.audiocdn.com/6285_128k\tJazz24\n"
    "https://stream.wfmt.com/main-mp3\tWFMT Chicago Classical\n"
    "https://ice5.somafm.com/secretagent-128-mp3\tSomaFM Secret Agent\n"
    "https://ice5.somafm.com/dronezone-256-mp3\tSomaFM Drone Zone\n"
    "https://ice5.somafm.com/u80s-256-mp3\tSomaFM Underground 80s\n"
    "https://ice5.somafm.com/illstreet-128-mp3\tSomaFM Illinois Street Lounge\n"
    "https://stream.radioparadise.com/world-etc-192\tRadio Paradise World-Etc\n"
    "https://stream.srg-ssr.ch/srgssr/rsp/mp3/128\tRadio Swiss Pop\n"
    "http://ice24.securenetsystems.net/WAMU\tBluegrass Country (WAMU)\n"
    "https://kexp.streamguys1.com/kexp160.aac\tKEXP Seattle\n"
    "https://weta.streamguys1.com/wetaclassical-icy\tWETA Classical\n"
    "https://weta.streamguys1.com/wetavirtuoso-icy\tWETA Virtuoso\n"
    "https://weta.streamguys1.com/wetavivalavoce-icy\tWETA VivaLaVoce\n";

size_t stations_load(table *tb) {   /* also called from main.c */
    char p[4096];
    stations_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) {
        /* first run: seed a starter set (public-service news + curated
         * music). Only ever written when the file does not exist. */
        util_mkdirs_for(p);
        FILE *w = fopen(p, "w");
        if (!w) return 0;
        fputs(SEED_STATIONS, w);
        fclose(w);
        f = fopen(p, "r");
        if (!f) return 0;
    }
    char line[2048];
    size_t n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        station_add_track(tb, tab + 1, line);   /* url \t name */
        n++;
    }
    fclose(f);
    return n;
}
static int station_persist(const char *url, const char *name, int remove_by_name) {
    char p[4096];
    stations_path(p, sizeof p);
    util_mkdirs_for(p);
    if (!remove_by_name) {
        FILE *f = fopen(p, "a");
        if (!f) return -1;
        fprintf(f, "%s\t%s\n", url, name);
        fclose(f);
        return 0;
    }
    /* rewrite without the named station */
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *o = fopen(tmp, "w");
    if (!o) { fclose(f); return -1; }
    char line[2048];
    int removed = 0;
    while (fgets(line, sizeof line, f)) {
        char probe[2048];
        snprintf(probe, sizeof probe, "%s", line);
        probe[strcspn(probe, "\r\n")] = 0;
        char *tab = strchr(probe, '\t');
        if (tab && str_ieq(tab + 1, name)) { removed = 1; continue; }
        fputs(line, o);
    }
    fclose(f);
    fclose(o);
    rename(tmp, p);
    return removed ? 0 : -1;
}

static void progress_line(const player_status *ps, int cols) {
    int bw = cols - 16;
    if (bw > 72) bw = 72;
    if (bw < 10) bw = 10;
    if (ps->dur > 0) {
        double frac = ps->pos / ps->dur;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int head = (int)(frac * (bw - 1) + 0.5);
        printf("  [");
        for (int i = 0; i < bw; i++)
            putchar(i < head ? '=' : (i == head ? '>' : '-'));
        printf("]");
    } else {
        printf("  [ live stream ]");
    }
}
static void status_region(browser *st, const player_status *ps, int cols) {
    if (ps->playing) {
        if (ps->playing == 1) {
            vu_line(ps->vu_l, ps->vu_r, cols);
            printf("\x1b[K\r\n");
        } else printf("\x1b[K\r\n");
        progress_line(ps, cols);
        printf("\x1b[K\r\n");
        const track *ct = table_at(st->tb, ps->track_index);
        const char *ttl = track_first_tag(ct, "TITLE");
        char cp[16], cd[16], mt[512];
        fmt_duration(ps->pos, cp, sizeof cp);
        fmt_duration(ps->dur, cd, sizeof cd);
        if (ps->stream_title[0])
            snprintf(mt, sizeof mt, "%s \xe2\x80\xa2 %s",
                     ttl ? ttl : "radio", ps->stream_title);
        else
            console_identity(ct, mt, sizeof mt);
        int mw = cols - 46;
        if (mw < 12) mw = 12;
        char clock[40];
        if (ps->dur > 0) snprintf(clock, sizeof clock, "%s/%s", cp, cd);
        else             snprintf(clock, sizeof clock, "%s \xe2\x88\x9e", cp);
        char tailtxt[256];
        snprintf(tailtxt, sizeof tailtxt,
                 " %s [%zu/%zu] %dk dsp:%s vol:%d%%%s",
                 clock, ps->queue_pos + 1, ps->queue_len, ps->rate / 1000,
                 dsp_mode_name(player_dsp(CUI(st)->pl)),
                 (int)(dsp_gain(player_dsp(CUI(st)->pl)) * 100 + 0.5),
                 ps->null_output ? " (NO AUDIO)" : "");
        /* budget the line so glyph+marquee+tail can never exceed cols-1
         * (a wrap here would scroll the terminal and unmoor the layout) */
        int tw = (int)strlen(tailtxt);
        int mw2 = cols - 1 - 2 - tw;      /* "▶ " is 2 columns */
        if (mw2 < 8) mw2 = 8;
        if (mw2 < mw) mw = mw2;
        printf("%s ", ps->playing == 2 ? "⏸" : "▶");
        marquee(mt, mw);
        printf("%.*s\x1b[K", u8clip(tailtxt, cols - 3 - mw), tailtxt);
    } else {
        printf("\x1b[K\r\n\x1b[K\r\nstopped\x1b[K");
    }
}

static void redraw_queue(browser *st) {
    player_status ps;
    player_get_status(CUI(st)->pl, &ps);
    player_get_queue(CUI(st)->pl, &st->qview);
    if (st->qview.len == 0) { st->focus = 0; return; } /* nothing queued */

    int rows = term_rows();
    int cols = term_cols();
    int chrome = 7 + (st->msg[0] ? 2 : 0);
    int avail = rows - chrome;
    if ((size_t)avail < st->qview.len) avail--;   /* "… more" line */
    if (avail < 3) avail = 3;

    printf("\x1b[H");
    {
        char hdr[256];
        snprintf(hdr, sizeof hdr,
            "tagplay — QUEUE   Space pause · \xe2\x86\x90\xe2\x86\x92 10s · <> 60s · r restart · s stop · J/K reorder · t tags · a art");
        printf("%.*s\x1b[K\r\n\x1b[K\r\n", u8clip(hdr, cols - 1), hdr);
    }

    if (st->qcur >= st->qview.len) st->qcur = st->qview.len - 1;
    if (st->qoff > st->qcur) st->qoff = st->qcur;
    if (st->qcur >= st->qoff + (size_t)avail)
        st->qoff = st->qcur - (size_t)avail + 1;
    size_t n = st->qview.len - st->qoff < (size_t)avail
             ? st->qview.len - st->qoff : (size_t)avail;
    for (size_t i = 0; i < n; i++) {
        size_t row = st->qoff + i;
        size_t ti = *(size_t *)vec_at(&st->qview, row);
        const track *t = table_at(st->tb, ti);
        char who[512], dur[16];
        console_identity(t, who, sizeof who);
        fmt_duration(t->duration, dur, sizeof dur);
        int now = (ps.playing && row == ps.queue_pos);
        int hot = (row == st->qcur);
        char line[1024];
        snprintf(line, sizeof line, "%s%4zu  %s  %s",
                 now ? "▶ " : "  ", row + 1, who, dur);
        printf("%s%.*s\x1b[0m\x1b[K\r\n", hot ? "\x1b[7m" : "",
               u8clip(line, cols - 2), line);
    }
    if (st->qoff + n < st->qview.len)
        printf("      … %zu more\x1b[K\r\n", st->qview.len - st->qoff - n);
    if (st->msg[0]) printf("\r\n  %.*s\x1b[K\r\n", u8clip(st->msg, cols - 3), st->msg);
    printf("\x1b[K\r\n");

    /* totals: whole queue, and remaining from the playing position */
    double tot = 0, left = 0;
    for (size_t i = 0; i < st->qview.len; i++) {
        double d = table_at(st->tb,
                            *(size_t *)vec_at(&st->qview, i))->duration;
        tot += d;
        if (ps.playing && i > ps.queue_pos) left += d;
    }
    if (ps.playing) left += ps.dur > ps.pos ? ps.dur - ps.pos : 0;
    char bt[32], bl[32];
    fmt_duration_long(tot, bt, sizeof bt);
    fmt_duration_long(left, bl, sizeof bl);
    printf("queue: %zu · %s", st->qview.len, bt);
    if (ps.playing) printf(" · %s left", bl);
    if (st->sel.len) printf("   \x1b[1mselected: %zu\x1b[0m", st->sel.len);
    printf("\x1b[K\r\n");

    /* rows above the VU line: header+blank(2) + list(n) + overflow +
     * msg(2) + blank(1) + totals(1) */
    st->vu_row = 2 + (int)n + (st->qoff + n < st->qview.len ? 1 : 0)
               + (st->msg[0] ? 2 : 0) + 1 + 1 + 1;
    status_region(st, &ps, cols);
    printf("\x1b[0J"); /* clear anything below */
    st->sig_valid = 1;
    st->sig_trows = rows;
    st->sig_tcols = cols;
    st->sig_focus = 2;
    st->sig_app = console_sig(st->ui);
    st->sig_rows = st->qview.len;
    fflush(stdout);
}

/* VU line + progress line + status line (3 rows always; the last line
 * carries no trailing newline) */


/* ---- tp_app UI hook implementations ---- */

void console_hook_identity(const track *t, char *out, size_t sz) {
    console_identity(t, out, sz);
}

void console_detail_header(const track *t, char *out, size_t sz) {
    if (t->fmt == FMT_RADIO) {
        snprintf(out, sz, "%s stream", fmt_name(t->fmt));
    } else {
        char dur[16];
        fmt_duration(t->duration, dur, sizeof dur);
        snprintf(out, sz, "%s \xc2\xb7 %u Hz \xc2\xb7 %u ch \xc2\xb7 %s",
                 fmt_name(t->fmt), t->sample_rate, t->channels, dur);
    }
}

int console_pulse(void *ui) {
    player_status ps;
    player_get_status(((console_ui *)ui)->pl, &ps);
    return ps.playing == 1 ? 1 : (ps.playing ? 2 : 0);
}

uint64_t console_sig(void *ui) {
    player_status ps;
    player_get_status(((console_ui *)ui)->pl, &ps);
    return (uint64_t)ps.playing
         | ((uint64_t)(ps.queue_pos & 0xFFFF) << 4)
         | ((uint64_t)(ps.track_index & 0xFFFFF) << 20)
         | ((uint64_t)(ps.queue_len & 0xFFFF) << 44);
}

int console_poll_msg(void *ui, char *out, size_t sz) {
    console_ui *u = ui;
    player_status now;
    player_get_status(u->pl, &now);
    if (now.note_seq != u->seen_note_seq && now.note[0]) {
        u->seen_note_seq = now.note_seq;
        snprintf(out, sz, "%s", now.note);
        return 1;
    }
    return 0;
}

int console_status_rows(void *ui) {
    player_status ps;
    player_get_status(((console_ui *)ui)->pl, &ps);
    return ps.playing ? 3 : 0;
}

void console_status(void *ui, struct browser *b, int cols) {
    (void)ui;
    player_status ps;
    player_get_status(CUI(b)->pl, &ps);
    status_region(b, &ps, cols);
}

size_t console_alt_len(void *ui) {
    player_status ps;
    player_get_status(((console_ui *)ui)->pl, &ps);
    return ps.queue_len;
}

size_t console_alt_home(void *ui) {
    player_status ps;
    player_get_status(((console_ui *)ui)->pl, &ps);
    return ps.queue_pos;
}

void console_alt_view(void *ui, struct browser *b) {
    (void)ui;
    redraw_queue(b);
}

int console_alt_snapshot(void *ui, struct browser *b) {
    player_get_queue(((console_ui *)ui)->pl, &b->qview);
    return 1;
}

int console_alt_key(void *ui, struct browser *b, int c) {
    console_ui *u = ui;
    player_status qps;
    player_get_status(u->pl, &qps);
    if (c == ' ') {                 /* Space: pause/resume */
        player_toggle_pause(u->pl);
    } else if (c == '>' || c == K_SRIGHT) {  /* fast-forward 60 s */
        double tp = qps.pos + 60;
        if (qps.dur > 0 && tp > qps.dur - 0.5) tp = qps.dur - 0.5;
        player_seek(u->pl, tp);
    } else if (c == '<' || c == K_SLEFT) {   /* rewind 60 s */
        player_seek(u->pl, qps.pos > 60 ? qps.pos - 60 : 0);
    } else if (c == K_RIGHT) {      /* right: seek +10s */
        double tp = qps.pos + 10;
        if (qps.dur > 0 && tp > qps.dur - 0.5) tp = qps.dur - 0.5;
        player_seek(u->pl, tp);
    } else if (c == K_LEFT) {       /* left: seek -10s */
        player_seek(u->pl, qps.pos > 10 ? qps.pos - 10 : 0);
    } else if (c == 'r') {          /* restart current track */
        player_seek(u->pl, 0);
    } else if (c == 's') {          /* stop (queue kept) */
        player_stop(u->pl);
    } else if (c == 'J') {          /* move cursored track down */
        if (b->qcur + 1 < b->qview.len) {
            player_move(u->pl, b->qcur, b->qcur + 1);
            b->qcur++;
        }
    } else if (c == 'K') {          /* move cursored track up */
        if (b->qcur > 0) {
            player_move(u->pl, b->qcur, b->qcur - 1);
            b->qcur--;
        }
    } else if (c == '+' || c == '=' || c == '-') {
        double g = dsp_gain(player_dsp(u->pl)) + (c == '-' ? -0.05 : 0.05);
        dsp_set_gain(player_dsp(u->pl), g);
        u->mute_saved = 0;
        snprintf(b->msg, sizeof b->msg, "vol: %d%%",
                 (int)(dsp_gain(player_dsp(u->pl)) * 100 + 0.5));
    } else if (c == 'm') {
        if (u->mute_saved > 0) {
            dsp_set_gain(player_dsp(u->pl), u->mute_saved);
            u->mute_saved = 0;
        } else {
            u->mute_saved = dsp_gain(player_dsp(u->pl));
            if (u->mute_saved <= 0) u->mute_saved = 1.0;
            dsp_set_gain(player_dsp(u->pl), 0);
        }
    } else if (c == '\r' || c == '\n') {
        player_jump(u->pl, b->qcur); /* play the cursored track */
    } else return 0;
    return 1;
}

int console_global_key(void *ui, struct browser *b, int c) {
    console_ui *u = ui;
    if (c == 16) { player_toggle_pause(u->pl); return 1; }   /* ctrl-p */
    if (c == 14) { player_next(u->pl); return 1; }           /* ctrl-n */
    if (c == 2)  { player_prev(u->pl); return 1; }           /* ctrl-b */
    if (c == '+' || c == '=' || c == '-') {
        double g = dsp_gain(player_dsp(u->pl)) + (c == '-' ? -0.05 : 0.05);
        dsp_set_gain(player_dsp(u->pl), g);
        u->mute_saved = 0;
        snprintf(b->msg, sizeof b->msg, "vol: %d%%",
                 (int)(dsp_gain(player_dsp(u->pl)) * 100 + 0.5));
        return 1;
    }
    if (c == 'm') {
        if (u->mute_saved > 0) {
            dsp_set_gain(player_dsp(u->pl), u->mute_saved);
            snprintf(b->msg, sizeof b->msg, "unmuted: %d%%",
                     (int)(dsp_gain(player_dsp(u->pl)) * 100 + 0.5));
            u->mute_saved = 0;
        } else {
            u->mute_saved = dsp_gain(player_dsp(u->pl));
            if (u->mute_saved <= 0) u->mute_saved = 1.0;
            dsp_set_gain(player_dsp(u->pl), 0);
            snprintf(b->msg, sizeof b->msg, "muted");
        }
        return 1;
    }
    return 0;
}

void console_on_enter(void *ui, struct browser *b, const vec *q, int from_sel) {
    console_ui *u = ui;
    player_play(u->pl, (const size_t *)q->data, q->len);
    snprintf(b->msg, sizeof b->msg, "playing %zu track%s%s",
             q->len, q->len == 1 ? "" : "s",
             from_sel ? " (selection)" : "");
    b->focus = 2;       /* land in the queue view */
    b->qcur = 0;
}

int console_command(void *ui, struct browser *b, const char *cmd) {
    console_ui *u = ui;
    if (!strcmp(cmd, "p") || !strcmp(cmd, "pause")) { player_toggle_pause(u->pl); return 1; }
    if (!strcmp(cmd, "n") || !strcmp(cmd, "next")) { player_next(u->pl); return 1; }
    if (!strcmp(cmd, "b") || !strcmp(cmd, "prev")) { player_prev(u->pl); return 1; }
    if (!strcmp(cmd, "stop")) { player_stop(u->pl); return 1; }
    if (!strncmp(cmd, "seek ", 5)) {
        double target;
        long mm, ss2;
        if (sscanf(cmd + 5, "%ld:%ld", &mm, &ss2) == 2) target = (double)(mm * 60 + ss2);
        else target = atof(cmd + 5);
        player_seek(u->pl, target);
        return 1;
    }
    if (!strncmp(cmd, "vol", 3)) {
        const char *a = cmd + 3;
        while (*a == ' ') a++;
        if (*a) {
            dsp_set_gain(player_dsp(u->pl), atof(a) / 100.0);
            u->mute_saved = 0;
        }
        snprintf(b->msg, sizeof b->msg, "vol: %d%%",
                 (int)(dsp_gain(player_dsp(u->pl)) * 100 + 0.5));
        return 1;
    }
    if (!strncmp(cmd, "radio", 5)) {
        const char *a = cmd + 5;
        while (*a == ' ') a++;
        if (!strncmp(a, "add ", 4)) {
            const char *uu = a + 4;
            while (*uu == ' ') uu++;
            const char *sp = strchr(uu, ' ');
            if (!sp || !strstr(uu, "://")) {
                snprintf(b->msg, sizeof b->msg,
                         "usage: :radio add <url> <name>");
                return 1;
            }
            char url[1024];
            snprintf(url, sizeof url, "%.*s", (int)(sp - uu), uu);
            const char *nm = sp + 1;
            while (*nm == ' ') nm++;
            if (!*nm) {
                snprintf(b->msg, sizeof b->msg,
                         "usage: :radio add <url> <name>");
                return 1;
            }
            station_add_track((table *)b->tb, nm, url);
            station_persist(url, nm, 0);
            snprintf(b->msg, sizeof b->msg, "station added: %s", nm);
            return 1;
        }
        if (!strncmp(a, "rm ", 3)) {
            const char *nm = a + 3;
            while (*nm == ' ') nm++;
            if (station_persist(NULL, nm, 1) == 0)
                snprintf(b->msg, sizeof b->msg,
                         "removed '%s' (gone next start; still listed now)", nm);
            else
                snprintf(b->msg, sizeof b->msg, "no station '%s'", nm);
            return 1;
        }
        snprintf(b->msg, sizeof b->msg,
                 "usage: :radio add <url> <name> | :radio rm <name>  "
                 "(find them: format=radio)");
        return 1;
    }
    if (!strncmp(cmd, "dsp", 3)) {
        char name[32];
        double amt = 0.5;
        if (sscanf(cmd + 3, "%31s %lf", name, &amt) >= 1)
            dsp_set_mode(player_dsp(u->pl), name, amt);
        return 1;
    }
    return 0;
}

static const char *const CONSOLE_ID_KEYS[] = {
    "TITLE", "ARTIST", "ALBUMARTIST", "COMPOSER", "PERFORMER",
    "CONDUCTOR", "ORCHESTRA", "ENSEMBLE", "ALBUM", "DATE",
    "TRACKNUMBER", "DISCNUMBER", "GENRE", NULL
};
const char *const *console_id_keys = CONSOLE_ID_KEYS;
