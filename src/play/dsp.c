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

typedef enum { M_OFF, M_TUBE, M_TAPE, M_VINYL, M_SHELLAC,
               M_AM, M_EQ, M_TONE } dsp_mode;

struct dsp_chain {
    pthread_mutex_t mu;

    dsp_mode mode;
    double   amount;
    double   gain;           /* user volume, applied at emission          */
    int      rate, channels;

    /* audiotard stage parameters, derived from mode+amount */
    int          use_shape, use_tape, use_vinyl, use_shellac, use_am;
    int          use_eq, use_tone, os;
    ws_params      wsp;
    tape_params    tp;
    vinyl_params   vp;
    shellac_params shp;
    am_params      ap;
    double eq_db[10];              /* ISO octave bands, dB              */
    double bass_db, treble_db;

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
    size_t   fade_in_left;   /* frames of clean->processed blend on (re)prime */
    size_t   off_fade_left;  /* frames of processed->clean blend while off_pending */
    int      off_pending;    /* "off" requested: fade out, then really switch */

    double  *rbuf, *chan, *chan2;   /* render scratch                     */
    size_t   rcap, chcap;           /* frames                             */
};

/* A live MODE change must not stall the stream: the clean history is
 * mode-independent and the block seam already crossfades, so flipping
 * modes keeps the pipeline hot and only refreshes the RMS match. The
 * full reset below is for format changes and track boundaries, where
 * the history really is invalid. */
static void mode_flip(dsp_chain *c) {
    c->gain_set = 0;
    c->match_gain = HEADROOM;
}

static void pipeline_reset(dsp_chain *c) {
    c->clen = 0;
    c->cbase = c->in_total = c->emitted = c->t = 0;
    c->qlen = c->qrd = 0;
    c->tail_ok = 0;
    c->gain_set = 0;
    c->match_gain = HEADROOM;
    c->fade_in_left = 0;
    c->off_fade_left = 0;
    c->off_pending = 0;
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
    c->use_shellac = c->use_am = c->use_eq = c->use_tone = 0;
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
    case M_SHELLAC:
        c->use_shellac = 1;
        c->shp = SHELLAC_DEFAULTS;
        c->shp.era = s > 1.2 ? 0 : 1;    /* crank it into the horn era */
        c->shp.wow_cents     *= s;
        c->shp.crackle_per_s *= s;
        c->shp.crackle_db += sdb;
        c->shp.hiss_db    += sdb;
        break;
    case M_AM:
        c->use_am = 1;
        c->ap = AM_DEFAULTS;
        c->ap.depth = 0.80 + 0.30 * s;   /* > 1.0: overmodulation fold */
        c->ap.comp  = 0.35 * s;
        if (c->ap.comp > 1.0) c->ap.comp = 1.0;
        c->ap.static_per_s *= s;
        c->ap.static_db += sdb;
        c->ap.hiss_db   += sdb;
        c->ap.fade_db = c->amount > 0.7 ? 10.0 * (c->amount - 0.7) : 0.0;
        break;
    case M_EQ:   c->use_eq = 1; break;   /* bands set via dsp_set_eq   */
    case M_TONE: c->use_tone = 1; break; /* dials via dsp_set_tone     */
    default:
        break;
    }
}

/* the ten ISO octave centers of a classic graphic equalizer */
static const double EQ_FREQ[10] = {
    31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

int dsp_set_eq(dsp_chain *c, const double *db, int n) {
    pthread_mutex_lock(&c->mu);
    int eq_fresh = (c->mode != M_EQ);
    if (eq_fresh) mode_flip(c);
    c->mode = M_EQ;
    for (int i = 0; i < 10; i++) {
        double g = i < n ? db[i] : 0.0;
        if (g > 18) g = 18;
        if (g < -18) g = -18;
        c->eq_db[i] = g;
    }
    derive_params(c);
    if (eq_fresh) c->gain_set = 0;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

int dsp_set_tone(dsp_chain *c, double bass_db, double treble_db) {
    pthread_mutex_lock(&c->mu);
    int tn_fresh = (c->mode != M_TONE);
    if (tn_fresh) mode_flip(c);
    c->mode = M_TONE;
    if (bass_db > 12) bass_db = 12;
    if (bass_db < -12) bass_db = -12;
    if (treble_db > 12) treble_db = 12;
    if (treble_db < -12) treble_db = -12;
    c->bass_db = bass_db;
    c->treble_db = treble_db;
    derive_params(c);
    if (tn_fresh) c->gain_set = 0;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

int dsp_set_mode(dsp_chain *c, const char *mode, double amount) {
    if (amount < 0) amount = 0;
    if (amount > 1) amount = 1;
    dsp_mode m;
    if      (!strcmp(mode, "off"))   m = M_OFF;
    else if (!strcmp(mode, "tube"))  m = M_TUBE;
    else if (!strcmp(mode, "tape"))  m = M_TAPE;
    else if (!strcmp(mode, "vinyl")) m = M_VINYL;
    else if (!strcmp(mode, "shellac")) m = M_SHELLAC;
    else if (!strcmp(mode, "am"))    m = M_AM;
    else return -1;
    pthread_mutex_lock(&c->mu);
    if (m == M_OFF && c->mode != M_OFF && c->mode != M_EQ
        && c->mode != M_TONE && c->qlen > 0 && !c->off_pending) {
        /* keep rendering the old mode; the emission loop fades the
         * output into the clean stream, then completes the switch */
        c->off_pending = 1;
        c->off_fade_left = X_FRAMES;
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    c->off_pending = 0;
    int fresh = (m != c->mode);
    if (fresh) mode_flip(c);
    c->mode = m;
    c->amount = amount;
    derive_params(c);
    /* re-measure the RMS match only on a mode CHANGE: re-measuring on
     * every amount tweak steps the level mid-stream -- an audible
     * click under a slider drag. A slightly stale match during a drag
     * is inaudible; the next mode change refreshes it. */
    if (fresh) c->gain_set = 0;
    pthread_mutex_unlock(&c->mu);
    return 0;
}
const char *dsp_mode_name(const dsp_chain *c) {
    switch (c->mode) {
    case M_TUBE:  return "tube";
    case M_TAPE:  return "tape";
    case M_VINYL: return "vinyl";
    case M_SHELLAC: return "shellac";
    case M_AM:    return "am";
    case M_EQ:    return "eq";
    case M_TONE:  return "tone";
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
    /* corner frequencies must stay below Nyquist: filters designed
     * at or above fs/2 go non-finite (silent NaN at low stream
     * rates -- 22.05/24 kHz news radio). Clamp at the vendor
     * boundary; audiotard gets the same guard upstream. */
    double nyq = 0.45 * fs;
    tape_params  tp_s = c->tp;
    vinyl_params vp_s = c->vp;
    am_params    ap_s = c->ap;
    if (tp_s.lp_hz  > nyq) tp_s.lp_hz  = nyq;
    if (tp_s.bump_hz > nyq) tp_s.bump_hz = nyq;
    if (vp_s.lp_hz  > nyq) vp_s.lp_hz  = nyq;
    if (vp_s.hp_hz  > nyq) vp_s.hp_hz  = nyq;
    if (ap_s.bw_hz  > nyq) ap_s.bw_hz  = nyq;
    if (ap_s.hp_hz  > nyq) ap_s.hp_hz  = nyq;
    uint64_t pr = (c->use_tape || c->use_vinyl ||
                   c->use_shellac || c->use_am) ? PR_MEDIA : PR_SHAPE;
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
            tape_process(c->chan, span, fs, &tp_s, (unsigned)cc, t0))
            return -1;
        if (c->use_vinyl &&
            vinyl_process(c->chan, span, fs, &vp_s, (unsigned)cc, t0))
            return -1;
        if (c->use_shellac &&
            shellac_process(c->chan, span, fs, &c->shp, t0))
            return -1;   /* noise is time-seeded: both channels hiss
                          * identically, as one mono groove should */
        if (c->use_am &&
            am_process(c->chan, span, fs, &ap_s, t0))
            return -1;
        if (c->use_tone) {   /* audiotard's tone dials, verbatim spec */
            biquad q;
            if (fabs(c->bass_db) > 0.01) {
                bq_design(&q, BQ_LOWSHELF, fs, 120.0, 0.7071, c->bass_db);
                bq_process(&q, c->chan, span);
            }
            if (fabs(c->treble_db) > 0.01) {
                bq_design(&q, BQ_HIGHSHELF, fs, 8000.0, 0.7071,
                          c->treble_db);
                bq_process(&q, c->chan, span);
            }
        }
        if (c->use_eq) {
            biquad q;
            for (int b = 0; b < 10; b++) {
                if (fabs(c->eq_db[b]) < 0.01) continue;
                if (EQ_FREQ[b] > 0.45 * fs) continue;
                bq_design(&q, BQ_PEAK, fs, EQ_FREQ[b], 1.414,
                          c->eq_db[b]);
                bq_process(&q, c->chan, span);
            }
        }
        for (size_t i = 0; i < span; i++)
            c->rbuf[i * (size_t)ch + cc] = c->chan[i];
    }

    size_t off = (size_t)(c->t - pre) * (size_t)ch;   /* emit offset */
    size_t nem = B_FRAMES * (size_t)ch;

    /* non-finite scrub over the emission span: a bad filter design

     * or overflow must never poison the stream, the seam tail, or

     * the RMS match below */

    for (size_t i = 0; i < nem; i++)

        if (!isfinite(c->rbuf[off + i])) c->rbuf[off + i] = 0.0;


    if (!c->gain_set && (c->use_eq || c->use_tone)) {
        /* an equalizer's level change IS its function: RMS-matching a
         * +9 dB bass shelf would cancel it into an overall cut. Match
         * audiotard's chain: no normalization around tone/EQ. */
        c->match_gain = 1.0;
        c->gain_set = 1;
    }
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

    int ch = c->channels;
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
    /* while the pipeline primes, pass the clean input through (already
     * in buf) instead of emitting silence, and arm a clean->processed
     * crossfade for the moment real output arrives */
    if (lead) {
        c->fade_in_left = X_FRAMES;
        if (g != 1.0f)
            for (size_t i = 0; i < lead * (size_t)ch; i++) buf[i] *= g;
    }
    const float *src = c->outq + c->qrd * (size_t)ch;
    float *dst = buf + lead * (size_t)ch;
    for (size_t i = 0; i < take; i++) {
        double pw = 1.0;
        if (c->fade_in_left) {
            pw = 1.0 - (double)c->fade_in_left / (double)X_FRAMES;
            c->fade_in_left--;
        }
        if (c->off_pending && c->off_fade_left) {
            /* fade the processed share back to zero: net weight of the
             * processed stream falls, the clean stream (already in
             * dst) rises */
            pw *= (double)c->off_fade_left / (double)X_FRAMES;
            c->off_fade_left--;
        } else if (c->off_pending) pw = 0.0;
        for (int cc = 0; cc < ch; cc++) {
            size_t k = i * (size_t)ch + (size_t)cc;
            dst[k] = (float)(src[k] * pw + dst[k] * (1.0 - pw)) * g;
        }
    }
    c->qrd += take;
    if (c->off_pending && !c->off_fade_left) {
        pipeline_reset(c);           /* fade complete: become truly off */
        c->mode = M_OFF;
        derive_params(c);
    }
    c->emitted += (uint64_t)frames;
    pthread_mutex_unlock(&c->mu);
}

/* ---- the parameter registry ---------------------------------------- */

typedef struct {
    dsp_mode    mode;
    const char *name, *unit;
    double      lo, hi;
    size_t      off;          /* into dsp_chain */
    int         is_int, is_log;
} pdesc;

#define P(m, n, u, lo, hi, field, ii, il) \
    { m, n, u, lo, hi, offsetof(dsp_chain, field), ii, il }

static const pdesc PTAB[] = {
    P(M_TUBE,   "shape",   "",    0,     2,    wsp.shape,       1, 0),
    P(M_TUBE,   "drive",   "",    0.05,  8.0,  wsp.drive,       0, 0),
    P(M_TUBE,   "bias",    "",    0.0,   1.0,  wsp.bias,        0, 0),
    P(M_TUBE,   "h2a",     "",    0.0,   1.5,  wsp.h2,          0, 0),
    P(M_TUBE,   "os",      "x",   1,     4,    os,              1, 0),
    P(M_VINYL,  "wow",     "c",   0.0,  30.0,  vp.wow_cents,    0, 0),
    P(M_VINYL,  "wowrate", "Hz",  0.1,   5.0,  vp.wow_rate,     0, 0),
    P(M_VINYL,  "drift",   "c",   0.0,  30.0,  vp.drift_cents,  0, 0),
    P(M_VINYL,  "crackle", "/s",  0.0,  50.0,  vp.crackle_per_s,0, 0),
    P(M_VINYL,  "crackdb", "dB", -80.0,  0.0,  vp.crackle_db,   0, 0),
    P(M_VINYL,  "hiss",    "dB", -90.0,-20.0,  vp.hiss_db,      0, 0),
    P(M_VINYL,  "lp",      "Hz", 1000, 16000,  vp.lp_hz,        0, 1),
    P(M_VINYL,  "hp",      "Hz",   10,   400,  vp.hp_hz,        0, 1),
    P(M_TAPE,   "wow",     "c",   0.0,  30.0,  tp.wow_cents,    0, 0),
    P(M_TAPE,   "wowrate", "Hz",  0.1,   5.0,  tp.wow_rate,     0, 0),
    P(M_TAPE,   "flutter", "c",   0.0,  20.0,  tp.flutter_cents,0, 0),
    P(M_TAPE,   "flutrate","Hz",  2.0,  20.0,  tp.flutter_rate, 0, 0),
    P(M_TAPE,   "drift",   "c",   0.0,  30.0,  tp.drift_cents,  0, 0),
    P(M_TAPE,   "hiss",    "dB", -90.0,-20.0,  tp.hiss_db,      0, 0),
    P(M_TAPE,   "bump",    "dB",  0.0,  12.0,  tp.bump_db,      0, 0),
    P(M_TAPE,   "bumphz",  "Hz",   20,   300,  tp.bump_hz,      0, 1),
    P(M_TAPE,   "hfloss",  "",    0.0,   1.0,  tp.hf_loss,      0, 0),
    P(M_TAPE,   "lp",      "Hz", 1000, 16000,  tp.lp_hz,        0, 1),
    P(M_SHELLAC,"era",     "",    0,     1,    shp.era,         1, 0),
    P(M_SHELLAC,"wow",     "c",   0.0,  40.0,  shp.wow_cents,   0, 0),
    P(M_SHELLAC,"hiss",    "dB", -70.0,-10.0,  shp.hiss_db,     0, 0),
    P(M_SHELLAC,"crackle", "/s",  0.0, 100.0,  shp.crackle_per_s,0,0),
    P(M_SHELLAC,"crackdb", "dB", -60.0,  0.0,  shp.crackle_db,  0, 0),
    P(M_AM,     "bw",      "Hz", 1500,  8000,  ap.bw_hz,        0, 1),
    P(M_AM,     "hp",      "Hz",   50,   500,  ap.hp_hz,        0, 1),
    P(M_AM,     "depth",   "",    0.2,   1.4,  ap.depth,        0, 0),
    P(M_AM,     "comp",    "",    0.0,   1.0,  ap.comp,         0, 0),
    P(M_AM,     "static",  "/s",  0.0,  20.0,  ap.static_per_s, 0, 0),
    P(M_AM,     "statdb",  "dB", -60.0,  0.0,  ap.static_db,    0, 0),
    P(M_AM,     "hiss",    "dB", -80.0,-20.0,  ap.hiss_db,      0, 0),
    P(M_AM,     "fade",    "dB",  0.0,  30.0,  ap.fade_db,      0, 0),
};
#undef P
#define NPTAB (sizeof PTAB / sizeof PTAB[0])

static dsp_mode mode_of(const char *mode) {
    if (!strcmp(mode, "tube")) return M_TUBE;
    if (!strcmp(mode, "tape")) return M_TAPE;
    if (!strcmp(mode, "vinyl")) return M_VINYL;
    if (!strcmp(mode, "shellac")) return M_SHELLAC;
    if (!strcmp(mode, "am")) return M_AM;
    return M_OFF;
}

static const pdesc *pfind(const char *mode, int i) {
    dsp_mode m = mode_of(mode);
    int k = 0;
    for (size_t j = 0; j < NPTAB; j++)
        if (PTAB[j].mode == m && k++ == i) return &PTAB[j];
    return NULL;
}

int dsp_param_count(const char *mode) {
    dsp_mode m = mode_of(mode);
    int n = 0;
    for (size_t j = 0; j < NPTAB; j++)
        if (PTAB[j].mode == m) n++;
    return n;
}

int dsp_param_info(const char *mode, int i, const char **name,
                   const char **unit, double *lo, double *hi,
                   int *is_int, int *is_log) {
    const pdesc *p = pfind(mode, i);
    if (!p) return -1;
    if (name) *name = p->name;
    if (unit) *unit = p->unit;
    if (lo) *lo = p->lo;
    if (hi) *hi = p->hi;
    if (is_int) *is_int = p->is_int;
    if (is_log) *is_log = p->is_log;
    return 0;
}

int dsp_param_get(dsp_chain *c, const char *mode, int i, double *out) {
    const pdesc *p = pfind(mode, i);
    if (!p || !out) return -1;
    pthread_mutex_lock(&c->mu);
    if (p->is_int) *out = *(int *)((char *)c + p->off);
    else           *out = *(double *)((char *)c + p->off);
    pthread_mutex_unlock(&c->mu);
    return 0;
}

int dsp_param_set(dsp_chain *c, const char *mode, int i, double v) {
    const pdesc *p = pfind(mode, i);
    if (!p) return -1;
    if (v < p->lo) v = p->lo;
    if (v > p->hi) v = p->hi;
    pthread_mutex_lock(&c->mu);
    if (p->is_int) *(int *)((char *)c + p->off) = (int)(v + 0.5);
    else           *(double *)((char *)c + p->off) = v;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

int dsp_param_set_name(dsp_chain *c, const char *name, double v) {
    const char *mode = dsp_mode_name(c);
    int n = dsp_param_count(mode);
    for (int i = 0; i < n; i++) {
        const char *nm;
        if (dsp_param_info(mode, i, &nm, NULL, NULL, NULL, NULL,
                           NULL) == 0 && !strcmp(nm, name))
            return dsp_param_set(c, mode, i, v);
    }
    return -1;
}

double dsp_amount(const dsp_chain *c) { return c->amount; }

/* ---- effect measurements ------------------------------------------ */

/* Hann-windowed single-bin power at frequency f over x[0..n) */
static double bin_power(const float *x, int n, double f, double fs) {
    double cr = 0, ci = 0;
    for (int i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1));
        double ph = 2.0 * M_PI * f * i / fs;
        cr += w * x[i] * cos(ph);
        ci += w * x[i] * sin(ph);
    }
    return cr * cr + ci * ci;
}

/* run frames of a mono-duplicated stereo source through ch, return the
 * last keep samples (left channel) in out[] */
static void probe_run(dsp_chain *ch, int sine, double f0, double fs,
                      float *out, int keep) {
    enum { BLK = 1024 };
    float blk[BLK * 2];
    int total = 70;              /* > pre-roll + settle */
    int have = 0;
    long pos = 0;
    for (int b = 0; b < total; b++) {
        for (int i = 0; i < BLK; i++) {
            float s = sine
                ? 0.5f * (float)sin(2.0 * M_PI * f0 * (pos + i) / fs)
                : 0.0f;
            blk[2 * i] = blk[2 * i + 1] = s;
        }
        pos += BLK;
        dsp_process(ch, blk, BLK);
        for (int i = 0; i < BLK; i++) {
            if (have < keep) out[have++] = blk[2 * i];
            else {
                memmove(out, out + 1, (size_t)(keep - 1) *
                        sizeof *out);
                out[keep - 1] = blk[2 * i];
            }
        }
    }
}

int dsp_measure(dsp_chain *c, dsp_meas *m) {
    if (!m) return -1;
    memset(m, 0, sizeof *m);
    pthread_mutex_lock(&c->mu);
    dsp_mode mode = c->mode;
    double amount = c->amount;
    int rate = c->rate > 0 ? c->rate : 44100;
    ws_params      wsp = c->wsp;
    tape_params    tp  = c->tp;
    vinyl_params   vp  = c->vp;
    shellac_params shp = c->shp;
    am_params      ap  = c->ap;
    double eq[10];
    memcpy(eq, c->eq_db, sizeof eq);
    double bass = c->bass_db, treb = c->treble_db;
    int os = c->os;
    pthread_mutex_unlock(&c->mu);

    m->noise_dbfs = -120.0;
    m->snr_db = 120.0;
    if (mode == M_OFF) return 0;

    dsp_chain *s = dsp_create();
    if (!s) return -1;
    dsp_on_format(s, rate, 2);
    switch (mode) {
    case M_EQ:   dsp_set_eq(s, eq, 10); break;
    case M_TONE: dsp_set_tone(s, bass, treb); break;
    default: {
        const char *nm = mode == M_TUBE ? "tube"
                       : mode == M_TAPE ? "tape"
                       : mode == M_VINYL ? "vinyl"
                       : mode == M_SHELLAC ? "shellac" : "am";
        dsp_set_mode(s, nm, amount);
        pthread_mutex_lock(&s->mu);
        s->wsp = wsp; s->tp = tp; s->vp = vp;
        s->shp = shp; s->ap = ap; s->os = os;
        pthread_mutex_unlock(&s->mu);
        break;
    }
    }

    enum { KEEP = 4096 };
    static float cap[KEEP];       /* UI-thread only, per contract */
    /* an f0 near 1 kHz with an integer number of cycles in KEEP */
    double f0 = floor(1000.0 * KEEP / rate) * (double)rate / KEEP;

    probe_run(s, 1, f0, rate, cap, KEEP);
    double p1 = bin_power(cap, KEEP, f0, rate);
    double ph[5] = { 0 };
    double psum = 0;
    for (int h = 2; h <= 5; h++) {
        if (h * f0 < 0.48 * rate) {
            ph[h - 1] = bin_power(cap, KEEP, h * f0, rate);
            psum += ph[h - 1];
        }
    }
    if (p1 > 1e-20) {
        m->thd_pct = 100.0 * sqrt(psum / p1);
        m->h2_db = ph[1] > 1e-20 ? 10.0 * log10(ph[1] / p1) : -120.0;
        m->h3_db = ph[2] > 1e-20 ? 10.0 * log10(ph[2] / p1) : -120.0;
    }
    double sine_rms = 0;
    for (int i = 0; i < KEEP; i++)
        sine_rms += (double)cap[i] * cap[i];
    sine_rms = sqrt(sine_rms / KEEP);

    probe_run(s, 0, f0, rate, cap, KEEP);
    double nr = 0;
    for (int i = 0; i < KEEP; i++)
        nr += (double)cap[i] * cap[i];
    nr = sqrt(nr / KEEP);
    m->noise_dbfs = nr > 1e-6 ? 20.0 * log10(nr) : -120.0;
    m->snr_db = (nr > 1e-6 && sine_rms > 1e-6)
              ? 20.0 * log10(sine_rms / nr) : 120.0;

    dsp_destroy(s);
    return 0;
}
