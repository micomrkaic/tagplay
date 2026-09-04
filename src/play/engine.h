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

/* audiotard engine: oversampled waveshaping core.
 *
 * Signal path:  x --[polyphase FIR upsample xL]--> shape() --> DC block
 *                 --[same FIR, decimate /L]--> y
 *
 * All processing in double, offline (whole-buffer). The up/down FIR is a
 * Kaiser-windowed sinc designed at runtime for the given sample rate and
 * oversampling factor. Group delay of both filter passes is compensated
 * in the decimator, so y is time-aligned with x (important for ABX
 * loudness matching and null tests later).
 */
#ifndef AUDIOTARD_ENGINE_H
#define AUDIOTARD_ENGINE_H

#include <stddef.h>

typedef enum {
    WS_TANH,   /* y = tanh(g x)/g            : odd harmonics, symmetric   */
    WS_TUBE,   /* biased tanh, normalized    : even + odd, "tube" flavor  */
    WS_H2      /* y = x + a x^2              : pure 2nd harmonic (+DC)    */
} ws_shape;

typedef struct {
    ws_shape shape;
    double   drive;   /* g for WS_TANH / WS_TUBE                          */
    double   bias;    /* b for WS_TUBE (asymmetry)                        */
    double   h2;      /* a for WS_H2                                      */
} ws_params;

/* For WS_H2: coefficient a giving H2 at h2_db (dB re fundamental) when the
 * input is a sine of amplitude ref_amp.  H2/H1 = a*A/2  =>  a = 2*r/A.   */
double ws_h2_coeff(double h2_db, double ref_amp);

/* Process n samples at sample rate fs with oversampling factor L (>=1).
 * L==1 bypasses resampling entirely (useful as an aliasing baseline).
 * Returns 0 on success, -1 on allocation failure.                        */
int ws_process(const double *in, double *out, size_t n,
               double fs, int L, const ws_params *p);

/* --- exposed for tests ------------------------------------------------ */

/* Kaiser-windowed sinc lowpass. fc, trans in cycles/sample (normalized to
 * the rate the filter runs at). Returns malloc'd taps, sets *ntaps (odd).
 * align > 1 additionally rounds N up so (N-1) is a multiple of align --
 * needed so the up/down group delay 2M lands on an integer number of
 * base-rate samples (exact time alignment for null tests).             */
double *fir_design_lowpass(double fc, double trans, double atten_db,
                           int align, int *ntaps);

#endif
