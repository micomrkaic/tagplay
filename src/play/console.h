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

#ifndef TP_CONSOLE_H
#define TP_CONSOLE_H
#include "track.h"
#include "browser.h"
#include <stdint.h>

struct player;
void  *console_init(struct player *pl);
size_t stations_load(table *tb);

/* hook implementations wired into PLAY_APP (app_play.c) */
void console_hook_identity(const track *t, char *out, size_t sz);
void console_detail_header(const track *t, char *out, size_t sz);
int  console_pulse(void *ui);
uint64_t console_sig(void *ui);
int  console_poll_msg(void *ui, char *out, size_t sz);
int  console_status_rows(void *ui);
void console_status(void *ui, struct browser *b, int cols);
size_t console_alt_len(void *ui);
size_t console_alt_home(void *ui);
void console_alt_view(void *ui, struct browser *b);
int  console_alt_key(void *ui, struct browser *b, int key);
int  console_alt_snapshot(void *ui, struct browser *b);
void console_on_enter(void *ui, struct browser *b, const vec *items, int from_sel);
int  console_global_key(void *ui, struct browser *b, int key);
int  console_command(void *ui, struct browser *b, const char *cmd);
extern const char *const *console_id_keys;

#endif
