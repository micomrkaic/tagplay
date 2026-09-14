/* This file is part of tagplay/tagview.
 *
 * tagview -- search-driven image browser on the tagplay core
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

#ifndef TV_IMG_TAGS_H
#define TV_IMG_TAGS_H
#include "track.h"
#include <stdint.h>
#include <stddef.h>

int img_read_jpeg(track *t);
int img_read_png(track *t);
uint8_t *img_file_read(const track *t, size_t *len);

#endif
