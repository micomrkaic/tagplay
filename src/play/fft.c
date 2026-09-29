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

#include "fft.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void fft_complex(double *re, double *im, int n)
{
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double tr = re[b] * cr - im[b] * ci;
                double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;  im[b] = im[a] - ti;
                re[a] += tr;         im[a] += ti;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

void fft_spectrum_pow(const double *x, int n, double *pw)
{
    static double re[FFT_MAX], im[FFT_MAX];
    if (n > FFT_MAX) n = FFT_MAX;

    double wsum = 0.0;
    for (int i = 0; i < n; i++) {
        double w = 0.5 * (1.0 - cos(2.0 * M_PI * i / (n - 1)));  /* Hann */
        re[i] = x[i] * w;
        im[i] = 0.0;
        wsum += w;
    }
    fft_complex(re, im, n);

    double norm = 2.0 / wsum;    /* full-scale sine -> power ~1.0 */
    for (int k = 0; k < n / 2; k++) {
        double m = norm * sqrt(re[k] * re[k] + im[k] * im[k]);
        pw[k] = m * m;
    }
}

void fft_spectrum_db(const double *x, int n, double *db)
{
    static double pw[FFT_MAX / 2];
    fft_spectrum_pow(x, n, pw);
    for (int k = 0; k < n / 2; k++)
        db[k] = 10.0 * log10(pw[k] + 1e-24);
}
