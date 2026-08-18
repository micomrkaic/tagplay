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

#include "repl.h"
#include "query.h"
#include "player.h"
#include "cache.h"
#include "art.h"

#define COL_ALBUM 1u
#define COL_YEAR  2u
#define COL_GENRE 4u
#define COL_FMT   8u
#define COL_DUR   16u
#define COL_TRACK 32u
#define COLS_DEFAULT (COL_ALBUM | COL_FMT | COL_DUR)
#include <sys/select.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>


typedef struct {
    char   buf[1024];
    size_t len, cur;         /* content length, cursor */
    vec    match;            /* vec of size_t, current result */
    vec    last_good;        /* last successfully parsed result */
    int    parse_ok;
    char   sortspec[128];
    const table *tb;
    player *pl;
    vec    sel;              /* size_t table indices, insertion order */
    int    focus;            /* 0 = query line, 1 = result list */
    size_t lcur, loff;       /* list cursor + scroll offset */
    char   msg[160];         /* transient feedback line */
    double mute_saved;       /* pre-mute gain; 0 = not muted */
    /* focus: 0 = query line, 1 = result list, 2 = queue view */
    vec    qview;            /* snapshot of player queue (size_t) */
    size_t qcur, qoff;       /* queue view cursor + scroll */
    unsigned seen_note_seq;
    char   group[32];        /* tag key to group by; "" = off */
    unsigned cols_on;        /* COL_* bitmask */
    /* partial-refresh bookkeeping: where the status region starts and a
     * signature of everything that affects the layout above it */
    int    vu_row;           /* 1-based terminal row of the VU line */
    int    sig_valid;
    int    sig_focus, sig_playing;
    size_t sig_rows, sig_qpos, sig_track;
    int    sig_trows, sig_tcols;   /* terminal size at last full redraw */
} rstate;

static struct termios orig_tio;
static void raw_off(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio); }
static int raw_on(void) {
    if (tcgetattr(STDIN_FILENO, &orig_tio)) return -1;
    struct termios t = orig_tio;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

static int term_rows(void) {
    struct winsize ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row) return ws.ws_row;
    return 24;
}
static int term_cols(void) {
    struct winsize ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col) return ws.ws_col;
    return 80;
}

/* clip a UTF-8 string to at most `bytes` bytes without splitting a
 * multibyte sequence; returns the safe byte count */
static int u8clip(const char *s, int bytes) {
    int len = (int)strlen(s);
    if (len <= bytes) return len;
    while (bytes > 0 && ((unsigned char)s[bytes] & 0xC0) == 0x80) bytes--;
    return bytes;
}

#include <time.h>
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static long tag_num(const track *t, const char *key);

/* classical-aware identity: if COMPOSER exists and differs from ARTIST,
 * the composer takes the em-dash and the performer goes in parens:
 *   "J.S. Bach — Chaconne (Hamelin)" instead of "Hamelin — Chaconne" */
static void track_identity(const track *t, char *out, size_t sz) {
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

static void status_region(rstate *st, const player_status *ps, int cols);

/* (4) ASCII VU meter: two channel bars on one line, dB-scaled */
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

static long sel_find(const rstate *st, size_t ti) {
    for (size_t i = 0; i < st->sel.len; i++)
        if (*(size_t *)vec_at((vec *)&st->sel, i) == ti) return (long)i;
    return -1;
}
static void sel_toggle(rstate *st, size_t ti) {
    long i = sel_find(st, ti);
    if (i < 0) {
        vec_push(&st->sel, &ti);
    } else {
        memmove((char *)st->sel.data + (size_t)i * sizeof(size_t),
                (char *)st->sel.data + ((size_t)i + 1) * sizeof(size_t),
                (st->sel.len - (size_t)i - 1) * sizeof(size_t));
        st->sel.len--;
    }
}

#include <limits.h>
/* messages and playlist paths are display strings; truncation is fine */
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void config_path(char *out, size_t sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, sz, "%s/tagplay/config", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, sz, "%s/.config/tagplay/config", home ? home : ".");
    }
}
static const struct { const char *name; unsigned bit; } COLTAB[] = {
    { "album", COL_ALBUM }, { "year", COL_YEAR }, { "genre", COL_GENRE },
    { "fmt", COL_FMT }, { "dur", COL_DUR }, { "track", COL_TRACK },
};
static void config_save(rstate *st) {
    char p[4096];
    config_path(p, sizeof p);
    util_mkdirs_for(p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# tagplay display config (rewritten on :cols / :group)\n");
    fprintf(f, "cols=");
    int first = 1;
    for (size_t i = 0; i < sizeof COLTAB / sizeof *COLTAB; i++)
        if (st->cols_on & COLTAB[i].bit) {
            fprintf(f, "%s%s", first ? "" : ",", COLTAB[i].name);
            first = 0;
        }
    fprintf(f, "\ngroup=%s\n", st->group);
    fclose(f);
}
static void config_load(rstate *st) {
    st->cols_on = COLS_DEFAULT;
    st->group[0] = 0;
    char p[4096];
    config_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "cols=", 5)) {
            st->cols_on = 0;
            char *tok = strtok(line + 5, ",");
            while (tok) {
                for (size_t i = 0; i < sizeof COLTAB / sizeof *COLTAB; i++)
                    if (str_ieq(tok, COLTAB[i].name))
                        st->cols_on |= COLTAB[i].bit;
                tok = strtok(NULL, ",");
            }
        } else if (!strncmp(line, "group=", 6)) {
            snprintf(st->group, sizeof st->group, "%s", line + 6);
            for (char *q = st->group; *q; q++)
                *q = (char)toupper((unsigned char)*q);
        }
    }
    fclose(f);
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
    track_add_tag(t, "ARTIST", "Radio");
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
    "http://mp3.rtvslo.si/ars\tRadio Slovenija ARS\n"
    "http://mp3.rtvslo.si/val202\tVal 202\n"
    "http://mp3.rtvslo.si/prvi\tRadio Slovenija Prvi (News)\n";

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

static void playlist_dir(char *out, size_t sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, sz, "%s/tagplay/playlists", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, sz, "%s/.config/tagplay/playlists", home ? home : ".");
    }
}
static void playlist_path(char *out, size_t sz, const char *name) {
    char dir[4096];
    playlist_dir(dir, sizeof dir);
    snprintf(out, sz, "%s/%s.m3u", dir, name);
}

static void playlist_save(rstate *st, const char *name, const vec *idx) {
    char path[4352];
    playlist_path(path, sizeof path, name);
    util_mkdirs_for(path);
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(st->msg, sizeof st->msg, "cannot write %s", path);
        return;
    }
    fprintf(f, "#EXTM3U\n");
    for (size_t i = 0; i < idx->len; i++) {
        const track *t = table_at(st->tb, *(size_t *)vec_at((vec *)idx, i));
        const char *a = track_first_tag(t, "ARTIST");
        const char *ti = track_first_tag(t, "TITLE");
        fprintf(f, "#EXTINF:%ld,%s - %s\n", (long)(t->duration + 0.5),
                a ? a : "?", ti ? ti : "?");
        char rp[PATH_MAX];
        fprintf(f, "%s\n", realpath(t->path, rp) ? rp : t->path);
    }
    fclose(f);
    snprintf(st->msg, sizeof st->msg, "saved %zu tracks -> %s", idx->len, name);
}

static void playlist_load(rstate *st, const char *name) {
    char path[4352];
    playlist_path(path, sizeof path, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(st->msg, sizeof st->msg, "no playlist '%s'", name);
        return;
    }
    /* realpath index of the table, built once per load */
    size_t n = table_len(st->tb);
    char **rps = xmalloc(n * sizeof(char *));
    for (size_t i = 0; i < n; i++) {
        char rp[PATH_MAX];
        rps[i] = xstrdup(realpath(table_at(st->tb, i)->path, rp)
                         ? rp : table_at(st->tb, i)->path);
    }
    size_t found = 0, missing = 0;
    char line[PATH_MAX + 2];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        long hit = -1;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(rps[i], line)) { hit = (long)i; break; }
        if (hit < 0) { missing++; continue; }
        if (sel_find(st, (size_t)hit) < 0) vec_push(&st->sel, &(size_t){ (size_t)hit });
        found++;
    }
    fclose(f);
    for (size_t i = 0; i < n; i++) free(rps[i]);
    free(rps);
    if (missing)
        snprintf(st->msg, sizeof st->msg,
                 "loaded '%s': %zu tracks (%zu missing from library)",
                 name, found, missing);
    else
        snprintf(st->msg, sizeof st->msg, "loaded '%s': %zu tracks", name, found);
}

#include <dirent.h>
static void playlist_list(rstate *st) {
    char dir[4096];
    playlist_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(st->msg, sizeof st->msg, "no playlists yet");
        return;
    }
    char *w = st->msg;
    size_t left = sizeof st->msg;
    int k = snprintf(w, left, "playlists: ");
    w += k; left -= (size_t)k;
    struct dirent *e;
    int any = 0;
    while ((e = readdir(d)) && left > 2) {
        char *dot = strstr(e->d_name, ".m3u");
        if (!dot || dot[4]) continue;
        *dot = 0;
        k = snprintf(w, left, "%s%s", any ? ", " : "", e->d_name);
        if (k < 0 || (size_t)k >= left) break;
        w += k; left -= (size_t)k;
        any = 1;
    }
    closedir(d);
    if (!any) snprintf(st->msg, sizeof st->msg, "no playlists yet");
}

static void total_duration(const table *tb, const vec *idx, char *out, size_t sz) {
    double s = 0;
    for (size_t i = 0; i < idx->len; i++)
        s += table_at(tb, *(size_t *)vec_at((vec *)idx, i))->duration;
    fmt_duration_long(s, out, sz);
}

static void print_track_line(const rstate *st, size_t row, size_t ti, int width) {
    const track *t = table_at(st->tb, ti);
    char who[512], dur[16], line[1024];
    track_identity(t, who, sizeof who);
    fmt_duration(t->duration, dur, sizeof dur);
    int selected = sel_find(st, ti) >= 0;
    int hot = (st->focus == 1 && row == st->lcur);
    size_t off = 0;
    off += (size_t)snprintf(line + off, sizeof line - off, "%4zu [%c] ",
                            row + 1, selected ? 'x' : ' ');
    if ((st->cols_on & COL_TRACK) || st->group[0]) {
        long tn = tag_num(t, "TRACKNUMBER");
        if (tn > 0)
            off += (size_t)snprintf(line + off, sizeof line - off,
                                    "%2ld. ", tn);
        else
            off += (size_t)snprintf(line + off, sizeof line - off, "    ");
    }
    off += (size_t)snprintf(line + off, sizeof line - off, "%s", who);
    if ((st->cols_on & COL_ALBUM) && !str_ieq(st->group, "ALBUM")) {
        const char *album = track_first_tag(t, "ALBUM");
        off += (size_t)snprintf(line + off, sizeof line - off, "  [%s]",
                                album ? album : "?");
    }
    if (st->cols_on & COL_YEAR) {
        long y = tag_num(t, "DATE");
        if (!y) y = tag_num(t, "YEAR");
        if (y) off += (size_t)snprintf(line + off, sizeof line - off,
                                       "  (%ld)", y);
    }
    if (st->cols_on & COL_GENRE) {
        const char *g = track_first_tag(t, "GENRE");
        if (g) off += (size_t)snprintf(line + off, sizeof line - off,
                                       "  %s", g);
    }
    if (st->cols_on & COL_FMT)
        off += (size_t)snprintf(line + off, sizeof line - off, "  %s",
                                fmt_name(t->fmt));
    if (st->cols_on & COL_DUR)
        off += (size_t)snprintf(line + off, sizeof line - off, "%s%s",
                                (st->cols_on & COL_FMT) ? "·" : "  ", dur);
    printf("%s%.*s\x1b[0m\x1b[K\r\n", hot ? "\x1b[7m" : "",
           u8clip(line, width), line);
}

/* group header, e.g. "── Sonatas & Partitas ── (1720)" */
static void print_group_header(rstate *st, const track *t, int width) {
    const char *v = track_first_tag(t, st->group);
    long y = tag_num(t, "DATE");
    char line[512];
    snprintf(line, sizeof line, "\xe2\x94\x80\xe2\x94\x80 %s %s%ld%s",
             v ? v : "(none)", y ? "\xe2\x94\x80\xe2\x94\x80 " : "",
             y, y ? "" : "");
    if (!y) line[strlen(line) - 1] = 0; /* drop stray 0 */
    printf("\x1b[90m%.*s\x1b[0m\x1b[K\r\n", u8clip(line, width), line);
}

/* does row i start a new group relative to row i-1? */
static int group_breaks(rstate *st, const vec *show, size_t i) {
    if (!st->group[0]) return 0;
    const track *cur = table_at(st->tb, *(size_t *)vec_at((vec *)show, i));
    if (i == 0) return 1;
    const track *prv = table_at(st->tb, *(size_t *)vec_at((vec *)show, i - 1));
    const char *a = track_first_tag(cur, st->group);
    const char *b = track_first_tag(prv, st->group);
    return strcasecmp(a ? a : "", b ? b : "") != 0;
}

/* how many tracks starting at `from` fit in `lines` display lines */
static size_t tracks_that_fit(rstate *st, const vec *show, size_t from,
                              int lines) {
    size_t i = from;
    int used = 0;
    while (i < show->len && used < lines) {
        if (group_breaks(st, show, i)) used++;
        if (used >= lines) break;
        used++;
        i++;
    }
    return i - from;
}

static long tag_num(const track *t, const char *key) {
    const char *v = track_first_tag(t, key);
    return v ? atol(v) : 0;
}
struct gctx { const table *tb; const char *key; };
static int group_cmp(const void *a, const void *b, void *ud) {
    struct gctx *g = ud;
    const track *ta = table_at(g->tb, *(const size_t *)a);
    const track *tb_ = table_at(g->tb, *(const size_t *)b);
    const char *va = track_first_tag(ta, g->key);
    const char *vb = track_first_tag(tb_, g->key);
    int c = strcasecmp(va ? va : "", vb ? vb : "");
    if (c) return c;
    long d = tag_num(ta, "DISCNUMBER") - tag_num(tb_, "DISCNUMBER");
    if (d) return d < 0 ? -1 : 1;
    d = tag_num(ta, "TRACKNUMBER") - tag_num(tb_, "TRACKNUMBER");
    if (d) return d < 0 ? -1 : 1;
    const char *na = track_first_tag(ta, "TITLE");
    const char *nb = track_first_tag(tb_, "TITLE");
    return strcasecmp(na ? na : "", nb ? nb : "");
}
static void apply_group(rstate *st) {
    if (!st->group[0] || !st->match.len) return;
    struct gctx g = { st->tb, st->group };
    psort(st->match.data, st->match.len, sizeof(size_t), group_cmp, &g);
}

static void rerun(rstate *st) {
    qnode *q = query_parse(st->buf, 1 /* tolerant */);
    if (!q && st->len > 0) {
        st->parse_ok = 0; /* keep last_good on display */
        return;
    }
    query_run(q, st->tb, &st->match);
    if (st->sortspec[0]) query_sort(st->tb, &st->match, st->sortspec);
    apply_group(st);
    query_free(q);
    st->parse_ok = 1;
    /* copy into last_good */
    st->last_good.len = 0;
    for (size_t i = 0; i < st->match.len; i++)
        vec_push(&st->last_good, vec_at(&st->match, i));
}

static void redraw_queue(rstate *st) {
    player_status ps;
    player_get_status(st->pl, &ps);
    player_get_queue(st->pl, &st->qview);
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
        track_identity(t, who, sizeof who);
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
    st->sig_playing = ps.playing;
    st->sig_rows = st->qview.len;
    st->sig_qpos = ps.queue_pos;
    st->sig_track = ps.track_index;
    fflush(stdout);
}

/* VU line + progress line + status line (3 rows always; the last line
 * carries no trailing newline) */
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
static void status_region(rstate *st, const player_status *ps, int cols) {
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
            track_identity(ct, mt, sizeof mt);
        int mw = cols - 46;
        if (mw < 12) mw = 12;
        char clock[40];
        if (ps->dur > 0) snprintf(clock, sizeof clock, "%s/%s", cp, cd);
        else             snprintf(clock, sizeof clock, "%s \xe2\x88\x9e", cp);
        char tailtxt[256];
        snprintf(tailtxt, sizeof tailtxt,
                 " %s [%zu/%zu] %dk dsp:%s vol:%d%%%s",
                 clock, ps->queue_pos + 1, ps->queue_len, ps->rate / 1000,
                 dsp_mode_name(player_dsp(st->pl)),
                 (int)(dsp_gain(player_dsp(st->pl)) * 100 + 0.5),
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

static void redraw(rstate *st, size_t prev_count) {
    if (st->focus == 2) { redraw_queue(st); return; }
    const vec *show = st->parse_ok ? &st->match : &st->last_good;
    player_status pre;
    player_get_status(st->pl, &pre);
    int rows = term_rows();
    int cols = term_cols();
    int chrome = 5 + (st->msg[0] ? 2 : 0) + (pre.playing ? 3 : 0);
    int avail = rows - chrome;
    if ((size_t)avail < show->len) avail--;       /* "… more" line */
    if (avail < 3) avail = 3;

    printf("\x1b[H"); /* home; lines clear themselves with \\x1b[K */
    {
        char hdr[256];
        snprintf(hdr, sizeof hdr, "tagplay — %zu tracks   %s", table_len(st->tb),
                 st->focus
                   ? "LIST: Space toggle · a all · i invert · t tags · c clear · +/- vol · Enter play"
                   : "Tab: select tracks · Enter: play · :help");
        printf("%.*s\x1b[K\r\n\x1b[K\r\n", u8clip(hdr, cols - 1), hdr);
    }
    /* clamp cursor and scroll the window around it */
    if (st->lcur >= show->len) st->lcur = show->len ? show->len - 1 : 0;
    if (st->loff > st->lcur) st->loff = st->lcur;
    if (avail > 0 && st->lcur >= st->loff + (size_t)avail)
        st->loff = st->lcur - (size_t)avail + 1;
    if (st->loff >= show->len) st->loff = 0;
    size_t n = tracks_that_fit(st, show, st->loff, avail);
    /* keep the cursor visible under variable header consumption */
    while (st->focus == 1 && st->lcur >= st->loff + n && n < show->len) {
        st->loff++;
        n = tracks_that_fit(st, show, st->loff, avail);
    }
    for (size_t i = 0; i < n; i++) {
        size_t row = st->loff + i;
        if (group_breaks(st, show, row))
            print_group_header(st,
                table_at(st->tb, *(size_t *)vec_at((vec *)show, row)),
                cols - 2);
        print_track_line(st, row, *(size_t *)vec_at((vec *)show, row), cols - 2);
    }
    if (st->loff + n < show->len)
        printf("      … %zu more\x1b[K\r\n", show->len - st->loff - n);
    if (st->msg[0]) printf("\r\n  %.*s\x1b[K\r\n", u8clip(st->msg, cols - 3), st->msg);
    printf("\x1b[K\r\n");

    player_status ps;
    player_get_status(st->pl, &ps);
    /* rows above VU: header+blank(2) + list(n) + overflow + msg(2) + blank(1) */
    st->vu_row = 2 + (int)n + (st->loff + n < show->len ? 1 : 0)
               + (st->msg[0] ? 2 : 0) + 1 + 1;
    if (ps.playing) {
        status_region(st, &ps, cols);
        printf("\x1b[K\r\n");
    }
    char tot[32];
    total_duration(st->tb, show, tot, sizeof tot);
    const char *dim = st->parse_ok ? "" : "\x1b[2m";
    const char *rst = "\x1b[0m";
    if (prev_count != (size_t)-1 && prev_count != show->len)
        printf("%s%zu → %zu tracks · %s%s", dim, prev_count, show->len, tot, rst);
    else
        printf("%s%zu tracks · %s%s", dim, show->len, tot, rst);
    if (st->sel.len) {
        char stot[32];
        total_duration(st->tb, &st->sel, stot, sizeof stot);
        printf("   \x1b[1mselected: %zu · %s\x1b[0m", st->sel.len, stot);
    }
    if (st->sortspec[0]) printf("   (sort: %s)", st->sortspec);
    printf("\x1b[K\r\n> %.*s\x1b[K\x1b[0J", (int)st->len, st->buf);
    st->sig_valid = 1;
    st->sig_trows = rows;
    st->sig_tcols = cols;
    st->sig_focus = st->focus;
    st->sig_playing = ps.playing;
    st->sig_rows = show->len;
    st->sig_qpos = ps.queue_pos;
    st->sig_track = ps.track_index;
    /* place cursor */
    if (st->cur < st->len)
        printf("\x1b[%zuD", st->len - st->cur);
    fflush(stdout);
}

static void list_all(rstate *st) {
    const vec *show = st->parse_ok ? &st->match : &st->last_good;
    raw_off();
    printf("\x1b[2J\x1b[H");
    for (size_t i = 0; i < show->len; i++) {
        const track *t = table_at(st->tb, *(size_t *)vec_at((vec *)show, i));
        char dur[16];
        fmt_duration(t->duration, dur, sizeof dur);
        const char *a = track_first_tag(t, "ARTIST");
        const char *ti = track_first_tag(t, "TITLE");
        printf("%4zu [%c] %-24.24s %-40.40s %8s  %s\n",
               i + 1, sel_find(st, *(size_t *)vec_at((vec *)show, i)) >= 0 ? 'x' : ' ',
               a ? a : "?", ti ? ti : "?", dur, t->path);
    }
    printf("\n[%zu tracks — press Enter to continue]", show->len);
    fflush(stdout);
    getchar();
    raw_on();
}

static void show_help(void) {
    raw_off();
    printf("\x1b[2J\x1b[H"
        "Query syntax:\n"
        "  bare words          substring match on any field (implicit AND)\n"
        "  field ~ \"regex\"     PCRE2, case-insensitive\n"
        "  field = value       exact (case-insensitive); != for negation\n"
        "  year<1990 length>=3:00 rate=96000   numeric comparisons\n"
        "  & | ! ( )           boolean operators; ',' = '&'\n"
        "  fields: any tag key + path format length rate year track disc\n"
        "Commands:\n"
        "  Enter               play current results (replaces queue)\n"
        "  :ls                 list all matches\n"
        "  :p  :n  :b  :stop   pause/resume, next, prev, stop\n"
        "  Ctrl-P/N/B          same, without clearing the query\n"
        "  Tab                 cycles query -> list -> queue view -> query\n"
        "  queue view          shows what's playing: j/k move, Enter jumps,\n"
        "                      Space pause, left/right seek 10s, r restart,\n"
        "                      s stop, J/K reorder the queue, :save keeps it\n"
        "  list mode           j/k/arrows move, Space toggles [x],\n"
        "                      a adds all matches, i inverts, c clears, Enter plays;\n"
        "                      t shows every tag on the cursored track (also in\n"
        "                      queue view) — the answer to 'which field is that in?'\n"
        "  :save name          save selection (or matches) as m3u playlist\n"
        "  :load name          load playlist into selection\n"
        "  :lists              show saved playlists    :clear  drop selection\n"
        "  :seek 1:23          seek in current track\n"
        "  :vol 80             volume percent (0-200)\n"
        "  :dsp tube 0.4       dsp mode + amount; :dsp off\n"
        "  :sort f1,-f2        sort results (- = descending)   :sort  clears\n"
        "  :group album        group matches under dim headers, disc/track order\n"
        "                      inside; any tag works (:group composer); :group off\n"
        "  :cols +year -album  toggle row fields (album year genre fmt dur track);\n"
        "                      settings persist in ~/.config/tagplay/config\n"
        "  :stats              tag key frequency\n"
        "  :rescan             (restart with same args instead, for now)\n"
        "  :q                  quit\n"
        "\n[press Enter]");
    fflush(stdout);
    getchar();
    raw_on();
}

static void show_track_detail(rstate *st, size_t ti) {
    const track *t = table_at(st->tb, ti);
    raw_off();
    printf("\x1b[2J\x1b[H");
    char dur[16];
    fmt_duration(t->duration, dur, sizeof dur);
    printf("%s\n", t->path);
    if (t->fmt == FMT_RADIO)
        printf("%s stream\n\n", fmt_name(t->fmt));
    else
        printf("%s · %u Hz · %u ch · %s\n\n",
               fmt_name(t->fmt), t->sample_rate, t->channels, dur);
    /* identity keys first, in a fixed meaningful order */
    static const char *first[] = {
        "TITLE", "ARTIST", "ALBUMARTIST", "COMPOSER", "PERFORMER",
        "CONDUCTOR", "ORCHESTRA", "ENSEMBLE", "ALBUM", "DATE",
        "TRACKNUMBER", "DISCNUMBER", "GENRE", NULL
    };
    int shown[512] = { 0 };
    for (int k = 0; first[k]; k++)
        for (size_t i = 0; i < t->tags.len; i++) {
            tagkv *kv = vec_at((vec *)&t->tags, i);
            if (str_ieq(kv->key, first[k])) {
                printf("  %-14s %s\n", kv->key, kv->value);
                if (i < 512) shown[i] = 1;
            }
        }
    int rest = 0;
    for (size_t i = 0; i < t->tags.len; i++) {
        if (i < 512 && shown[i]) continue;
        tagkv *kv = vec_at((vec *)&t->tags, i);
        if (!rest) { printf("\n"); rest = 1; }
        printf("  %-14s %s\n", kv->key, kv->value);
    }
    /* embedded cover, if any, below the tags */
    if (t->fmt == FMT_FLAC || t->fmt == FMT_MP3) {
        size_t alen = 0;
        uint8_t *img = art_extract(t->path, t->fmt, &alen);
        if (img) {
            printf("\n");
            int cols = term_cols() - 4;
            if (cols > 72) cols = 72;
            if (art_render_ascii(img, alen, cols, 999) != 0)
                printf("  (embedded art present but undecodable)\n");
            free(img);
        } else {
            printf("\n  (no embedded art)\n");
        }
    }
    printf("\n[press Enter]");
    fflush(stdout);
    getchar();
    raw_on();
}

/* full-screen cover for a track ('a' in the queue view) */
static void show_art(rstate *st, size_t ti) {
    const track *t = table_at(st->tb, ti);
    raw_off();
    printf("\x1b[2J\x1b[H");
    char who[512];
    track_identity(t, who, sizeof who);
    size_t alen = 0;
    uint8_t *img = (t->fmt == FMT_FLAC || t->fmt == FMT_MP3)
                 ? art_extract(t->path, t->fmt, &alen) : NULL;
    if (img) {
        int cols = term_cols() - 2;
        int rows = term_rows() - 4;
        if (art_render_ascii(img, alen, cols, rows) != 0)
            printf("embedded art present but undecodable\n");
        free(img);
    } else {
        printf("no embedded art\n");
    }
    printf("\n%s   [press Enter]", who);
    fflush(stdout);
    getchar();
    raw_on();
}

static void show_stats(rstate *st) {
    raw_off();
    printf("\x1b[2J\x1b[H");
    /* naive key frequency */
    vec keys; vec_init(&keys, sizeof(char *));
    vec counts; vec_init(&counts, sizeof(size_t));
    for (size_t i = 0; i < table_len(st->tb); i++) {
        const track *t = table_at(st->tb, i);
        for (size_t j = 0; j < t->tags.len; j++) {
            tagkv *kv = vec_at((vec *)&t->tags, j);
            size_t k;
            for (k = 0; k < keys.len; k++)
                if (str_ieq(*(char **)vec_at(&keys, k), kv->key)) break;
            if (k == keys.len) {
                char *dup = xstrdup(kv->key);
                vec_push(&keys, &dup);
                size_t one = 1;
                vec_push(&counts, &one);
            } else {
                (*(size_t *)vec_at(&counts, k))++;
            }
        }
    }
    for (size_t k = 0; k < keys.len; k++)
        printf("%8zu  %s\n", *(size_t *)vec_at(&counts, k), *(char **)vec_at(&keys, k));
    for (size_t k = 0; k < keys.len; k++) free(*(char **)vec_at(&keys, k));
    vec_free(&keys); vec_free(&counts);
    printf("\n[press Enter]");
    fflush(stdout);
    getchar();
    raw_on();
}

static FILE *dbg;
static void dbglog(const char *msg) {
    if (!getenv("TAGPLAY_DEBUG")) return;
    if (!dbg) dbg = fopen("/tmp/tagplay.log", "w");
    if (dbg) { fprintf(dbg, "%s\n", msg); fflush(dbg); }
}
static void handle_command(rstate *st, const char *cmd, int *quit) {
    dbglog(cmd);
    if (!strcmp(cmd, "q") || !strcmp(cmd, "quit")) { *quit = 1; return; }
    if (!strncmp(cmd, "sort", 4)) {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        size_t n = strlen(arg);
        if (n >= sizeof st->sortspec) n = sizeof st->sortspec - 1;
        memcpy(st->sortspec, arg, n);
        st->sortspec[n] = 0;
        return;
    }
    if (!strcmp(cmd, "help")) { show_help(); return; }
    if (!strcmp(cmd, "stats")) { show_stats(st); return; }
    if (!strcmp(cmd, "ls")) { list_all(st); return; }
    if (!strcmp(cmd, "p") || !strcmp(cmd, "pause")) { player_toggle_pause(st->pl); return; }
    if (!strcmp(cmd, "n") || !strcmp(cmd, "next")) { player_next(st->pl); return; }
    if (!strcmp(cmd, "b") || !strcmp(cmd, "prev")) { player_prev(st->pl); return; }
    if (!strcmp(cmd, "stop")) { player_stop(st->pl); return; }
    if (!strncmp(cmd, "seek ", 5)) {
        double target;
        long mm, ss2;
        if (sscanf(cmd + 5, "%ld:%ld", &mm, &ss2) == 2) target = (double)(mm * 60 + ss2);
        else target = atof(cmd + 5);
        player_seek(st->pl, target);
        return;
    }
    if (!strncmp(cmd, "vol", 3)) {
        const char *a = cmd + 3;
        while (*a == ' ') a++;
        if (*a) {
            dsp_set_gain(player_dsp(st->pl), atof(a) / 100.0);
            st->mute_saved = 0;
        }
        snprintf(st->msg, sizeof st->msg, "vol: %d%%",
                 (int)(dsp_gain(player_dsp(st->pl)) * 100 + 0.5));
        return;
    }
    if (!strncmp(cmd, "save ", 5)) {
        if (st->focus == 2 || (!st->sel.len && st->qview.len)) {
            /* queue view (or nothing else to save): snapshot the queue,
             * which captures any J/K reordering */
            player_get_queue(st->pl, &st->qview);
            if (st->qview.len) { playlist_save(st, cmd + 5, &st->qview); return; }
        }
        const vec *src = st->sel.len ? &st->sel
                       : (st->parse_ok ? &st->match : &st->last_good);
        if (!src->len) snprintf(st->msg, sizeof st->msg, "nothing to save");
        else playlist_save(st, cmd + 5, src);
        return;
    }
    if (!strncmp(cmd, "load ", 5)) { playlist_load(st, cmd + 5); return; }
    if (!strcmp(cmd, "lists")) { playlist_list(st); return; }
    if (!strcmp(cmd, "clear")) {
        st->sel.len = 0;
        snprintf(st->msg, sizeof st->msg, "selection cleared");
        return;
    }
    if (!strncmp(cmd, "group", 5)) {
        const char *a = cmd + 5;
        while (*a == ' ') a++;
        if (!*a || !strcmp(a, "off")) {
            st->group[0] = 0;
            snprintf(st->msg, sizeof st->msg, "grouping off");
        } else {
            snprintf(st->group, sizeof st->group, "%s", a);
            for (char *q = st->group; *q; q++)
                *q = (char)toupper((unsigned char)*q);
            st->sortspec[0] = 0;   /* grouping owns the order */
            snprintf(st->msg, sizeof st->msg,
                     "grouped by %s (disc/track order inside; :group off to clear)",
                     st->group);
        }
        config_save(st);
        rerun(st);
        return;
    }
    if (!strncmp(cmd, "cols", 4)) {
        const char *a = cmd + 4;
        while (*a == ' ') a++;
        if (*a) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s", a);
            for (char *tok = strtok(buf, " ,"); tok; tok = strtok(NULL, " ,")) {
                int on = 1;
                if (*tok == '-') { on = 0; tok++; }
                else if (*tok == '+') tok++;
                if (str_ieq(tok, "reset")) { st->cols_on = COLS_DEFAULT; continue; }
                int hit = 0;
                for (size_t i = 0; i < sizeof COLTAB / sizeof *COLTAB; i++)
                    if (str_ieq(tok, COLTAB[i].name)) {
                        if (on) st->cols_on |= COLTAB[i].bit;
                        else    st->cols_on &= ~COLTAB[i].bit;
                        hit = 1;
                    }
                if (!hit) {
                    snprintf(st->msg, sizeof st->msg,
                             "unknown column '%s' (album year genre fmt dur track)",
                             tok);
                    return;
                }
            }
            config_save(st);
        }
        char cur[128] = "";
        for (size_t i = 0; i < sizeof COLTAB / sizeof *COLTAB; i++)
            if (st->cols_on & COLTAB[i].bit) {
                strcat(cur, COLTAB[i].name);
                strcat(cur, " ");
            }
        snprintf(st->msg, sizeof st->msg,
                 "columns: %s (toggle: :cols +year -album | reset)", cur);
        return;
    }
    if (!strncmp(cmd, "radio", 5)) {
        const char *a = cmd + 5;
        while (*a == ' ') a++;
        if (!strncmp(a, "add ", 4)) {
            const char *u = a + 4;
            while (*u == ' ') u++;
            const char *sp = strchr(u, ' ');
            if (!sp || !strstr(u, "://")) {
                snprintf(st->msg, sizeof st->msg,
                         "usage: :radio add <url> <name>");
                return;
            }
            char url[1024];
            snprintf(url, sizeof url, "%.*s", (int)(sp - u), u);
            const char *nm = sp + 1;
            while (*nm == ' ') nm++;
            if (!*nm) {
                snprintf(st->msg, sizeof st->msg,
                         "usage: :radio add <url> <name>");
                return;
            }
            station_add_track((table *)st->tb, nm, url);
            station_persist(url, nm, 0);
            rerun(st);
            snprintf(st->msg, sizeof st->msg, "station added: %s", nm);
            return;
        }
        if (!strncmp(a, "rm ", 3)) {
            const char *nm = a + 3;
            while (*nm == ' ') nm++;
            if (station_persist(NULL, nm, 1) == 0)
                snprintf(st->msg, sizeof st->msg,
                         "removed '%s' (gone next start; still listed now)", nm);
            else
                snprintf(st->msg, sizeof st->msg, "no station '%s'", nm);
            return;
        }
        snprintf(st->msg, sizeof st->msg,
                 "usage: :radio add <url> <name> | :radio rm <name>  "
                 "(find them: format=radio)");
        return;
    }
    if (!strncmp(cmd, "dsp", 3)) {
        char name[32];
        double amt = 0.5;
        if (sscanf(cmd + 3, "%31s %lf", name, &amt) >= 1)
            dsp_set_mode(player_dsp(st->pl), name, amt);
        return;
    }
}

void repl_run(const table *tb, player *pl) {
    rstate st;
    memset(&st, 0, sizeof st);
    st.tb = tb;
    st.pl = pl;
    vec_init(&st.match, sizeof(size_t));
    vec_init(&st.last_good, sizeof(size_t));
    vec_init(&st.sel, sizeof(size_t));
    vec_init(&st.qview, sizeof(size_t));
    st.parse_ok = 1;

    /* select() on STDIN_FILENO + buffered getchar() would lose bytes:
     * one read() can pull several keys into the stdio buffer where
     * select can't see them. Unbuffered stdin makes getchar == read(1). */
    config_load(&st);
    setvbuf(stdin, NULL, _IONBF, 0);
    /* full output buffering: a redraw becomes one write(), so the
     * terminal never renders a half-painted frame */
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    /* the REPL owns the terminal; audio libraries (ALSA under SDL) chat
     * on stderr and would stamp their warnings across the display.
     * Redirect stderr to a log for the session instead. */
    {
        char logp[4096];
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) snprintf(logp, sizeof logp, "%s/tagplay/stderr.log", xdg);
        else snprintf(logp, sizeof logp, "%s/.cache/tagplay/stderr.log",
                      getenv("HOME") ? getenv("HOME") : ".");
        util_mkdirs_for(logp);
        if (!freopen(logp, "w", stderr))
            (void)!freopen("/dev/null", "w", stderr);
    }
    if (raw_on()) {
        fprintf(stderr, "tagplay: not a terminal; use -q EXPR\n");
        return;
    }
    atexit(raw_off);
    rerun(&st);
    redraw(&st, (size_t)-1);

    int quit = 0;
    while (!quit) {
        /* wait for a key, or 1 s timeout to refresh the status line */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        player_status tick;
        player_get_status(st.pl, &tick);
        /* 10 Hz while audibly playing (VU + marquee), 1 Hz otherwise */
        struct timeval tv = tick.playing == 1
                          ? (struct timeval){ 0, 100000 }
                          : (struct timeval){ 1, 0 };
        int r = select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv);
        if (r == 0) {
            if (!(tick.playing || st.focus == 2)) continue;
            player_status now;
            player_get_status(st.pl, &now);
            if (now.note_seq != st.seen_note_seq && now.note[0]) {
                st.seen_note_seq = now.note_seq;
                snprintf(st.msg, sizeof st.msg, "%s", now.note);
                redraw(&st, (size_t)-1);
                continue;
            }
            int same_layout = st.sig_valid &&
                st.sig_trows == term_rows() &&
                st.sig_tcols == term_cols() &&
                st.sig_focus == st.focus &&
                st.sig_playing == now.playing &&
                st.sig_qpos == now.queue_pos &&
                st.sig_track == now.track_index &&
                (st.focus != 2 || st.sig_rows == now.queue_len);
            if (same_layout && st.vu_row > 0) {
                /* surgical: rewrite only the VU + status lines in place,
                 * leaving the list (and the prompt cursor) untouched */
                printf("\x1b" "7\x1b[%d;1H", st.vu_row);
                status_region(&st, &now, term_cols());
                printf("\x1b" "8");
                fflush(stdout);
            } else {
                redraw(&st, (size_t)-1);
            }
            continue;
        }
        if (r < 0) continue;
        int c = getchar();
        if (c == EOF) break;
        st.msg[0] = 0; /* feedback lives for one keystroke */
        if (getenv("TAGPLAY_DEBUG")) {
            char m[32];
            snprintf(m, sizeof m, "key %d", c);
            dbglog(m);
        }
        size_t prev = (st.parse_ok ? st.match.len : st.last_good.len);
        const vec *shown = st.parse_ok ? &st.match : &st.last_good;
        if (c == '\t') { /* Tab: cycle query -> list -> queue -> query */
            player_status tps;
            player_get_status(st.pl, &tps);
            if (st.focus == 0 && shown->len) st.focus = 1;
            else if (st.focus == 1 && tps.queue_len) {
                st.focus = 2;
                st.qcur = tps.queue_pos;
            } else if (st.focus == 0 && tps.queue_len) {
                st.focus = 2;
                st.qcur = tps.queue_pos;
            } else st.focus = 0;
            redraw(&st, prev);
            continue;
        }
        if (st.focus == 2) { /* ---- queue view ---- */
            int handled = 1;
            player_status qps;
            player_get_status(st.pl, &qps);
            if (c == 'j') { if (st.qcur + 1 < st.qview.len) st.qcur++; }
            else if (c == 'k') { if (st.qcur > 0) st.qcur--; }
            else if (c == 'g') st.qcur = 0;
            else if (c == 'G') st.qcur = st.qview.len ? st.qview.len - 1 : 0;
            else if (c == ' ') {           /* Space: pause/resume */
                player_toggle_pause(st.pl);
            } else if (c == '>') {         /* fast-forward 60 s */
                double tp = qps.pos + 60;
                if (qps.dur > 0 && tp > qps.dur - 0.5) tp = qps.dur - 0.5;
                player_seek(st.pl, tp);
            } else if (c == '<') {         /* rewind 60 s */
                player_seek(st.pl, qps.pos > 60 ? qps.pos - 60 : 0);
            } else if (c == 'r') {         /* restart current track */
                player_seek(st.pl, 0);
            } else if (c == 's') {         /* stop (queue kept) */
                player_stop(st.pl);
            } else if (c == 't') {
                if (st.qview.len)
                    show_track_detail(&st,
                        *(size_t *)vec_at(&st.qview, st.qcur));
            } else if (c == 'a') {
                if (st.qview.len)
                    show_art(&st, *(size_t *)vec_at(&st.qview, st.qcur));
            } else if (c == 'J') {         /* move cursored track down */
                if (st.qcur + 1 < st.qview.len) {
                    player_move(st.pl, st.qcur, st.qcur + 1);
                    st.qcur++;
                }
            } else if (c == 'K') {         /* move cursored track up */
                if (st.qcur > 0) {
                    player_move(st.pl, st.qcur, st.qcur - 1);
                    st.qcur--;
                }
            }
            else if (c == '+' || c == '=' || c == '-') {
                double g = dsp_gain(player_dsp(st.pl)) + (c == '-' ? -0.05 : 0.05);
                dsp_set_gain(player_dsp(st.pl), g);
                st.mute_saved = 0;
                snprintf(st.msg, sizeof st.msg, "vol: %d%%",
                         (int)(dsp_gain(player_dsp(st.pl)) * 100 + 0.5));
            } else if (c == 'm') {
                if (st.mute_saved > 0) {
                    dsp_set_gain(player_dsp(st.pl), st.mute_saved);
                    st.mute_saved = 0;
                } else {
                    st.mute_saved = dsp_gain(player_dsp(st.pl));
                    if (st.mute_saved <= 0) st.mute_saved = 1.0;
                    dsp_set_gain(player_dsp(st.pl), 0);
                }
            } else if (c == '\r' || c == '\n') {
                player_jump(st.pl, st.qcur); /* play the cursored track */
            } else if (c == 27) {
                int c1 = getchar();
                if (c1 == '[') {
                    int c2 = getchar();
                    if (c2 == 'B') { if (st.qcur + 1 < st.qview.len) st.qcur++; }
                    else if (c2 == 'A') { if (st.qcur > 0) st.qcur--; }
                    else if (c2 == 'C') { /* right: seek +10s */
                        double tp = qps.pos + 10;
                        if (qps.dur > 0 && tp > qps.dur - 0.5) tp = qps.dur - 0.5;
                        player_seek(st.pl, tp);
                    } else if (c2 == 'D') { /* left: seek -10s */
                        player_seek(st.pl, qps.pos > 10 ? qps.pos - 10 : 0);
                    } else if (c2 == '1') { /* ESC [ 1 ; 2 C/D: shift-arrows */
                        int c3 = getchar();
                        if (c3 == ';') {
                            int c4 = getchar(), c5 = getchar();
                            if (c4 == '2' && c5 == 'C') {
                                double tp = qps.pos + 60;
                                if (qps.dur > 0 && tp > qps.dur - 0.5)
                                    tp = qps.dur - 0.5;
                                player_seek(st.pl, tp);
                            } else if (c4 == '2' && c5 == 'D') {
                                player_seek(st.pl,
                                    qps.pos > 60 ? qps.pos - 60 : 0);
                            }
                        }
                    }
                    else if (c2 == '5' || c2 == '6') {
                        getchar();
                        int rw = term_rows() - 6;
                        if (rw < 1) rw = 1;
                        if (c2 == '6') {
                            st.qcur += (size_t)rw;
                            if (st.qcur >= st.qview.len)
                                st.qcur = st.qview.len ? st.qview.len - 1 : 0;
                        } else st.qcur = st.qcur > (size_t)rw
                                       ? st.qcur - (size_t)rw : 0;
                    }
                } else st.focus = 0;
            } else if (c == 16 || c == 14 || c == 2) {
                handled = 0;          /* transport ctrl keys fall through */
            } else if (c == ':') {
                st.len = st.cur = 0;
                st.buf[0] = 0;
                rerun(&st);
                st.focus = 0;
                handled = 0;
            } else if (c >= 32 && c < 127) {
                st.focus = 0;         /* typing returns to search */
                handled = 0;
            }
            if (handled) { redraw(&st, prev); continue; }
        }
        if (st.focus == 1) { /* ---- list mode ---- */
            int handled = 1;
            if (c == 'j') { if (st.lcur + 1 < shown->len) st.lcur++; }
            else if (c == 'k') { if (st.lcur > 0) st.lcur--; }
            else if (c == 'g') st.lcur = 0;
            else if (c == 'G') st.lcur = shown->len ? shown->len - 1 : 0;
            else if (c == ' ') {
                if (shown->len) {
                    sel_toggle(&st, *(size_t *)vec_at((vec *)shown, st.lcur));
                    if (st.lcur + 1 < shown->len) st.lcur++; /* advance */
                }
            } else if (c == 'a') {
                for (size_t i = 0; i < shown->len; i++) {
                    size_t ti = *(size_t *)vec_at((vec *)shown, i);
                    if (sel_find(&st, ti) < 0) vec_push(&st.sel, &ti);
                }
                snprintf(st.msg, sizeof st.msg, "added %zu -> sel:%zu",
                         shown->len, st.sel.len);
            } else if (c == '+' || c == '=' || c == '-') {
                double g = dsp_gain(player_dsp(st.pl)) + (c == '-' ? -0.05 : 0.05);
                dsp_set_gain(player_dsp(st.pl), g);
                st.mute_saved = 0;
                snprintf(st.msg, sizeof st.msg, "vol: %d%%",
                         (int)(dsp_gain(player_dsp(st.pl)) * 100 + 0.5));
            } else if (c == 'm') {
                if (st.mute_saved > 0) {
                    dsp_set_gain(player_dsp(st.pl), st.mute_saved);
                    st.mute_saved = 0;
                    snprintf(st.msg, sizeof st.msg, "unmuted: %d%%",
                             (int)(dsp_gain(player_dsp(st.pl)) * 100 + 0.5));
                } else {
                    st.mute_saved = dsp_gain(player_dsp(st.pl));
                    if (st.mute_saved <= 0) st.mute_saved = 1.0;
                    dsp_set_gain(player_dsp(st.pl), 0);
                    snprintf(st.msg, sizeof st.msg, "muted");
                }
            } else if (c == 't') {
                if (shown->len)
                    show_track_detail(&st,
                        *(size_t *)vec_at((vec *)shown, st.lcur));
            } else if (c == 'i') {
                for (size_t i = 0; i < shown->len; i++)
                    sel_toggle(&st, *(size_t *)vec_at((vec *)shown, i));
                snprintf(st.msg, sizeof st.msg, "selection inverted -> sel:%zu",
                         st.sel.len);
            } else if (c == 'c') {
                st.sel.len = 0;
                snprintf(st.msg, sizeof st.msg, "selection cleared");
            } else if (c == 27) {
                int c1 = getchar();
                if (c1 == '[') {
                    int c2 = getchar();
                    if (c2 == 'B') { if (st.lcur + 1 < shown->len) st.lcur++; }
                    else if (c2 == 'A') { if (st.lcur > 0) st.lcur--; }
                    else if (c2 == '5' || c2 == '6') { /* PgUp/PgDn */
                        getchar();
                        int rows = term_rows() - 6;
                        if (rows < 1) rows = 1;
                        if (c2 == '6') {
                            st.lcur += (size_t)rows;
                            if (st.lcur >= shown->len)
                                st.lcur = shown->len ? shown->len - 1 : 0;
                        } else {
                            st.lcur = st.lcur > (size_t)rows
                                    ? st.lcur - (size_t)rows : 0;
                        }
                    }
                } else st.focus = 0; /* bare Esc back to query */
            } else if (c == '\r' || c == '\n') {
                handled = 0;          /* Enter falls through to play */
            } else if (c == 16 || c == 14 || c == 2) {
                handled = 0;          /* transport ctrl keys fall through */
            } else if (c == ':') {
                /* commands from list mode get a fresh line: the query is
                 * not being edited here, so clearing it is safe */
                st.len = st.cur = 0;
                st.buf[0] = 0;
                rerun(&st);
                st.focus = 0;
                handled = 0;
            } else if (c >= 32 && c < 127) {
                st.focus = 0;         /* typing returns to the query */
                handled = 0;
            }
            if (handled) { redraw(&st, prev); continue; }
        }
        if (c == '\r' || c == '\n') {
            st.buf[st.len] = 0;
            if (st.buf[0] == ':') {
                handle_command(&st, st.buf + 1, &quit);
                st.len = st.cur = 0;
                st.buf[0] = 0;
                rerun(&st);
            } else {
                /* Enter: play the selection if any, else current results;
                 * clear the line so ':' commands start fresh */
                const vec *q = st.sel.len ? &st.sel
                             : (st.parse_ok ? &st.match : &st.last_good);
                if (q->len) {
                    player_play(st.pl, (const size_t *)q->data, q->len);
                    snprintf(st.msg, sizeof st.msg, "playing %zu track%s%s",
                             q->len, q->len == 1 ? "" : "s",
                             st.sel.len ? " (selection)" : "");
                    st.focus = 2;       /* land in the queue view */
                    st.qcur = 0;
                }
                st.len = st.cur = 0;
                st.buf[0] = 0;
                rerun(&st);
            }
        } else if (c == 127 || c == 8) { /* backspace */
            if (st.cur > 0) {
                memmove(st.buf + st.cur - 1, st.buf + st.cur, st.len - st.cur);
                st.cur--; st.len--;
                st.buf[st.len] = 0;
                rerun(&st);
            }
        } else if (c == 16) { /* ctrl-p: pause/resume */
            player_toggle_pause(st.pl);
        } else if (c == 14) { /* ctrl-n: next */
            player_next(st.pl);
        } else if (c == 2) {  /* ctrl-b: prev */
            player_prev(st.pl);
        } else if (c == 21) { /* ctrl-u */
            st.len = st.cur = 0;
            st.buf[0] = 0;
            rerun(&st);
        } else if (c == 23) { /* ctrl-w: delete word */
            while (st.cur > 0 && st.buf[st.cur - 1] == ' ') { st.cur--; st.len--; }
            while (st.cur > 0 && st.buf[st.cur - 1] != ' ') {
                memmove(st.buf + st.cur - 1, st.buf + st.cur, st.len - st.cur);
                st.cur--; st.len--;
            }
            st.buf[st.len] = 0;
            rerun(&st);
        } else if (c == 27) { /* escape sequences: arrows/home/end */
            int c1 = getchar();
            if (c1 == '[') {
                int c2 = getchar();
                if (c2 == 'D' && st.cur > 0) st.cur--;
                else if (c2 == 'C' && st.cur < st.len) st.cur++;
                else if (c2 == 'H') st.cur = 0;
                else if (c2 == 'F') st.cur = st.len;
                else if (c2 == '3') { /* delete key: [3~ */
                    getchar();
                    if (st.cur < st.len) {
                        memmove(st.buf + st.cur, st.buf + st.cur + 1,
                                st.len - st.cur - 1);
                        st.len--;
                        st.buf[st.len] = 0;
                        rerun(&st);
                    }
                }
            }
        } else if (c >= 32 && c < 127 && st.len + 1 < sizeof st.buf) {
            memmove(st.buf + st.cur + 1, st.buf + st.cur, st.len - st.cur);
            st.buf[st.cur++] = (char)c;
            st.len++;
            st.buf[st.len] = 0;
            rerun(&st);
        }
        if (!quit) redraw(&st, prev);
    }
    raw_off();
    printf("\n");
    vec_free(&st.match);
    vec_free(&st.last_good);
    vec_free(&st.sel);
    vec_free(&st.qview);
}
