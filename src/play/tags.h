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

#ifndef TP_TAGS_H
#define TP_TAGS_H
#include "track.h"

/* Each fills tags + audio params on t (path/mtime/fsize already set).
 * Return 0 on success, -1 on unreadable/corrupt. */
int tags_read_flac(track *t);
int tags_read_wav(track *t);
int tags_read_mp3(track *t);

/* If TITLE missing, synthesize TITLE/ALBUM/ARTIST from path; adds SOURCE=path */
void tags_fallback_from_path(track *t);

#endif
