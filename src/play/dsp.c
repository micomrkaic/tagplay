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

/* dsp.c — Audiotard wired into tagplay's playback path.
 *
 * Processing uses audiotard's effects.c/engine.c verbatim (GPL-3, same
 * author). Streaming follows audiotard's own producer recipe (wasm
 * worker / GTK live mode): render B-frame blocks with PR frames of
 * pre-roll context so filters, delay lines and noise envelopes settle
 * on real signal; discard the pre-roll; crossfade X frames at seams;
 * pad the render past the emitted region because the last ~140 samples
 * of a render are FIR-edge-corrupted; keep wow/flutter phase-continuous
 * across blocks via the buffer start time t0; apply one constant
 * RMS-match gain (x0.708 headroom) measured on the first block instead
 * of per-block trims that would pump.
 *
 * The cost of causal streaming is latency: a frame can only be emitted
 * once its block (plus lookahead) has been rendered, so output lags
 * input by up to B+X+PAD frames (~116 ms at 44.1 kHz). The pipeline
 * emits silence while priming; the SDL queue absorbs the rest. State
 * persists across track boundaries (dsp_on_format resets only on a real
 * rate/channel change), so the tape keeps rolling through gapless joins.
 */
#include "dsp.h"
#include "util.h"
#include "effects.h"
#include "engine.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>

#define B_FRAMES   4096      /* emitted per render                       */
#define X_FRAMES   512       /* seam crossfade                           */
#define PAD_FRAMES 512       /* FIR edge-corruption guard                */
#define PR_MEDIA   16384     /* pre-roll: vinyl/tape need long settling  */
#define PR_SHAPE   2048      /* shaper-only: little more than FIR warmup */
#define HEADROOM   0.708     /* -3 dB: hot masters + harmonics headroom  */

typedef enum { M_OFF, M_TUBE, M_TAPE, M_VINYL } dsp_mode;

struct dsp_chain {
    pthread_mutex_t mu;

    dsp_mode mode;
    double   amount;
    double   gain;           /* user volume, applied at emission          */
    int      rate, channels;

    /* audiotard stage parameters, derived from mode+amount */
    int          use_shape, use_tape, use_vinyl, os;
    ws_params    wsp;
    tape_params  tp;
    vinyl_params vp;

    /* ---- streaming pipeline (absolute frame positions) ---- */
    double  *clean;          /* interleaved history, clean[0] = frame cbase */
    size_t   ccap, clen;     /* in frames */
    uint64_t cbase;
    uint64_t in_total;       /* frames received                           */
    uint64_t emitted;        /* frames emitted                            */
    uint64_t t;              /* next block start                          */

    float   *outq;           /* processed FIFO, interleaved               */
    size_t   qcap, qlen, qrd;/* frames                                    */

    double  *tail;           /* X_FRAMES * ch, gain-applied               */
    int      tail_ok;

    double   match_gain;
    int      gain_set;

    double  *rbuf, *chan, *chan2;   /* render scratch                     */
    size_t   rcap, chcap;           /* frames                             */
};

static void pipeline_reset(dsp_chain *c) {
    c->clen = 0;
    c->cbase = c->in_total = c->emitted = c->t = 0;
    c->qlen = c->qrd = 0;
    c->tail_ok = 0;
    c->gain_set = 0;
    c->match_gain = HEADROOM;
}

dsp_chain *dsp_create(void) {
    dsp_chain *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    c->gain = 1.0;
    c->os = 8;
    pipeline_reset(c);
    return c;
}
void dsp_destroy(dsp_chain *c) {
    free(c->clean); free(c->outq); free(c->tail);
    free(c->rbuf); free(c->chan); free(c->chan2);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

void dsp_on_format(dsp_chain *c, int rate, int channels) {
    pthread_mutex_lock(&c->mu);
    if (rate != c->rate || channels != c->channels) {
        c->rate = rate;
        c->channels = channels;
        pipeline_reset(c);   /* real format change: fresh pipeline        */
        free(c->tail);
        c->tail = xmalloc(sizeof(double) * X_FRAMES * (size_t)channels);
    }
    /* same format (gapless track join): keep everything rolling */
    pthread_mutex_unlock(&c->mu);
}

/* amount in [0,1]; 0.5 == audiotard's calibrated defaults */
static void derive_params(dsp_chain *c) {
    double s = 2.0 * c->amount;              /* 1.0 at the defaults      */
    double sdb = s > 0.001 ? 20.0 * log10(s) : -120.0;
    c->use_shape = c->use_tape = c->use_vinyl = 0;
    switch (c->mode) {
    case M_TUBE:
        c->use_shape = 1;
        c->wsp = (ws_params){ .shape = WS_TUBE, .drive = 2.0 * s, .bias = 0.2 };
        if (c->wsp.drive < 0.05) c->wsp.drive = 0.05;
        break;
    case M_TAPE:
        c->use_tape = 1;
        c->tp = TAPE_DEFAULTS;
        c->tp.wow_cents     *= s;
        c->tp.flutter_cents *= s;
        c->tp.drift_cents   *= s;
        c->tp.hf_loss       *= s;
        if (c->tp.hf_loss > 1.0) c->tp.hf_loss = 1.0;
        c->tp.hiss_db += sdb;
        break;
    case M_VINYL:
        c->use_vinyl = 1;
        c->vp = VINYL_DEFAULTS;
        c->vp.wow_cents     *= s;
        c->vp.drift_cents   *= s;
        c->vp.crackle_per_s *= s;
        c->vp.crackle_db += sdb;
        c->vp.hiss_db    += sdb;
        break;
    default:
        break;
    }
}

int dsp_set_mode(dsp_chain *c, const char *mode, double amount) {
    if (amount < 0) amount = 0;
    if (amount > 1) amount = 1;
    dsp_mode m;
    if      (!strcmp(mode, "off"))   m = M_OFF;
    else if (!strcmp(mode, "tube"))  m = M_TUBE;
    else if (!strcmp(mode, "tape"))  m = M_TAPE;
    else if (!strcmp(mode, "vinyl")) m = M_VINYL;
    else return -1;
    pthread_mutex_lock(&c->mu);
    if (m != c->mode) pipeline_reset(c);
    c->mode = m;
    c->amount = amount;
    derive_params(c);
    c->gain_set = 0;         /* re-measure the RMS match at next block    */
    pthread_mutex_unlock(&c->mu);
    return 0;
}
const char *dsp_mode_name(const dsp_chain *c) {
    switch (c->mode) {
    case M_TUBE:  return "tube";
    case M_TAPE:  return "tape";
    case M_VINYL: return "vinyl";
    default:      return "off";
    }
}
void dsp_set_gain(dsp_chain *c, double g) {
    if (g < 0) g = 0;
    if (g > 2) g = 2;
    c->gain = g;
}
double dsp_gain(const dsp_chain *c) { return c->gain; }

/* ---- pipeline internals ---- */
static void clean_append(dsp_chain *c, const float *buf, long frames) {
    int ch = c->channels;
    /* compact: history older than t - PR is never needed again */
    uint64_t keep_from = c->t > PR_MEDIA ? c->t - PR_MEDIA : 0;
    if (keep_from > c->cbase) {
        size_t drop = (size_t)(keep_from - c->cbase);
        if (drop > c->clen) drop = c->clen;
        memmove(c->clean, c->clean + drop * (size_t)ch,
                (c->clen - drop) * (size_t)ch * sizeof(double));
        c->clen -= drop;
        c->cbase += drop;
    }
    if (c->clen + (size_t)frames > c->ccap) {
        c->ccap = (c->clen + (size_t)frames) * 2;
        c->clean = xrealloc(c->clean, c->ccap * (size_t)ch * sizeof(double));
    }
    double *dst = c->clean + c->clen * (size_t)ch;
    for (long i = 0; i < frames * ch; i++) dst[i] = (double)buf[i];
    c->clen += (size_t)frames;
    c->in_total += (uint64_t)frames;
}

static void outq_push(dsp_chain *c, const double *frames_in, size_t nframes) {
    int ch = c->channels;
    if (c->qrd) { /* compact consumed head */
        memmove(c->outq, c->outq + c->qrd * (size_t)ch,
                (c->qlen - c->qrd) * (size_t)ch * sizeof(float));
        c->qlen -= c->qrd;
        c->qrd = 0;
    }
    if (c->qlen + nframes > c->qcap) {
        c->qcap = (c->qlen + nframes) * 2;
        c->outq = xrealloc(c->outq, c->qcap * (size_t)ch * sizeof(float));
    }
    float *dst = c->outq + c->qlen * (size_t)ch;
    for (size_t i = 0; i < nframes * (size_t)ch; i++)
        dst[i] = (float)frames_in[i];
    c->qlen += nframes;
}

static int render_block(dsp_chain *c) {
    int ch = c->channels;
    double fs = (double)c->rate;
    uint64_t pr = (c->use_tape || c->use_vinyl) ? PR_MEDIA : PR_SHAPE;
    uint64_t pre  = c->t > pr ? c->t - pr : 0;
    if (pre < c->cbase) pre = c->cbase;
    uint64_t endr = c->t + B_FRAMES + X_FRAMES + PAD_FRAMES;
    if (endr > c->cbase + c->clen) return -1; /* shouldn't happen */
    size_t span = (size_t)(endr - pre);

    if (span > c->rcap) {
        c->rcap = span * 2;
        c->rbuf = xrealloc(c->rbuf, c->rcap * (size_t)ch * sizeof(double));
    }
    if (span > c->chcap) {
        c->chcap = span * 2;
        c->chan  = xrealloc(c->chan,  c->chcap * sizeof(double));
        c->chan2 = xrealloc(c->chan2, c->chcap * sizeof(double));
    }
    memcpy(c->rbuf, c->clean + (size_t)(pre - c->cbase) * (size_t)ch,
           span * (size_t)ch * sizeof(double));

    double t0 = (double)pre / fs;
    for (int cc = 0; cc < ch; cc++) {
        for (size_t i = 0; i < span; i++)
            c->chan[i] = c->rbuf[i * (size_t)ch + cc];
        if (c->use_shape) {
            if (ws_process(c->chan, c->chan2, span, fs, c->os, &c->wsp))
                return -1;
            memcpy(c->chan, c->chan2, span * sizeof(double));
        }
        if (c->use_tape &&
            tape_process(c->chan, span, fs, &c->tp, (unsigned)cc, t0))
            return -1;
        if (c->use_vinyl &&
            vinyl_process(c->chan, span, fs, &c->vp, (unsigned)cc, t0))
            return -1;
        for (size_t i = 0; i < span; i++)
            c->rbuf[i * (size_t)ch + cc] = c->chan[i];
    }

    size_t off = (size_t)(c->t - pre) * (size_t)ch;   /* emit offset */
    size_t nem = B_FRAMES * (size_t)ch;

    if (!c->gain_set) { /* once: match block RMS to clean, minus headroom */
        const double *cl = c->clean + (size_t)(c->t - c->cbase) * (size_t)ch;
        double rs = 0, ro = 0;
        for (size_t i = 0; i < nem; i++) {
            rs += cl[i] * cl[i];
            ro += c->rbuf[off + i] * c->rbuf[off + i];
        }
        c->match_gain = (ro > 1e-12 ? sqrt(rs / ro) : 1.0) * HEADROOM;
        c->gain_set = 1;
    }
    for (size_t i = 0; i < nem; i++) c->rbuf[off + i] *= c->match_gain;

    if (c->tail_ok) { /* crossfade the seam against the previous tail */
        for (size_t i = 0; i < X_FRAMES; i++) {
            double w = (double)i / X_FRAMES;
            for (int cc = 0; cc < ch; cc++) {
                size_t k = i * (size_t)ch + (size_t)cc;
                c->rbuf[off + k] = c->tail[k] * (1.0 - w)
                                 + c->rbuf[off + k] * w;
            }
        }
    }
    /* stash the next seam's tail from the render interior (pre-pad) */
    for (size_t i = 0; i < X_FRAMES * (size_t)ch; i++)
        c->tail[i] = c->rbuf[off + nem + i] * c->match_gain;
    c->tail_ok = 1;

    outq_push(c, c->rbuf + off, B_FRAMES);
    c->t += B_FRAMES;
    return 0;
}

void dsp_process(dsp_chain *c, float *buf, long frames) {
    pthread_mutex_lock(&c->mu);
    float g = (float)c->gain;
    long n = frames * c->channels;

    if (c->mode == M_OFF || c->rate <= 0) {
        if (c->clen || c->qlen) pipeline_reset(c); /* lazily drop pipeline */
        if (g != 1.0f)
            for (long i = 0; i < n; i++) buf[i] *= g;
        pthread_mutex_unlock(&c->mu);
        return;
    }

    clean_append(c, buf, frames);
    while (c->cbase + c->clen >= c->t + B_FRAMES + X_FRAMES + PAD_FRAMES)
        if (render_block(c)) break;

    /* emit: silence while priming, then the processed stream (delayed) */
    size_t avail = c->qlen - c->qrd;
    size_t take = (size_t)frames < avail ? (size_t)frames : avail;
    size_t lead = (size_t)frames - take;   /* only during priming */
    memset(buf, 0, lead * (size_t)c->channels * sizeof(float));
    const float *src = c->outq + c->qrd * (size_t)c->channels;
    float *dst = buf + lead * (size_t)c->channels;
    for (size_t i = 0; i < take * (size_t)c->channels; i++)
        dst[i] = src[i] * g;
    c->qrd += take;
    c->emitted += (uint64_t)frames;
    pthread_mutex_unlock(&c->mu);
}
