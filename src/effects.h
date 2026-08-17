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

/* audiotard effects: parametric EQ (RBJ biquads), wow/flutter via a
 * modulated fractional-delay line, noise generators, and the vinyl/tape
 * composite media simulations.
 *
 * Everything here is linear or additive (the one exception, the tape
 * HF-loss crossfade, is time-varying linear), so none of it needs the
 * oversampling machinery -- these run at the base rate, after the
 * waveshaper stage.
 */
#ifndef AUDIOTARD_EFFECTS_H
#define AUDIOTARD_EFFECTS_H

#include <stddef.h>

/* ---- biquad (RBJ cookbook), transposed direct form II ---------------- */

typedef struct {
    double b0, b1, b2, a1, a2;   /* normalized (a0 = 1)  */
    double s1, s2;               /* state                */
} biquad;

typedef enum {
    BQ_PEAK, BQ_LOWSHELF, BQ_HIGHSHELF, BQ_LOWPASS, BQ_HIGHPASS, BQ_BANDPASS
} bq_type;

/* gain_db is ignored for LP/HP/BP. Resets state. */
void   bq_design(biquad *q, bq_type t, double fs, double f,
                 double Q, double gain_db);
double bq_tick(biquad *q, double x);
void   bq_process(biquad *q, double *buf, size_t n);

/* ---- composite media simulations ------------------------------------ */

typedef struct {
    double wow_cents;     /* peak pitch deviation of the 'wow' LFO       */
    double wow_rate;      /* Hz (0.55 = 33 rpm eccentricity)             */
    double drift_cents;   /* random slow pitch drift, peak-ish           */
    double crackle_per_s; /* mean tick rate (Poisson); 0 disables        */
    double crackle_db;    /* tick level, dB re full scale                */
    double hiss_db;       /* surface-noise RMS, dBFS (pink)              */
    double lp_hz;         /* signal bandwidth                            */
    double hp_hz;         /* signal low cut                              */
} vinyl_params;

typedef struct {
    double wow_cents;     /* slow component                              */
    double wow_rate;
    double flutter_cents; /* fast component                              */
    double flutter_rate;
    double drift_cents;
    double hiss_db;       /* tape hiss RMS, dBFS (white-ish)             */
    double bump_db;       /* head-bump peaking gain                      */
    double bump_hz;
    double hf_loss;       /* 0..1: strength of level-dependent HF loss   */
    double lp_hz;         /* bandwidth                                   */
} tape_params;

extern const vinyl_params VINYL_DEFAULTS;
extern const tape_params  TAPE_DEFAULTS;

/* In-place, one channel. 'channel' decorrelates hiss between channels
 * while keeping crackle correlated (groove damage hits both channels;
 * hiss does not). 't0' is the buffer's start time in seconds within the
 * source: LFO phases derive from it so block-wise (streaming) rendering
 * stays phase-continuous with whole-file rendering; noise seeds mix it
 * in so consecutive blocks don't repeat identical noise.               */
int vinyl_process(double *buf, size_t n, double fs,
                  const vinyl_params *p, unsigned channel, double t0);
int tape_process(double *buf, size_t n, double fs,
                 const tape_params *p, unsigned channel, double t0);

#endif
