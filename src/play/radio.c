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

/* radio.c — ICY/Icecast stream transport.
 *
 * A curl thread fills a byte ring; the consumer (the radio decoder in
 * decoder.c) drains clean MP3 bytes. ICY metadata interleaving
 * (icy-metaint) is stripped here, and StreamTitle is published for the
 * status line. Both proper HTTP Icecast responses and legacy
 * "ICY 200 OK" (HTTP/0.9-style) SHOUTcast responses are handled: for
 * the latter, headers arrive in the body and are parsed inline.
 */
#include "radio.h"
#include "util.h"
#include <curl/curl.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

#define RING_CAP   (512 * 1024)   /* ~30 s at 128 kbps */
#define RING_HIGH  (RING_CAP - 65536)

typedef enum { H_MAYBE_INLINE, H_INLINE, H_DONE } hdr_state;

struct radio_stream {
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    CURL           *curl;

    /* ring of de-ICY'd audio bytes */
    uint8_t *ring;
    size_t   rcap, rlen, rrd;

    /* ICY state */
    long   metaint;        /* 0 = no interleaving */
    long   until_meta;     /* audio bytes until next metadata block */
    long   meta_need;      /* bytes of current metadata block pending */
    char   metabuf[4096];
    size_t metalen;
    char   title[256];
    char   station[128];
    char   ctype[64];      /* Content-Type, lowercased */

    /* inline (ICY 200 OK) header parsing */
    hdr_state hstate;
    char   hbuf[8192];
    size_t hlen;

    /* playlist indirection: many mounts answer with a tiny m3u/pls TEXT
     * file naming the real stream. Detect it from the first body bytes,
     * collect it, extract the first URL, and transparently restart. */
    int    pl_mode;        /* -1 unknown, 0 audio, 1 collecting playlist */
    char   plbuf[8192];
    size_t pllen;
    char   next_url[1024]; /* set when a playlist named the real stream */

    int    error;          /* transfer ended or failed */
    int    stop;
};

static void publish_meta(radio_stream *r) {
    /* metabuf: "StreamTitle='...';StreamUrl='...';" */
    r->metabuf[r->metalen] = 0;
    const char *k = strstr(r->metabuf, "StreamTitle='");
    if (!k) return;
    k += 13;
    const char *e = strstr(k, "';");
    if (!e) e = k + strlen(k);
    size_t n = (size_t)(e - k);
    if (n >= sizeof r->title) n = sizeof r->title - 1;
    memcpy(r->title, k, n);
    r->title[n] = 0;
}

static void hdr_line(radio_stream *r, const char *line, size_t n) {
    if (n > 12 && !strncasecmp(line, "icy-metaint:", 12)) {
        r->metaint = atol(line + 12);
        r->until_meta = r->metaint;
    } else if (n > 13 && !strncasecmp(line, "content-type:", 13)) {
        const char *v = line + 13;
        while (*v == ' ') v++;
        size_t m = n - (size_t)(v - line);
        if (m >= sizeof r->ctype) m = sizeof r->ctype - 1;
        for (size_t i = 0; i < m; i++)
            r->ctype[i] = (char)tolower((unsigned char)v[i]);
        r->ctype[m] = 0;
        r->ctype[strcspn(r->ctype, "\r\n")] = 0;
    } else if (n > 9 && !strncasecmp(line, "icy-name:", 9)) {
        const char *v = line + 9;
        while (*v == ' ') v++;
        snprintf(r->station, sizeof r->station, "%.*s",
                 (int)(n - (size_t)(v - line)), v);
        size_t sl = strlen(r->station);
        while (sl && (r->station[sl-1] == '\r' || r->station[sl-1] == '\n'))
            r->station[--sl] = 0;
    }
}

/* proper HTTP responses: curl gives us headers here */
static size_t on_header(char *buf, size_t sz, size_t nm, void *ud) {
    radio_stream *r = ud;
    pthread_mutex_lock(&r->mu);
    hdr_line(r, buf, sz * nm);
    r->hstate = H_DONE; /* body will be clean */
    pthread_mutex_unlock(&r->mu);
    return sz * nm;
}

/* push de-ICY'd audio bytes into the ring (mu held) */
static void ring_push(radio_stream *r, const uint8_t *p, size_t n) {
    while (n) {
        if (r->rlen == r->rcap) return; /* overflow: drop (caller throttles) */
        size_t w = (r->rrd + r->rlen) % r->rcap;
        size_t run = r->rcap - w;
        if (run > r->rcap - r->rlen) run = r->rcap - r->rlen;
        if (run > n) run = n;
        memcpy(r->ring + w, p, run);
        r->rlen += run;
        p += run;
        n -= run;
    }
}

/* feed body bytes through the ICY splitter (mu held) */
static void icy_feed(radio_stream *r, const uint8_t *p, size_t n) {
    while (n) {
        if (r->meta_need > 0) {
            size_t take = (size_t)r->meta_need < n ? (size_t)r->meta_need : n;
            size_t room = sizeof r->metabuf - 1 - r->metalen;
            size_t keep = take < room ? take : room;
            memcpy(r->metabuf + r->metalen, p, keep);
            r->metalen += keep;
            r->meta_need -= (long)take;
            p += take;
            n -= take;
            if (r->meta_need == 0) {
                publish_meta(r);
                r->metalen = 0;
                r->until_meta = r->metaint;
            }
            continue;
        }
        if (r->metaint > 0 && r->until_meta == 0) {
            r->meta_need = (long)p[0] * 16;
            p++;
            n--;
            if (r->meta_need == 0) r->until_meta = r->metaint;
            continue;
        }
        size_t take = n;
        if (r->metaint > 0 && (long)take > r->until_meta)
            take = (size_t)r->until_meta;
        ring_push(r, p, take);
        if (r->metaint > 0) r->until_meta -= (long)take;
        p += take;
        n -= take;
    }
}

static size_t on_body(char *buf, size_t sz, size_t nm, void *ud) {
    radio_stream *r = ud;
    const uint8_t *p = (const uint8_t *)buf;
    size_t n = sz * nm;
    pthread_mutex_lock(&r->mu);
    if (r->stop) { pthread_mutex_unlock(&r->mu); return 0; /* abort */ }

    if (r->hstate == H_MAYBE_INLINE) {
        /* legacy servers put "ICY 200 OK\r\nheaders\r\n\r\n" in the body */
        r->hstate = (n >= 4 && !memcmp(p, "ICY ", 4)) ? H_INLINE : H_DONE;
    }
    if (r->hstate == H_INLINE) {
        size_t keep = n < sizeof r->hbuf - 1 - r->hlen
                    ? n : sizeof r->hbuf - 1 - r->hlen;
        memcpy(r->hbuf + r->hlen, p, keep);
        r->hlen += keep;
        r->hbuf[r->hlen] = 0;
        char *end = strstr(r->hbuf, "\r\n\r\n");
        if (!end) { pthread_mutex_unlock(&r->mu); return sz * nm; }
        /* parse header lines */
        for (char *l = r->hbuf; l < end; ) {
            char *nl = strstr(l, "\r\n");
            if (!nl) break;
            hdr_line(r, l, (size_t)(nl - l));
            l = nl + 2;
        }
        size_t consumed_now = n - (r->hlen - (size_t)(end + 4 - r->hbuf));
        (void)consumed_now;
        /* remaining body bytes after the blank line belong to audio */
        size_t body_off = (size_t)(end + 4 - r->hbuf);
        size_t audio_in_hbuf = r->hlen - body_off;
        r->hstate = H_DONE;
        icy_feed(r, (const uint8_t *)r->hbuf + body_off, audio_in_hbuf);
        r->hlen = 0;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
        return sz * nm;
    }

    if (r->pl_mode < 0) {
        /* classify: playlists are short text ("#EXTM3U", "[playlist]",
         * or a bare http URL); audio starts with ID3/MP3 sync/ICY */
        r->pl_mode = 0;
        if (n >= 7 && !memcmp(p, "#EXTM3U", 7)) r->pl_mode = 1;
        else if (n >= 10 && !strncasecmp((const char *)p, "[playlist]", 10)) r->pl_mode = 1;
        else if (n >= 7 && !memcmp(p, "http://", 7)) r->pl_mode = 1;
        else if (n >= 8 && !memcmp(p, "https://", 8)) r->pl_mode = 1;
    }
    if (r->pl_mode == 1) {
        size_t keep = n < sizeof r->plbuf - 1 - r->pllen
                    ? n : sizeof r->plbuf - 1 - r->pllen;
        memcpy(r->plbuf + r->pllen, p, keep);
        r->pllen += keep;
        pthread_mutex_unlock(&r->mu);
        return sz * nm;   /* consume; resolution happens at transfer end */
    }

    /* throttle: if the ring is full, wait for the consumer */
    while (!r->stop && r->rlen > RING_HIGH) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&r->cv, &r->mu, &ts);
    }
    if (!r->stop) icy_feed(r, p, n);
    pthread_cond_broadcast(&r->cv);
    int stop = r->stop;
    pthread_mutex_unlock(&r->mu);
    return stop ? 0 : sz * nm;
}

static void extract_playlist_url(radio_stream *r) {
    r->plbuf[r->pllen] = 0;
    for (char *l = strtok(r->plbuf, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        while (*l == ' ' || *l == '\t') l++;
        const char *u = NULL;
        if (!strncasecmp(l, "File", 4)) {         /* pls: FileN=url */
            const char *eq = strchr(l, '=');
            if (eq) u = eq + 1;
        } else if (!strncmp(l, "http://", 7) || !strncmp(l, "https://", 8)) {
            u = l;                                 /* m3u: bare URL line */
        }
        if (u && *u) {
            snprintf(r->next_url, sizeof r->next_url, "%s", u);
            return;
        }
    }
}

static void *curl_main(void *ud) {
    radio_stream *r = ud;
    CURLcode rc = curl_easy_perform(r->curl);
    (void)rc;
    pthread_mutex_lock(&r->mu);
    if (r->pl_mode == 1 && r->pllen && !r->stop)
        extract_playlist_url(r);
    r->error = 1;             /* ended (or: playlist collected) */
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

static radio_stream *radio_open_one(const char *url);

radio_stream *radio_open(const char *url) {
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 3; hop++) {
        radio_stream *r = radio_open_one(cur);
        if (!r) return NULL;
        /* if the mount answered with a playlist, the curl thread ends
         * quickly with next_url set; wait briefly for that verdict */
        pthread_mutex_lock(&r->mu);
        for (int i = 0; i < 60 && !r->error && r->rlen == 0; i++) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&r->cv, &r->mu, &ts);
        }
        int redirect = (r->next_url[0] != 0);
        if (redirect) snprintf(cur, sizeof cur, "%s", r->next_url);
        pthread_mutex_unlock(&r->mu);
        if (!redirect) return r;      /* real audio (or a real failure) */
        radio_close(r);               /* follow the playlist's URL */
    }
    return NULL;
}

static radio_stream *radio_open_one(const char *url) {
    static int curl_init_done;
    if (!curl_init_done) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_init_done = 1;
    }
    radio_stream *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    r->ring = xmalloc(RING_CAP);
    r->rcap = RING_CAP;
    r->hstate = H_MAYBE_INLINE;
    r->pl_mode = -1;

    r->curl = curl_easy_init();
    if (!r->curl) { radio_close(r); return NULL; }
    struct curl_slist *hdrs = curl_slist_append(NULL, "Icy-MetaData: 1");
    curl_easy_setopt(r->curl, CURLOPT_URL, url);
    curl_easy_setopt(r->curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(r->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(r->curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(r->curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(r->curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(r->curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(r->curl, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(r->curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(r->curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
    curl_easy_setopt(r->curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(r->curl, CURLOPT_USERAGENT, "tagplay/0.2");
#ifdef CURLOPT_HTTP09_ALLOWED
    curl_easy_setopt(r->curl, CURLOPT_HTTP09_ALLOWED, 1L);
#endif
    pthread_create(&r->th, NULL, curl_main, r);
    return r;
}

long radio_read(radio_stream *r, uint8_t *buf, size_t max, int timeout_ms) {
    pthread_mutex_lock(&r->mu);
    while (r->rlen == 0 && !r->error) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&r->cv, &r->mu, &ts)) break;
    }
    if (r->rlen == 0) {
        int err = r->error;
        pthread_mutex_unlock(&r->mu);
        return err ? -1 : 0;   /* 0 = no data yet, try again */
    }
    size_t take = max < r->rlen ? max : r->rlen;
    size_t run1 = r->rcap - r->rrd;
    if (run1 > take) run1 = take;
    memcpy(buf, r->ring + r->rrd, run1);
    if (take > run1) memcpy(buf + run1, r->ring, take - run1);
    r->rrd = (r->rrd + take) % r->rcap;
    r->rlen -= take;
    pthread_cond_broadcast(&r->cv); /* wake a throttled producer */
    pthread_mutex_unlock(&r->mu);
    return (long)take;
}

const char *radio_content_type(radio_stream *r) {
    return r->ctype;
}

int radio_title(radio_stream *r, char *out, size_t sz) {
    pthread_mutex_lock(&r->mu);
    int have = r->title[0] != 0;
    if (have) snprintf(out, sz, "%s", r->title);
    pthread_mutex_unlock(&r->mu);
    return have;
}

void radio_close(radio_stream *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->stop = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    if (r->curl) {
        pthread_join(r->th, NULL);
        curl_easy_cleanup(r->curl);
    }
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->cv);
    free(r->ring);
    free(r);
}
