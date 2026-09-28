/* This file is part of tagplay.
 *
 * tagplay-gui -- instrument-panel state (waveform peaks, spectrum)
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

#ifndef TP_GUI_VIZ_H
#define TP_GUI_VIZ_H

#include <stddef.h>
#include <pthread.h>
#include "track.h"

#define VIZ_PK   800           /* waveform buckets across the strip */
#define VIZ_FFT  2048          /* spectrum window */

typedef struct {
    /* full-file waveform peaks, built by a background decode pass */
    float    pkmin[VIZ_PK], pkmax[VIZ_PK];
    int      pk_filled;        /* buckets valid so far (progressive) */
    long     pk_track;         /* table index built/being built, -1 none */
    int      pk_stop;          /* ask the builder to bail */
    int      pk_running;
    pthread_t pk_th;
    /* spectrum: time-averaged power, clean + processed */
    double   sp_clean[VIZ_FFT / 2];
    double   sp_proc[VIZ_FFT / 2];
    int      sp_primed;
} viz;

void viz_init(viz *v);
void viz_shutdown(viz *v);
/* start (or keep) building peaks for table track ti; safe to call every
 * frame -- it only restarts when the track changes */
void viz_want_peaks(viz *v, const table *tb, size_t ti);
void viz_no_peaks(viz *v);     /* stop + clear (radio, silence) */
/* fold the latest tap into the averaged spectra (call per frame).
 * alpha in (0,1]: EMA weight of the new frame (1 = no averaging).
 * proc_scale multiplies processed samples before the FFT -- the GUI
 * passes 1/volume so the overlay compares CHARACTER, not level.
 * Returns 0 if nothing to fold. */
int viz_fold_spectrum(viz *v, const float *clean, const float *proc,
                      int n, double alpha, double proc_scale);

#endif
