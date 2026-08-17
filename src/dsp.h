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

#ifndef TP_DSP_H
#define TP_DSP_H

/* The insert chain between decoder and output. This is where Audiotard
 * plugs in: implement these five functions with your tube/tape/vinyl
 * processors (or link audiotard objects and dispatch to them from
 * dsp_process). State lives inside dsp_chain and survives track
 * boundaries -> gapless stays gapless through stateful effects.
 *
 * A built-in demo effect ("tube": tanh soft saturation) is included so
 * :dsp is audible before Audiotard is wired in. */
typedef struct dsp_chain dsp_chain;

dsp_chain *dsp_create(void);
void dsp_destroy(dsp_chain *c);

/* called whenever the stream format changes (per-track native rate) */
void dsp_on_format(dsp_chain *c, int rate, int channels);

/* mode: "off", "tube", ... ; amount in [0,1]. Returns 0 if mode known. */
int  dsp_set_mode(dsp_chain *c, const char *mode, double amount);
const char *dsp_mode_name(const dsp_chain *c);

/* in-place on interleaved float32 */
void dsp_process(dsp_chain *c, float *buf, long frames);

/* master gain, linear [0,2] */
void dsp_set_gain(dsp_chain *c, double gain);
double dsp_gain(const dsp_chain *c);

#endif
