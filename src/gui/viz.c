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

/* The waveform strip wants the WHOLE file, audacity-style, so a
 * background thread decodes the track a second time -- decoders are
 * many times faster than realtime -- and folds min/max into VIZ_PK
 * buckets. The GUI draws whatever is filled so far; the strip grows
 * left to right for a second or two on long files. Streams (no
 * duration) get no peaks.
 *
 * The spectrum is the live tap: Hann-windowed power via audiotard's
 * fft.c, exponentially averaged across frames so it reads steady. */

#include "viz.h"
#include "decoder.h"
#include "fft.h"
#include <string.h>
#include <stdlib.h>

void viz_init(viz *v) {
    memset(v, 0, sizeof *v);
    v->pk_track = -1;
    v->ring_c = calloc(VIZ_RING, sizeof *v->ring_c);
    v->ring_p = calloc(VIZ_RING, sizeof *v->ring_p);
}

static void pk_join(viz *v) {
    if (v->pk_running) {
        v->pk_stop = 1;
        pthread_join(v->pk_th, NULL);
        v->pk_running = 0;
        v->pk_stop = 0;
    }
}

void viz_shutdown(viz *v) {
    pk_join(v);
    free(v->ring_c);
    free(v->ring_p);
    v->ring_c = NULL;
    v->ring_p = NULL;
}

void viz_no_peaks(viz *v) {
    pk_join(v);
    v->pk_track = -1;
    v->pk_filled = 0;
}

struct pk_job {
    viz *v;
    const track *t;
};

static void *pk_main(void *arg) {
    struct pk_job *job = arg;
    viz *v = job->v;
    const track *t = job->t;
    free(job);
    decoder *d = decoder_open(t->path, t->fmt);
    if (!d) return NULL;
    int ch = decoder_channels(d);
    if (ch < 1) ch = 1;
    int rate = decoder_rate(d);
    double total = t->duration > 0 ? t->duration * rate : 0;
    float buf[4096 * 2];
    long done = 0;
    float mn = 0.0f, mx = 0.0f;
    int bucket = 0;
    while (!v->pk_stop) {
        long n = decoder_read(d, buf, 4096 / (ch > 2 ? ch : 2));
        if (n <= 0) break;
        for (long f = 0; f < n; f++) {
            float s = 0.0f;
            for (int k = 0; k < ch; k++) s += buf[f * ch + k];
            s /= (float)ch;
            if (s < mn) mn = s;
            if (s > mx) mx = s;
            done++;
            int nb = total > 0
                   ? (int)((double)done * VIZ_PK / total) : 0;
            if (nb > bucket && bucket < VIZ_PK) {
                v->pkmin[bucket] = mn;
                v->pkmax[bucket] = mx;
                bucket++;
                v->pk_filled = bucket;
                mn = mx = 0.0f;
            }
        }
    }
    if (bucket < VIZ_PK && (mn != 0.0f || mx != 0.0f)) {
        v->pkmin[bucket] = mn;
        v->pkmax[bucket] = mx;
        v->pk_filled = bucket + 1;
    }
    decoder_close(d);
    return NULL;
}

void viz_want_peaks(viz *v, const table *tb, size_t ti) {
    if ((long)ti == v->pk_track) return;
    pk_join(v);
    v->pk_track = (long)ti;
    v->pk_filled = 0;
    memset(v->pkmin, 0, sizeof v->pkmin);
    memset(v->pkmax, 0, sizeof v->pkmax);
    const track *t = table_at(tb, ti);
    if (!t || t->duration <= 0) return;      /* streams: no strip */
    struct pk_job *job = malloc(sizeof *job);
    if (!job) return;
    job->v = v;
    job->t = t;
    v->pk_running = pthread_create(&v->pk_th, NULL, pk_main, job) == 0;
    if (!v->pk_running) free(job);
}

int viz_fold_spectrum(viz *v, const float *clean, const float *proc,
                      int n, int mode, double param,
                      double proc_scale) {
    if (n < VIZ_FFT) return 0;
    static double x[VIZ_FFT], pwc[VIZ_FFT / 2], pwp[VIZ_FFT / 2];
    for (int i = 0; i < VIZ_FFT; i++)
        x[i] = clean[n - VIZ_FFT + i];
    fft_spectrum_pow(x, VIZ_FFT, pwc);
    for (int i = 0; i < VIZ_FFT; i++)
        x[i] = proc[n - VIZ_FFT + i] * proc_scale;
    fft_spectrum_pow(x, VIZ_FFT, pwp);

    if (mode == VIZ_BOX && v->ring_c && v->ring_p) {
        int N = (int)param;
        if (N < 2) N = 2;
        if (N > VIZ_RING) N = VIZ_RING;
        for (int i = 0; i < VIZ_FFT / 2; i++) {
            v->ring_c[v->ring_w][i] = (float)pwc[i];
            v->ring_p[v->ring_w][i] = (float)pwp[i];
        }
        v->ring_w = (v->ring_w + 1) % VIZ_RING;
        if (v->ring_n < VIZ_RING) v->ring_n++;
        int m = v->ring_n < N ? v->ring_n : N;
        for (int i = 0; i < VIZ_FFT / 2; i++) {
            double sc = 0, sp = 0;
            for (int k = 1; k <= m; k++) {
                int j = (v->ring_w + VIZ_RING - k) % VIZ_RING;
                sc += v->ring_c[j][i];
                sp += v->ring_p[j][i];
            }
            v->sp_clean[i] = sc / m;
            v->sp_proc[i]  = sp / m;
        }
    } else {
        double alpha = param;
        if (alpha <= 0.0) alpha = 0.05;
        if (alpha > 1.0) alpha = 1.0;
        const double a = v->sp_primed ? alpha : 1.0;
        for (int i = 0; i < VIZ_FFT / 2; i++) {
            v->sp_clean[i] += a * (pwc[i] - v->sp_clean[i]);
            v->sp_proc[i]  += a * (pwp[i] - v->sp_proc[i]);
        }
    }
    v->sp_primed = 1;
    return 1;
}
