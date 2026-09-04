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

#ifndef TP_PLAYER_H
#define TP_PLAYER_H
#include "track.h"
#include "dsp.h"

typedef struct player player;

player *player_create(const table *tb);
void    player_destroy(player *p);

/* replace queue with these table indices and start at position 0 */
void player_play(player *p, const size_t *idx, size_t n);
void player_toggle_pause(player *p);
void player_next(player *p);
void player_prev(player *p);
void player_stop(player *p);
void player_seek(player *p, double seconds);      /* absolute */
void player_jump(player *p, size_t queue_index);  /* start playing queue[i] */
void player_move(player *p, size_t from, size_t to); /* reorder queue */
void player_get_queue(player *p, vec *out);       /* copy queue indices */
dsp_chain *player_dsp(player *p);                 /* thread-safe enough: atomic-ish params */

/* status snapshot for the UI */
typedef struct {
    int    playing;        /* 0 stopped, 1 playing, 2 paused */
    size_t queue_pos, queue_len;
    size_t track_index;    /* table index of current track */
    double pos, dur;
    int    rate, channels;
    int    null_output;    /* 1 if no sound device (silent pacing) */
    double vu_l, vu_r;     /* post-DSP peak levels, 0..1 */
    char   stream_title[256]; /* live radio StreamTitle, "" if none */
    char   note[160];         /* one-shot note (unplayable track etc.) */
    unsigned note_seq;
} player_status;
void player_get_status(player *p, player_status *st);

#endif
