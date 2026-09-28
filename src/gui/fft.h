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

/* Small FFT for the GUI spectrum display. Radix-2 iterative, plus a
 * convenience wrapper: Hann-windowed magnitude spectrum in dB, normalized
 * so a full-scale sine reads ~0 dB.
 *
 * Not thread-safe (static scratch); call from one thread (the GUI does).
 */
#ifndef AUDIOTARD_FFT_H
#define AUDIOTARD_FFT_H

#define FFT_MAX 8192

/* In-place complex FFT; n must be a power of two <= FFT_MAX. */
void fft_complex(double *re, double *im, int n);

/* x: n mono samples -> normalized power pw[0 .. n/2-1] (full-scale sine
 * ~= 1.0), bin k at freq k*fs/n. Average THIS across time, not dB. */
void fft_spectrum_pow(const double *x, int n, double *pw);

/* Convenience: 10*log10 of the above. */
void fft_spectrum_db(const double *x, int n, double *db);

#endif
