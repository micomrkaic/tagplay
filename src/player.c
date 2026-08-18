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

#include "player.h"
#include "decoder.h"
#include <SDL.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define CHUNK_FRAMES 4096

typedef enum { CMD_NONE, CMD_PLAY, CMD_NEXT, CMD_PREV, CMD_STOP, CMD_SEEK, CMD_JUMP } cmd_t;

struct player {
    const table *tb;
    dsp_chain   *dsp;

    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;

    /* command mailbox (protected by mu) */
    cmd_t   cmd;
    double  seek_to;
    size_t  jump_to;
    int     paused;
    int     shutdown;

    /* state written by audio thread (protected by mu) */
    vec     queue;
    size_t  qpos;
    int     playing;        /* 0/1/2 */
    double  pos, dur;
    int     rate, channels;
    size_t  cur_index;
    int     null_output;

    /* VU: post-DSP peak per channel, exponentially decayed */
    double vu[2];
    char   stream_title[256];   /* live ICY StreamTitle, "" if none */
    char   note[160];           /* one-shot UI note (e.g. unplayable station) */
    unsigned note_seq;

    /* output device (SDL). dev is also touched by player_toggle_pause. */
    SDL_AudioDeviceID dev;
    int dev_rate, dev_channels;
    int sdl_ready;
};

/* ---- output: SDL2 (CoreAudio on macOS, ALSA/Pulse on Linux) ----
 * Push model: SDL_QueueAudio + a high-watermark throttle keeps the
 * synchronous decode loop; ~0.5 s of queued audio absorbs jitter. */

#define QUEUE_HIGH_SECONDS 0.5

static int out_open(player *p, int rate, int channels) {
    if (p->dev && p->dev_rate == rate && p->dev_channels == channels)
        return 0; /* gapless: same format, keep device */
    if (p->dev) {
        /* rate change: let the tail drain (bounded), then reopen */
        for (int i = 0; i < 200 && SDL_GetQueuedAudioSize(p->dev) > 0; i++)
            SDL_Delay(10);
        SDL_CloseAudioDevice(p->dev);
        p->dev = 0;
    }
    p->dev_rate = rate;
    p->dev_channels = channels;
    if (p->null_output) return 0;
    if (!p->sdl_ready) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) { p->null_output = 1; return 0; }
        p->sdl_ready = 1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_F32SYS;
    want.channels = (Uint8)channels;
    want.samples = 4096;
    want.callback = NULL; /* queue mode */
    p->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!p->dev) { p->null_output = 1; return 0; }
    SDL_PauseAudioDevice(p->dev, 0);
    return 0;
}

static void vu_update(player *p, const float *buf, long frames) {
    int ch = p->dev_channels > 2 ? 2 : p->dev_channels;
    double pk[2] = { 0, 0 };
    for (long f = 0; f < frames; f++)
        for (int c = 0; c < ch; c++) {
            double a = buf[f * p->dev_channels + c];
            if (a < 0) a = -a;
            if (a > pk[c]) pk[c] = a;
        }
    if (ch == 1) pk[1] = pk[0];
    pthread_mutex_lock(&p->mu);
    for (int c = 0; c < 2; c++)
        p->vu[c] = pk[c] > p->vu[c] * 0.6 ? pk[c] : p->vu[c] * 0.6;
    pthread_mutex_unlock(&p->mu);
}

static void out_write(player *p, const float *buf, long frames) {
    vu_update(p, buf, frames);
    if (p->null_output || !p->dev) {
        struct timespec ts;
        long ns = (long)((double)frames / p->dev_rate * 1e9);
        ts.tv_sec = ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
        nanosleep(&ts, NULL);
        return;
    }
    Uint32 nbytes = (Uint32)(frames * p->dev_channels * (long)sizeof(float));
    if (SDL_QueueAudio(p->dev, buf, nbytes) < 0) { p->null_output = 1; return; }
    Uint32 high = (Uint32)(QUEUE_HIGH_SECONDS * p->dev_rate) *
                  (Uint32)p->dev_channels * (Uint32)sizeof(float);
    while (SDL_GetQueuedAudioSize(p->dev) > high) {
        SDL_Delay(20);
        /* stay responsive: bail if a command, pause, or shutdown arrived
         * (a paused device stops draining -> this loop would spin) */
        pthread_mutex_lock(&p->mu);
        int intr = (p->cmd != CMD_NONE) || p->paused || p->shutdown;
        pthread_mutex_unlock(&p->mu);
        if (intr) break;
    }
}

static void out_flush(player *p) {
    if (p->dev) SDL_ClearQueuedAudio(p->dev);
}

static void out_close(player *p) {
    if (p->dev) {
        SDL_CloseAudioDevice(p->dev);
        p->dev = 0;
    }
}

/* ---- audio thread ---- */
static void *audio_main(void *arg) {
    player *p = arg;
    float *buf = xmalloc(sizeof(float) * CHUNK_FRAMES * 8);
    decoder *dec = NULL;

    pthread_mutex_lock(&p->mu);
    for (;;) {
        /* wait for work */
        while (!p->shutdown && p->cmd == CMD_NONE &&
               (p->playing == 0 || p->paused))
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->shutdown) break;

        /* handle mailbox */
        cmd_t cmd = p->cmd;
        p->cmd = CMD_NONE;
        if (cmd == CMD_PLAY || cmd == CMD_NEXT || cmd == CMD_PREV ||
            cmd == CMD_STOP || cmd == CMD_SEEK || cmd == CMD_JUMP)
            out_flush(p);
        switch (cmd) {
        case CMD_PLAY: /* queue was already swapped in by player_play */
            if (dec) { decoder_close(dec); dec = NULL; }
            p->qpos = 0;
            p->playing = p->queue.len ? 1 : 0;
            p->paused = 0;
            break;
        case CMD_NEXT:
            if (dec) { decoder_close(dec); dec = NULL; }
            if (p->playing && p->qpos + 1 < p->queue.len) p->qpos++;
            else p->playing = 0;
            break;
        case CMD_PREV:
            if (dec) { decoder_close(dec); dec = NULL; }
            if (p->playing && p->qpos > 0) p->qpos--;
            break;
        case CMD_STOP:
            if (dec) { decoder_close(dec); dec = NULL; }
            p->playing = 0;
            p->paused = 0;
            break;
        case CMD_SEEK:
            if (dec) {
                decoder_seek(dec, p->seek_to);
                p->pos = decoder_position(dec);
            }
            break;
        case CMD_JUMP:
            if (p->queue.len) {
                if (dec) { decoder_close(dec); dec = NULL; }
                p->qpos = p->jump_to < p->queue.len ? p->jump_to
                                                    : p->queue.len - 1;
                p->playing = 1;
                p->paused = 0;
            }
            break;
        default:
            break;
        }
        if (!p->playing || p->paused) continue;

        /* open current track if needed */
        if (!dec && p->qpos < p->queue.len) {
            p->stream_title[0] = 0;
            size_t ti = *(size_t *)vec_at(&p->queue, p->qpos);
            const track *t = table_at(p->tb, ti);
            /* show the track being opened immediately (radio opens can
             * take seconds); zero the meters meanwhile */
            p->cur_index = ti;
            p->pos = 0;
            p->dur = 0;
            p->vu[0] = p->vu[1] = 0;
            pthread_mutex_unlock(&p->mu);
            decoder *nd = decoder_open(t->path, t->fmt);
            pthread_mutex_lock(&p->mu);
            if (!nd) { /* unreadable/unplayable: note it and skip */
                const char *ttl = track_first_tag(t, "TITLE");
                snprintf(p->note, sizeof p->note,
                         "can't play: %s%s", ttl ? ttl : t->path,
                         t->fmt == FMT_RADIO ?
                         "  (unreachable or not an MP3 stream)" : "");
                p->note_seq++;
                if (p->qpos + 1 < p->queue.len) p->qpos++;
                else p->playing = 0;
                continue;
            }
            dec = nd;
            p->cur_index = ti;
            p->rate = decoder_rate(dec);
            p->channels = decoder_channels(dec);
            p->dur = decoder_duration(dec);
            p->pos = 0;
            pthread_mutex_unlock(&p->mu);
            out_open(p, p->rate, p->channels);
            dsp_on_format(p->dsp, p->rate, p->channels);
            pthread_mutex_lock(&p->mu);
        }
        if (!dec) continue;

        /* decode+play one chunk outside the lock */
        pthread_mutex_unlock(&p->mu);
        long n = decoder_read(dec, buf, CHUNK_FRAMES);
        if (n > 0) {
            dsp_process(p->dsp, buf, n);
            out_write(p, buf, n);
        } else if (n == DECODER_AGAIN) {
            /* live stream buffering: keep cadence with silence */
            long quiet = 1024;
            memset(buf, 0, sizeof(float) * (size_t)quiet *
                   (size_t)p->dev_channels);
            out_write(p, buf, quiet);
        }
        char lt[256];
        int have_lt = decoder_stream_title(dec, lt, sizeof lt);
        pthread_mutex_lock(&p->mu);
        if (have_lt) snprintf(p->stream_title, sizeof p->stream_title, "%s", lt);
        if (n > 0) {
            p->pos = decoder_position(dec);
        } else if (n == DECODER_AGAIN) {
            /* stay on this track */
        } else { /* end of track (or error): advance */
            decoder_close(dec);
            dec = NULL;
            if (p->qpos + 1 < p->queue.len) p->qpos++;
            else { p->playing = 0; out_close(p); }
        }
    }
    pthread_mutex_unlock(&p->mu);
    if (dec) decoder_close(dec);
    out_close(p);
    free(buf);
    return NULL;
}

/* ---- API ---- */
player *player_create(const table *tb) {
    player *p = xmalloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->tb = tb;
    p->dsp = dsp_create();
    vec_init(&p->queue, sizeof(size_t));
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_create(&p->th, NULL, audio_main, p);
    return p;
}
void player_destroy(player *p) {
    pthread_mutex_lock(&p->mu);
    p->shutdown = 1;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    pthread_join(p->th, NULL);
    if (p->sdl_ready) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    dsp_destroy(p->dsp);
    vec_free(&p->queue);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p);
}
static void post(player *p, cmd_t c) {
    p->cmd = c;
    pthread_cond_signal(&p->cv);
}
void player_play(player *p, const size_t *idx, size_t n) {
    pthread_mutex_lock(&p->mu);
    p->queue.len = 0;
    for (size_t i = 0; i < n; i++) vec_push(&p->queue, &idx[i]);
    /* visible to player_get_queue immediately; audio thread resets on CMD_PLAY */
    if (p->dev) SDL_PauseAudioDevice(p->dev, 0); /* play implies resume */
    post(p, CMD_PLAY);
    pthread_mutex_unlock(&p->mu);
}
void player_toggle_pause(player *p) {
    pthread_mutex_lock(&p->mu);
    if (p->playing) {
        p->paused = !p->paused;
        p->playing = p->paused ? 2 : 1;
        if (p->dev) SDL_PauseAudioDevice(p->dev, p->paused);
        pthread_cond_signal(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
}
void player_next(player *p) {
    pthread_mutex_lock(&p->mu);
    post(p, CMD_NEXT);
    pthread_mutex_unlock(&p->mu);
}
void player_prev(player *p) {
    pthread_mutex_lock(&p->mu);
    post(p, CMD_PREV);
    pthread_mutex_unlock(&p->mu);
}
void player_stop(player *p) {
    pthread_mutex_lock(&p->mu);
    post(p, CMD_STOP);
    pthread_mutex_unlock(&p->mu);
}
void player_move(player *p, size_t from, size_t to) {
    pthread_mutex_lock(&p->mu);
    if (from < p->queue.len && to < p->queue.len && from != to) {
        size_t v = *(size_t *)vec_at(&p->queue, from);
        if (from < to)
            memmove((char *)p->queue.data + from * sizeof(size_t),
                    (char *)p->queue.data + (from + 1) * sizeof(size_t),
                    (to - from) * sizeof(size_t));
        else
            memmove((char *)p->queue.data + (to + 1) * sizeof(size_t),
                    (char *)p->queue.data + to * sizeof(size_t),
                    (from - to) * sizeof(size_t));
        *(size_t *)vec_at(&p->queue, to) = v;
        /* keep qpos pointing at the same (possibly moved) track */
        if (p->qpos == from) p->qpos = to;
        else if (from < p->qpos && to >= p->qpos) p->qpos--;
        else if (from > p->qpos && to <= p->qpos) p->qpos++;
    }
    pthread_mutex_unlock(&p->mu);
}

void player_jump(player *p, size_t queue_index) {
    pthread_mutex_lock(&p->mu);
    p->jump_to = queue_index;
    post(p, CMD_JUMP);
    /* a paused device stops draining; jumping implies resume */
    if (p->dev) SDL_PauseAudioDevice(p->dev, 0);
    pthread_mutex_unlock(&p->mu);
}
void player_get_queue(player *p, vec *out) {
    out->len = 0;
    pthread_mutex_lock(&p->mu);
    for (size_t i = 0; i < p->queue.len; i++)
        vec_push(out, vec_at(&p->queue, i));
    pthread_mutex_unlock(&p->mu);
}
void player_seek(player *p, double sec) {
    pthread_mutex_lock(&p->mu);
    p->seek_to = sec;
    post(p, CMD_SEEK);
    pthread_mutex_unlock(&p->mu);
}
dsp_chain *player_dsp(player *p) { return p->dsp; }
void player_get_status(player *p, player_status *st) {
    pthread_mutex_lock(&p->mu);
    st->playing = p->playing;
    st->queue_pos = p->qpos;
    st->queue_len = p->queue.len;
    st->track_index = p->cur_index;
    st->pos = p->pos;
    st->dur = p->dur;
    st->rate = p->rate;
    st->channels = p->channels;
    st->null_output = p->null_output;
    st->vu_l = p->vu[0];
    st->vu_r = p->vu[1];
    snprintf(st->stream_title, sizeof st->stream_title, "%s", p->stream_title);
    snprintf(st->note, sizeof st->note, "%s", p->note);
    st->note_seq = p->note_seq;
    if (p->playing != 1) { st->vu_l = st->vu_r = 0; }
    pthread_mutex_unlock(&p->mu);
}
