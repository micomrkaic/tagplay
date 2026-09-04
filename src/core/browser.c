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

#include "browser.h"
#include "query.h"
#include "art.h"
#include "app.h"
#include "browser.h"

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




static struct termios orig_tio;
void browser_raw_off(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio); }
static int raw_on_(void) {
    if (tcgetattr(STDIN_FILENO, &orig_tio)) return -1;
    struct termios t = orig_tio;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

int term_rows(void) {
    struct winsize ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row) return ws.ws_row;
    return 24;
}
int term_cols(void) {
    struct winsize ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col) return ws.ws_col;
    return 80;
}

/* clip a UTF-8 string to at most `bytes` bytes without splitting a
 * multibyte sequence; returns the safe byte count */
int u8clip(const char *s, int bytes) {
    int len = (int)strlen(s);
    if (len <= bytes) return len;
    while (bytes > 0 && ((unsigned char)s[bytes] & 0xC0) == 0x80) bytes--;
    return bytes;
}

#include <time.h>
long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static long tag_num(const track *t, const char *key);

/* classical-aware identity: if COMPOSER exists and differs from ARTIST,
 * the composer takes the em-dash and the performer goes in parens:
 *   "J.S. Bach — Chaconne (Hamelin)" instead of "Hamelin — Chaconne" */
/* (4) ASCII VU meter: two channel bars on one line, dB-scaled */
static long sel_find(const browser *st, size_t ti) {
    for (size_t i = 0; i < st->sel.len; i++)
        if (*(size_t *)vec_at((vec *)&st->sel, i) == ti) return (long)i;
    return -1;
}
static void sel_toggle(browser *st, size_t ti) {
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
    if (xdg && *xdg) snprintf(out, sz, "%s/%s/config", xdg, APP->name);
    else {
        const char *home = getenv("HOME");
        snprintf(out, sz, "%s/.config/%s/config", home ? home : ".", APP->name);
    }
}
static const struct { const char *name; unsigned bit; } COLTAB[] = {
    { "album", COL_ALBUM }, { "year", COL_YEAR }, { "genre", COL_GENRE },
    { "fmt", COL_FMT }, { "dur", COL_DUR }, { "track", COL_TRACK },
};
static void config_save(browser *st) {
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
static void config_load(browser *st) {
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

static void playlist_dir(char *out, size_t sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, sz, "%s/%s/playlists", xdg, APP->name);
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

static void playlist_save(browser *st, const char *name, const vec *idx) {
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

static void playlist_load(browser *st, const char *name) {
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
static void playlist_list(browser *st) {
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

static void print_track_line(const browser *st, size_t row, size_t ti, int width) {
    const track *t = table_at(st->tb, ti);
    char who[512], dur[16], line[1024];
    APP->identity(t, who, sizeof who);
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
                                APP->fmt_name(t->fmt));
    if (st->cols_on & COL_DUR)
        off += (size_t)snprintf(line + off, sizeof line - off, "%s%s",
                                (st->cols_on & COL_FMT) ? "·" : "  ", dur);
    printf("%s%.*s\x1b[0m\x1b[K\r\n", hot ? "\x1b[7m" : "",
           u8clip(line, width), line);
}

/* group header, e.g. "── Sonatas & Partitas ── (1720)" */
static void print_group_header(browser *st, const track *t, int width) {
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
static int group_breaks(browser *st, const vec *show, size_t i) {
    if (!st->group[0]) return 0;
    const track *cur = table_at(st->tb, *(size_t *)vec_at((vec *)show, i));
    if (i == 0) return 1;
    const track *prv = table_at(st->tb, *(size_t *)vec_at((vec *)show, i - 1));
    const char *a = track_first_tag(cur, st->group);
    const char *b = track_first_tag(prv, st->group);
    return strcasecmp(a ? a : "", b ? b : "") != 0;
}

/* how many tracks starting at `from` fit in `lines` display lines */
static size_t tracks_that_fit(browser *st, const vec *show, size_t from,
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
static void apply_group(browser *st) {
    if (!st->group[0] || !st->match.len) return;
    struct gctx g = { st->tb, st->group };
    tp_sort(st->match.data, st->match.len, sizeof(size_t), group_cmp, &g);
}

static void rerun(browser *st) {
    if (st->sel_view) {
        /* selection view: the working list mirrors the marks, in
         * selection (playlist) order; sort/group deliberately skipped */
        st->match.len = 0;
        for (size_t i = 0; i < st->sel.len; i++)
            vec_push(&st->match, vec_at(&st->sel, i));
        st->parse_ok = 1;
        return;
    }
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

static void redraw(browser *st, size_t prev_count) {
    if (st->focus == 2) {
        if (APP->alt_view) APP->alt_view(st->ui, st);
        else st->focus = 0;
        return;
    }
    const vec *show = st->parse_ok ? &st->match : &st->last_good;
    int srows = APP->status_rows ? APP->status_rows(st->ui) : 0;
    int rows = term_rows();
    int cols = term_cols();
    int chrome = 5 + (st->msg[0] ? 2 : 0) + srows;
    int avail = rows - chrome;
    if (avail < 4) avail = 4;
    /* overflow accounting in LINES: group headers consume lines, so
     * tracks can overflow even when avail >= track count. Compute the
     * fit; if tracks remain, re-budget one line for the "more" row. */
    {
        size_t fit0 = tracks_that_fit(st, show, st->loff, avail);
        if (st->loff + fit0 < show->len) avail--;
    }

    printf("\x1b[H"); /* home; lines clear themselves with \\x1b[K */
    {
        char hdr[256];
        snprintf(hdr, sizeof hdr, "%s — %zu tracks   %s", APP->name, table_len(st->tb),
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

    /* rows above VU: header+blank(2) + list(n) + overflow + msg(2) + blank(1) */
    /* group headers rendered in the window shift the status region down;
     * the partial-refresh anchor must count them or ticks paint a ghost
     * region over the last track rows */
    int hdrs = 0;
    for (size_t i = st->loff; i < st->loff + n; i++)
        if (group_breaks(st, show, i)) hdrs++;
    st->vu_row = 2 + (int)n + hdrs + (st->loff + n < show->len ? 1 : 0)
               + (st->msg[0] ? 2 : 0) + 1 + 1;
    if (srows) {
        APP->status(st->ui, st, cols);
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
    st->sig_app = APP->ui_sig ? APP->ui_sig(st->ui) : 0;
    st->sig_rows = show->len;
    /* place cursor */
    if (st->cur < st->len)
        printf("\x1b[%zuD", st->len - st->cur);
    fflush(stdout);
}

static void list_all(browser *st) {
    const vec *show = st->parse_ok ? &st->match : &st->last_good;
    browser_raw_off();
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
    browser_raw_on();
}

static void show_help(void) {
    browser_raw_off();
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
        "  :sel                show only the marked tracks, in playlist order,\n"
        "                      for editing (Space unmarks); Enter plays them\n"
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
    browser_raw_on();
}

static void show_track_detail(browser *st, size_t ti) {
    const track *t = table_at(st->tb, ti);
    browser_raw_off();
    printf("\x1b[2J\x1b[H");
    char dur[16];
    fmt_duration(t->duration, dur, sizeof dur);
    printf("%s\n", t->path);
    (void)dur;
    {
        char hd[160];
        if (APP->detail_header) APP->detail_header(t, hd, sizeof hd);
        else snprintf(hd, sizeof hd, "%s", APP->fmt_name(t->fmt));
        printf("%s\n\n", hd);
    }
    /* identity keys first, in the app's meaningful order */
    static const char *fallback_keys[] = { "TITLE", NULL };
    const char *const *first = APP->id_keys ? APP->id_keys : fallback_keys;
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
    {
        size_t alen = 0;
        uint8_t *img = APP->art_read ? APP->art_read(t, &alen) : NULL;
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
    browser_raw_on();
}

/* full-screen cover for a track ('a' in the queue view) */
static void show_art(browser *st, size_t ti) {
    const track *t = table_at(st->tb, ti);
    browser_raw_off();
    printf("\x1b[2J\x1b[H");
    char who[512];
    APP->identity(t, who, sizeof who);
    size_t alen = 0;
    uint8_t *img = APP->art_read ? APP->art_read(t, &alen) : NULL;
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
    browser_raw_on();
}

static void show_stats(browser *st) {
    browser_raw_off();
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
    browser_raw_on();
}

static FILE *dbg;
static void dbglog(const char *msg) {
    if (!getenv("TAGPLAY_DEBUG")) return;
    if (!dbg) { char p[64]; snprintf(p, sizeof p, "/tmp/%s.log", APP->name); dbg = fopen(p, "w"); }
    if (dbg) { fprintf(dbg, "%s\n", msg); fflush(dbg); }
}
static void handle_command(browser *st, const char *cmd, int *quit) {
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
    if (!strncmp(cmd, "save ", 5)) {
        if (st->focus == 2 ||
            (!st->sel.len && APP->alt_len && APP->alt_len(st->ui))) {
            /* alt view (or nothing else to save): snapshot the app's
             * items, which captures any live reordering */
            if (APP->alt_snapshot && APP->alt_snapshot(st->ui, st) &&
                st->qview.len) {
                playlist_save(st, cmd + 5, &st->qview);
                return;
            }
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
    if (!strcmp(cmd, "sel")) {
        if (!st->sel.len) {
            snprintf(st->msg, sizeof st->msg, "nothing selected");
            return;
        }
        st->sel_view = 1;
        st->focus = 1;
        st->lcur = st->loff = 0;
        snprintf(st->msg, sizeof st->msg,
                 "%zu selected track%s — Space unmarks, Enter plays; any "
                 "typing returns to search", st->sel.len,
                 st->sel.len == 1 ? "" : "s");
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
    if (APP->command && APP->command(st->ui, st, cmd)) return;

}

void browser_run(const table *tb, void *ui) {
    browser st;
    memset(&st, 0, sizeof st);
    st.tb = tb;
    st.ui = ui;
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
        if (xdg && *xdg) snprintf(logp, sizeof logp, "%s/%s/stderr.log", xdg, APP->name);
        else snprintf(logp, sizeof logp, "%s/.cache/%s/stderr.log",
                      getenv("HOME") ? getenv("HOME") : ".", APP->name);
        util_mkdirs_for(logp);
        if (!freopen(logp, "w", stderr))
            (void)!freopen("/dev/null", "w", stderr);
    }
    if (raw_on_()) {
        fprintf(stderr, "%s: not a terminal; use -q EXPR\n", APP->name);
        return;
    }
    atexit(browser_raw_off);
    rerun(&st);
    redraw(&st, (size_t)-1);

    int quit = 0;
    while (!quit) {
        /* wait for a key, or timeout to refresh the status line */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        int pulse = APP->ui_pulse ? APP->ui_pulse(st.ui) : 0;
        /* 10 Hz while the app is animating, 1 Hz otherwise */
        struct timeval tv = pulse == 1 ? (struct timeval){ 0, 100000 }
                                       : (struct timeval){ 1, 0 };
        int r = select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv);
        if (r == 0) {
            if (!(pulse || st.focus == 2)) continue;
            if (APP->poll_msg && APP->poll_msg(st.ui, st.msg, sizeof st.msg)) {
                redraw(&st, (size_t)-1);
                continue;
            }
            uint64_t sig = APP->ui_sig ? APP->ui_sig(st.ui) : 0;
            int same_layout = st.sig_valid &&
                st.sig_trows == term_rows() &&
                st.sig_tcols == term_cols() &&
                st.sig_focus == st.focus &&
                st.sig_app == sig &&
                (st.focus != 2 ||
                 st.sig_rows == (APP->alt_len ? APP->alt_len(st.ui) : 0));
            if (same_layout && st.vu_row > 0 && APP->status) {
                /* surgical: rewrite only the status region in place,
                 * leaving the list (and the prompt cursor) untouched */
                printf("\x1b" "7\x1b[%d;1H", st.vu_row);
                APP->status(st.ui, &st, term_cols());
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
        /* pre-parse escape sequences into symbolic keys */
        if (c == 27) {
            int c1 = getchar();
            if (c1 != '[') c = K_ESC;
            else {
                int c2 = getchar();
                if      (c2 == 'A') c = K_UP;
                else if (c2 == 'B') c = K_DOWN;
                else if (c2 == 'C') c = K_RIGHT;
                else if (c2 == 'D') c = K_LEFT;
                else if (c2 == 'H') c = K_HOME;
                else if (c2 == 'F') c = K_END;
                else if (c2 == '3') { getchar(); c = K_DEL; }
                else if (c2 == '5') { getchar(); c = K_PGUP; }
                else if (c2 == '6') { getchar(); c = K_PGDN; }
                else if (c2 == '1') {
                    int c3 = getchar();
                    if (c3 == ';') {
                        int c4 = getchar(), c5 = getchar();
                        if      (c4 == '2' && c5 == 'C') c = K_SRIGHT;
                        else if (c4 == '2' && c5 == 'D') c = K_SLEFT;
                        else continue;
                    } else continue;
                } else continue;
            }
        }
        st.msg[0] = 0; /* feedback lives for one keystroke */
        if (getenv("TAGPLAY_DEBUG")) {
            char m[32];
            snprintf(m, sizeof m, "key %d", c);
            dbglog(m);
        }
        size_t prev = (st.parse_ok ? st.match.len : st.last_good.len);
        const vec *shown = st.parse_ok ? &st.match : &st.last_good;
        if (c == '\t') { /* Tab: cycle query -> list -> alt view -> query */
            size_t alen = APP->alt_len ? APP->alt_len(st.ui) : 0;
            if (st.focus == 0 && shown->len) st.focus = 1;
            else if ((st.focus == 1 || st.focus == 0) && alen) {
                st.focus = 2;
                st.qcur = APP->alt_home ? APP->alt_home(st.ui) : 0;
            } else st.focus = 0;
            redraw(&st, prev);
            continue;
        }
        if (st.focus == 2) { /* ---- alt (app) view ---- */
            int handled = 1;
            if (APP->alt_key && APP->alt_key(st.ui, &st, c)) {
                /* app consumed it (transport, reorder, jump, ...) */
            }
            else if (c == 'j' || c == K_DOWN) {
                if (st.qcur + 1 < st.qview.len) st.qcur++;
            } else if (c == 'k' || c == K_UP) {
                if (st.qcur > 0) st.qcur--;
            } else if (c == 'g') st.qcur = 0;
            else if (c == 'G') st.qcur = st.qview.len ? st.qview.len - 1 : 0;
            else if (c == K_PGUP || c == K_PGDN) {
                int rw = term_rows() - 6;
                if (rw < 1) rw = 1;
                if (c == K_PGDN) {
                    st.qcur += (size_t)rw;
                    if (st.qcur >= st.qview.len)
                        st.qcur = st.qview.len ? st.qview.len - 1 : 0;
                } else st.qcur = st.qcur > (size_t)rw
                               ? st.qcur - (size_t)rw : 0;
            } else if (c == 't') {
                if (st.qview.len)
                    show_track_detail(&st,
                        *(size_t *)vec_at(&st.qview, st.qcur));
            } else if (c == 'a') {
                if (st.qview.len)
                    show_art(&st, *(size_t *)vec_at(&st.qview, st.qcur));
            } else if (c == K_ESC) {
                st.focus = 0;
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
            if (c == 'j' || c == K_DOWN) { if (st.lcur + 1 < shown->len) st.lcur++; }
            else if (c == 'k' || c == K_UP) { if (st.lcur > 0) st.lcur--; }
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
            } else if (c == K_PGUP || c == K_PGDN) {
                int rows = term_rows() - 6;
                if (rows < 1) rows = 1;
                if (c == K_PGDN) {
                    st.lcur += (size_t)rows;
                    if (st.lcur >= shown->len)
                        st.lcur = shown->len ? shown->len - 1 : 0;
                } else {
                    st.lcur = st.lcur > (size_t)rows
                            ? st.lcur - (size_t)rows : 0;
                }
            } else if (c == K_ESC) {
                st.focus = 0; /* bare Esc back to query */
            } else if (c == '\r' || c == '\n') {
                handled = 0;          /* Enter falls through to play */
            } else if (c == ':') {
                /* commands from list mode get a fresh line: the query is
                 * not being edited here, so clearing it is safe */
                st.len = st.cur = 0;
                st.buf[0] = 0;
                rerun(&st);
                st.focus = 0;
                handled = 0;
            } else if (APP->global_key && APP->global_key(st.ui, &st, c)) {
                /* app key (volume, mute, transport, ...) */
            } else if (c >= 32 && c < 127) {
                st.focus = 0;         /* typing returns to the query */
                handled = 0;
            } else handled = 0;
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
                /* Enter: play the selection if any, else current results */
                const vec *q = st.sel.len ? &st.sel
                             : (st.parse_ok ? &st.match : &st.last_good);
                if (q->len && APP->on_enter)
                    APP->on_enter(st.ui, &st, q, st.sel.len > 0);
                /* the query is kept: Tab returns to the same filtered
                 * list, marks in context ('':'' still opens a fresh
                 * command line from list/alt views) */
            }
        } else if (c == 127 || c == 8) { /* backspace */
            st.sel_view = 0;
            if (st.cur > 0) {
                memmove(st.buf + st.cur - 1, st.buf + st.cur, st.len - st.cur);
                st.cur--; st.len--;
                st.buf[st.len] = 0;
                rerun(&st);
            }
        } else if (c == 16 || c == 14 || c == 2) { /* transport ctrl keys */
            if (APP->global_key) APP->global_key(st.ui, &st, c);
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
        } else if (c == K_LEFT)  { if (st.cur > 0) st.cur--; }
        else if (c == K_RIGHT) { if (st.cur < st.len) st.cur++; }
        else if (c == K_HOME)  { st.cur = 0; }
        else if (c == K_END)   { st.cur = st.len; }
        else if (c == K_DEL) {
            if (st.cur < st.len) {
                memmove(st.buf + st.cur, st.buf + st.cur + 1,
                        st.len - st.cur - 1);
                st.len--;
                st.buf[st.len] = 0;
                rerun(&st);
            }
        } else if (c >= 32 && c < 127 && st.len + 1 < sizeof st.buf) {
            st.sel_view = 0;
            memmove(st.buf + st.cur + 1, st.buf + st.cur, st.len - st.cur);
            st.buf[st.cur++] = (char)c;
            st.len++;
            st.buf[st.len] = 0;
            rerun(&st);
        }
        if (!quit) redraw(&st, prev);
    }
    browser_raw_off();
    printf("\n");
    vec_free(&st.match);
    vec_free(&st.last_good);
    vec_free(&st.sel);
    vec_free(&st.qview);
}

void browser_raw_on(void) { (void)raw_on_(); }
