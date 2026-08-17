/* This file is part of audiotard.
 *
 * audiotard -- calibrated audio distortions with blind listening tests
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

#include "effects.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ====================================================================== */
/* RBJ biquads                                                            */
/* ====================================================================== */

void bq_design(biquad *q, bq_type t, double fs, double f,
               double Q, double gain_db)
{
    double A  = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f / fs;
    double cw = cos(w0), sw = sin(w0);
    double al = sw / (2.0 * Q);
    double b0, b1, b2, a0, a1, a2;

    switch (t) {
    case BQ_PEAK:
        b0 = 1.0 + al * A;  b1 = -2.0 * cw;      b2 = 1.0 - al * A;
        a0 = 1.0 + al / A;  a1 = -2.0 * cw;      a2 = 1.0 - al / A;
        break;
    case BQ_LOWSHELF: {
        double s = 2.0 * sqrt(A) * al;
        b0 =       A * ((A + 1) - (A - 1) * cw + s);
        b1 = 2.0 * A * ((A - 1) - (A + 1) * cw);
        b2 =       A * ((A + 1) - (A - 1) * cw - s);
        a0 =            (A + 1) + (A - 1) * cw + s;
        a1 =     -2.0 * ((A - 1) + (A + 1) * cw);
        a2 =            (A + 1) + (A - 1) * cw - s;
        break;
    }
    case BQ_HIGHSHELF: {
        double s = 2.0 * sqrt(A) * al;
        b0 =        A * ((A + 1) + (A - 1) * cw + s);
        b1 = -2.0 * A * ((A - 1) + (A + 1) * cw);
        b2 =        A * ((A + 1) + (A - 1) * cw - s);
        a0 =             (A + 1) - (A - 1) * cw + s;
        a1 =      2.0 * ((A - 1) - (A + 1) * cw);
        a2 =             (A + 1) - (A - 1) * cw - s;
        break;
    }
    case BQ_LOWPASS:
        b0 = (1.0 - cw) / 2.0;  b1 = 1.0 - cw;   b2 = (1.0 - cw) / 2.0;
        a0 = 1.0 + al;          a1 = -2.0 * cw;  a2 = 1.0 - al;
        break;
    case BQ_HIGHPASS:
        b0 = (1.0 + cw) / 2.0;  b1 = -(1.0 + cw); b2 = (1.0 + cw) / 2.0;
        a0 = 1.0 + al;          a1 = -2.0 * cw;   a2 = 1.0 - al;
        break;
    case BQ_BANDPASS:            /* constant 0 dB peak gain */
    default:
        b0 = al;                b1 = 0.0;        b2 = -al;
        a0 = 1.0 + al;          a1 = -2.0 * cw;  a2 = 1.0 - al;
        break;
    }
    q->b0 = b0 / a0;  q->b1 = b1 / a0;  q->b2 = b2 / a0;
    q->a1 = a1 / a0;  q->a2 = a2 / a0;
    q->s1 = q->s2 = 0.0;
}

double bq_tick(biquad *q, double x)
{
    double y = q->b0 * x + q->s1;
    q->s1 = q->b1 * x - q->a1 * y + q->s2;
    q->s2 = q->b2 * x - q->a2 * y;
    return y;
}

void bq_process(biquad *q, double *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = bq_tick(q, buf[i]);
}

/* ====================================================================== */
/* PRNG + noise generators                                                */
/* ====================================================================== */

static double frand(uint64_t *s)          /* uniform [0,1) */
{
    uint64_t x = *s;
    x ^= x >> 12;  x ^= x << 25;  x ^= x >> 27;
    *s = x;
    return (double)((x * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}
static double frand2(uint64_t *s) { return 2.0 * frand(s) - 1.0; }

/* Paul Kellet's economy pink filter (white in, ~-3 dB/oct out). */
typedef struct { double b0, b1, b2; } pink_state;

static double pink_tick(pink_state *p, double w)
{
    p->b0 = 0.99765 * p->b0 + w * 0.0990460;
    p->b1 = 0.96300 * p->b1 + w * 0.2965164;
    p->b2 = 0.57000 * p->b2 + w * 1.0526913;
    return p->b0 + p->b1 + p->b2 + w * 0.1848;
}

/* Empirical RMS of the pink generator driven by unit-uniform white, so
 * hiss_db means what it says. Deterministic, computed once.             */
static double pink_rms_ref(void)
{
    static double ref = 0.0;
    if (ref == 0.0) {
        pink_state ps = {0};
        uint64_t   s  = 0x243f6a8885a308d3ULL;
        double acc = 0.0;
        const int N = 1 << 16;
        for (int i = 0; i < N; i++) {
            double v = pink_tick(&ps, frand2(&s));
            acc += v * v;
        }
        ref = sqrt(acc / N);
    }
    return ref;
}

/* ====================================================================== */
/* Modulated fractional delay (wow / flutter / drift)                     */
/* ====================================================================== */

/* Pitch deviation of 'cents' at modulation rate 'hz' needs a delay-mod
 * amplitude (in samples) of A = r / (2*pi*hz/fs), where r = 2^(c/1200)-1:
 * instantaneous rate ratio is 1 - d'(t).                                 */
static double mod_amp_samples(double cents, double hz, double fs)
{
    if (cents <= 0.0 || hz <= 0.0) return 0.0;
    double r = pow(2.0, cents / 1200.0) - 1.0;
    return r * fs / (2.0 * M_PI * hz);
}

typedef struct {
    double  *buf;
    size_t   mask;        /* len - 1, len = power of two   */
    uint64_t wr;          /* total samples written         */
    double   base;        /* static delay center           */
} dline;

static int dline_init(dline *d, double max_mod)
{
    double need = ceil(max_mod) + 8.0;   /* integer: exactly removable  */
    size_t len  = 64;
    while ((double)len < 2.0 * need + 16.0) len <<= 1;
    d->buf = calloc(len, sizeof *d->buf);
    if (!d->buf) return -1;
    d->mask = len - 1;
    d->wr   = 0;
    d->base = need;
    return 0;
}

/* Remove the delay line's constant base latency so media output stays
 * sample-aligned with the source (null tests, residual metric). The
 * final 'base' samples are zeroed -- sub-10 ms at the very end of a
 * whole render, and inside the discarded overlap for streaming blocks. */
static void dline_compensate(double *buf, size_t n, const dline *d)
{
    size_t b = (size_t)d->base;
    if (b >= n) return;
    memmove(buf, buf + b, (n - b) * sizeof *buf);
    memset(buf + (n - b), 0, b * sizeof *buf);
}

/* Catmull-Rom cubic read at (wr - base + mod) behind the write head. */
static double dline_tick(dline *d, double x, double mod)
{
    d->buf[d->wr & d->mask] = x;
    double pos = (double)d->wr - d->base + mod;   /* |mod| < base - 4 */
    d->wr++;
    if (pos < 1.0) return 0.0;                    /* warm-up          */
    double  fl = floor(pos);
    double  t  = pos - fl;
    uint64_t i = (uint64_t)fl;
    double y0 = d->buf[(i - 1) & d->mask];
    double y1 = d->buf[ i      & d->mask];
    double y2 = d->buf[(i + 1) & d->mask];
    double y3 = d->buf[(i + 2) & d->mask];
    double c1 = 0.5 * (y2 - y0);
    double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/* Slow random drift: one-pole lowpassed white noise, unit-ish peak. */
typedef struct { double y; double a; uint64_t seed; } drifter;

static void drift_init(drifter *dr, double fc, double fs, uint64_t seed)
{
    dr->y = 0.0;
    dr->a = 1.0 - exp(-2.0 * M_PI * fc / fs);
    dr->seed = seed;
}
static double drift_tick(drifter *dr)
{
    dr->y += dr->a * (3.0 * frand2(&dr->seed) - dr->y);
    return dr->y;
}

/* ====================================================================== */
/* Defaults                                                               */
/* ====================================================================== */

const vinyl_params VINYL_DEFAULTS = {
    .wow_cents = 8.0,   .wow_rate = 0.55,  .drift_cents = 4.0,
    .crackle_per_s = 12.0, .crackle_db = -33.0,
    .hiss_db = -63.0,   .lp_hz = 16000.0,  .hp_hz = 25.0
};

const tape_params TAPE_DEFAULTS = {
    .wow_cents = 4.0,    .wow_rate = 0.8,
    .flutter_cents = 2.5, .flutter_rate = 9.0, .drift_cents = 2.0,
    .hiss_db = -57.0,    .bump_db = 3.0,     .bump_hz = 65.0,
    .hf_loss = 0.35,     .lp_hz = 14000.0
};

#define DRIFT_FC 0.25   /* Hz: bandwidth of the random pitch drift */

/* ====================================================================== */
/* Vinyl                                                                  */
/* ====================================================================== */

int vinyl_process(double *buf, size_t n, double fs,
                  const vinyl_params *p, unsigned channel, double t0)
{
    double a_wow   = mod_amp_samples(p->wow_cents,   p->wow_rate, fs);
    double a_drift = mod_amp_samples(p->drift_cents, DRIFT_FC,    fs);

    dline dl;
    if (dline_init(&dl, a_wow + a_drift) != 0) return -1;

    drifter dr;
    drift_init(&dr, DRIFT_FC, fs, 0xd1f7c0ffee5eed01ULL);  /* same per ch:
        the platter turns once for both channels                          */

    /* extreme corners = transparent: skip the filter entirely so it
     * contributes no phase shift (matters for null tests)              */
    int use_lp = p->lp_hz < 19999.0;
    int use_hp = p->hp_hz > 10.5;
    biquad lp, hp, crk;
    bq_design(&lp,  BQ_LOWPASS,  fs, p->lp_hz, 0.7071, 0.0);
    bq_design(&hp,  BQ_HIGHPASS, fs, p->hp_hz, 0.7071, 0.0);
    bq_design(&crk, BQ_BANDPASS, fs, 3000.0,   1.2,    0.0);

    pink_state ps = {0};
    uint64_t tmix = (uint64_t)(t0 * 1000.0) * 0x100000001b3ULL;
    uint64_t hiss_seed = (0x5eedbead00000001ULL ^ tmix) ^
                         ((uint64_t)channel * 0x9e3779b97f4a7c15ULL);
    uint64_t crk_seed  = 0xc0ac0ac0ac0ac0a1ULL ^ tmix; /* correlated    */
    double hiss_g = pow(10.0, p->hiss_db / 20.0) / pink_rms_ref();
    double crk_g  = pow(10.0, p->crackle_db / 20.0);
    double pcrk   = p->crackle_per_s / fs;

    double ph  = fmod(2.0 * M_PI * p->wow_rate * t0, 2.0 * M_PI);
    double dph = 2.0 * M_PI * p->wow_rate / fs;

    for (size_t i = 0; i < n; i++) {
        double mod = a_wow * sin(ph) + a_drift * drift_tick(&dr);
        ph += dph;
        double y = dline_tick(&dl, buf[i], mod);

        if (use_hp) y = bq_tick(&hp, y);
        if (use_lp) y = bq_tick(&lp, y);

        /* surface noise */
        y += hiss_g * pink_tick(&ps, frand2(&hiss_seed));

        /* crackle: Poisson ticks with a heavy-ish amplitude tail,
         * rung through a 3 kHz bandpass                                  */
        double imp = 0.0;
        if (pcrk > 0.0 && frand(&crk_seed) < pcrk) {
            double a = exp(3.0 * (frand(&crk_seed) - 1.0));  /* -26..0 dB */
            imp = crk_g * a * (frand(&crk_seed) < 0.5 ? -1.0 : 1.0);
        }
        y += bq_tick(&crk, imp);

        buf[i] = y;
    }
    dline_compensate(buf, n, &dl);
    free(dl.buf);
    return 0;
}

/* ====================================================================== */
/* Tape                                                                   */
/* ====================================================================== */

int tape_process(double *buf, size_t n, double fs,
                 const tape_params *p, unsigned channel, double t0)
{
    double a_wow   = mod_amp_samples(p->wow_cents,     p->wow_rate,     fs);
    double a_fl    = mod_amp_samples(p->flutter_cents, p->flutter_rate, fs);
    double a_drift = mod_amp_samples(p->drift_cents,   DRIFT_FC,        fs);

    dline dl;
    if (dline_init(&dl, a_wow + a_fl + a_drift) != 0) return -1;

    drifter dr;
    drift_init(&dr, DRIFT_FC, fs, 0x7a9e5eed00000001ULL);

    int use_lp = p->lp_hz < 19999.0;
    biquad bump, lp;
    bq_design(&bump, BQ_PEAK,    fs, p->bump_hz, 0.9,    p->bump_db);
    bq_design(&lp,   BQ_LOWPASS, fs, p->lp_hz,   0.7071, 0.0);

    /* level-dependent HF loss: crossfade toward a one-pole LP (~4.5 kHz)
     * by hf_loss * envelope. Envelope: |x| with 5 ms attack / 60 ms
     * release, normalized so loud program material approaches 1.         */
    double g_lp1  = 1.0 - exp(-2.0 * M_PI * 4500.0 / fs);
    double lp1    = 0.0;
    double env    = 0.0;
    double a_att  = 1.0 - exp(-1.0 / (0.005 * fs));
    double a_rel  = 1.0 - exp(-1.0 / (0.060 * fs));

    uint64_t hiss_seed = (0x7ea5eed000000001ULL ^
                          ((uint64_t)(t0 * 1000.0) * 0x100000001b3ULL)) ^
                         ((uint64_t)channel * 0x9e3779b97f4a7c15ULL);
    double hiss_g = pow(10.0, p->hiss_db / 20.0) * sqrt(3.0); /* uniform
                       white has RMS 1/sqrt(3)                            */

    double ph_w = fmod(2.0 * M_PI * p->wow_rate * t0, 2.0 * M_PI);
    double dph_w = 2.0 * M_PI * p->wow_rate     / fs;
    double ph_f = fmod(2.0 * M_PI * p->flutter_rate * t0, 2.0 * M_PI);
    double dph_f = 2.0 * M_PI * p->flutter_rate / fs;

    for (size_t i = 0; i < n; i++) {
        double mod = a_wow * sin(ph_w) + a_fl * sin(ph_f)
                   + a_drift * drift_tick(&dr);
        ph_w += dph_w;
        ph_f += dph_f;
        double y = dline_tick(&dl, buf[i], mod);

        y = bq_tick(&bump, y);

        double ax = fabs(y);
        env += (ax > env ? a_att : a_rel) * (ax - env);
        double e = env / 0.4;               /* ~1 for loud material */
        if (e > 1.0) e = 1.0;
        lp1 += g_lp1 * (y - lp1);
        y = lp1 + (y - lp1) * (1.0 - p->hf_loss * e);

        if (use_lp) y = bq_tick(&lp, y);
        y += hiss_g * frand2(&hiss_seed);

        buf[i] = y;
    }
    dline_compensate(buf, n, &dl);
    free(dl.buf);
    return 0;
}
