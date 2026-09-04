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

#include "engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------- */
/* Kaiser-windowed sinc lowpass design                                    */
/* ---------------------------------------------------------------------- */

/* Modified Bessel function of the first kind, order 0 (series). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    double hx = 0.5 * x;
    for (int k = 1; k < 64; k++) {
        term *= (hx / k) * (hx / k);
        sum  += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

static double kaiser_beta(double atten_db)
{
    if (atten_db > 50.0)
        return 0.1102 * (atten_db - 8.7);
    if (atten_db >= 21.0)
        return 0.5842 * pow(atten_db - 21.0, 0.4) + 0.07886 * (atten_db - 21.0);
    return 0.0;
}

double *fir_design_lowpass(double fc, double trans, double atten_db,
                           int align, int *ntaps)
{
    double beta = kaiser_beta(atten_db);
    double dw   = 2.0 * M_PI * trans;                 /* rad/sample      */
    int N = (int)ceil((atten_db - 7.95) / (2.285 * dw));
    if (N < 9) N = 9;
    if (!(N & 1)) N++;                                /* odd => integer  */
    if (align > 1)
        while ((N - 1) % align) N++;   /* 2M multiple of L: with even
            align this lands on odd N, so both properties hold          */
    int M = (N - 1) / 2;                              /* group delay     */

    double *h = malloc((size_t)N * sizeof *h);
    if (!h) return NULL;

    double i0b = bessel_i0(beta);
    for (int n = 0; n < N; n++) {
        int    d = n - M;
        double s = (d == 0) ? 2.0 * fc
                            : sin(2.0 * M_PI * fc * d) / (M_PI * d);
        double t = (double)d / (double)M;
        double w = bessel_i0(beta * sqrt(1.0 - t * t)) / i0b;
        h[n] = s * w;
    }
    *ntaps = N;
    return h;
}

/* ---------------------------------------------------------------------- */
/* Polyphase upsampling / decimation                                      */
/* ---------------------------------------------------------------------- */

/* v[n*L + k] = L * sum_j h[j*L + k] * x[n - j]
 * i.e. FIR applied to the zero-stuffed input; delay = M at the high rate. */
static void upsample(const double *x, size_t n, int L,
                     const double *h, int N, double *v)
{
    int jmax = (N + L - 1) / L;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < L; k++) {
            double acc = 0.0;
            for (int j = 0; j < jmax; j++) {
                int tap = j * L + k;
                if (tap >= N) break;
                long xi = (long)i - j;
                if (xi >= 0) acc += h[tap] * x[xi];
            }
            v[i * (size_t)L + k] = (double)L * acc;
        }
    }
}

/* y[n] = (h * w)[n*L + 2M]  -- the 2M compensates the group delay of the
 * upsampling pass (M) plus this decimation pass (M), so y aligns with x. */
static void decimate(const double *w, size_t wlen, size_t n, int L,
                     const double *h, int N, double *y)
{
    int M = (N - 1) / 2;
    for (size_t i = 0; i < n; i++) {
        long m   = (long)i * L + 2L * M;
        double acc = 0.0;
        for (int t = 0; t < N; t++) {
            long wi = m - t;
            if (wi >= 0 && (size_t)wi < wlen) acc += h[t] * w[wi];
        }
        y[i] = acc;
    }
}

/* ---------------------------------------------------------------------- */
/* Waveshapers                                                            */
/* ---------------------------------------------------------------------- */

double ws_h2_coeff(double h2_db, double ref_amp)
{
    return 2.0 * pow(10.0, h2_db / 20.0) / ref_amp;
}

/* All shapes map 0 -> 0 and are normalized to unity small-signal gain so
 * that switching a shape in/out does not change loudness to first order. */
static void shape_buf(double *v, size_t n, const ws_params *p)
{
    switch (p->shape) {
    case WS_TANH: {
        double g = p->drive;
        double k = tanh(g);            /* span-normalized soft clipper  */
        for (size_t i = 0; i < n; i++)
            v[i] = tanh(g * v[i]) / k;
        break;
    }
    case WS_TUBE: {
        double g  = p->drive, b = p->bias;
        double t0 = tanh(g * b);
        /* Span normalization: a full-scale input keeps a full-scale
         * output span for ANY g,b. (Normalizing to unity small-signal
         * gain instead blows up as g*b grows: f'(0)=g*sech^2(g*b)->0.) */
        double k  = 0.5 * (tanh(g * (1.0 + b)) - tanh(g * (-1.0 + b)));
        for (size_t i = 0; i < n; i++)
            v[i] = (tanh(g * (v[i] + b)) - t0) / k;
        break;
    }
    case WS_H2: {
        double a = p->h2;
        for (size_t i = 0; i < n; i++)
            v[i] = v[i] + a * v[i] * v[i];
        break;
    }
    }
}

/* One-pole DC blocker: y[n] = x[n] - x[n-1] + R y[n-1].
 * Corner at 0.5 Hz: high enough to remove the waveshaper's DC term,
 * low enough that its phase lead at audio frequencies stays below the
 * null-test floor (phase ~ fc/f rad: -66 dB re a 1 kHz fundamental).    */
static void dc_block(double *v, size_t n, double fs_here)
{
    double R  = 1.0 - 2.0 * M_PI * 0.5 / fs_here;
    double x1 = 0.0, y1 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double y = v[i] - x1 + R * y1;
        x1 = v[i];
        y1 = y;
        v[i] = y;
    }
}

/* ---------------------------------------------------------------------- */
/* Top level                                                              */
/* ---------------------------------------------------------------------- */

int ws_process(const double *in, double *out, size_t n,
               double fs, int L, const ws_params *p)
{
    if (L <= 1) {                       /* aliasing baseline / bypass OS  */
        memcpy(out, in, n * sizeof *out);
        shape_buf(out, n, p);
        dc_block(out, n, fs);
        return 0;
    }

    /* Anti-image/anti-alias prototype at the oversampled rate.
     * Passband edge: min(20 kHz, 0.45*fs); stopband edge: fs/2.          */
    double pass  = fmin(20000.0 / fs, 0.45);
    double stop  = 0.5;
    double fc    = 0.5 * (pass + stop) / L;
    double trans = (stop - pass) / L;

    int     N;
    double *h = fir_design_lowpass(fc, trans, 96.0, L, &N);
    if (!h) return -1;
    int M = (N - 1) / 2;

    /* Padded high-rate buffer so the decimator's look-back (2M) and the
     * filter tails always index real (zeroed) memory.                    */
    size_t  vlen = n * (size_t)L + 2 * (size_t)M + (size_t)N;
    double *v    = calloc(vlen, sizeof *v);
    if (!v) { free(h); return -1; }

    upsample(in, n, L, h, N, v);
    shape_buf(v, vlen, p);              /* zeros map to zero: safe        */
    dc_block(v, vlen, fs * L);
    decimate(v, vlen, n, L, h, N, out);

    free(v);
    free(h);
    return 0;
}
