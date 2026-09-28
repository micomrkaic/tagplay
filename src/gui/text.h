/* This file is part of tagplay.
 *
 * tagplay-gui -- truetype text via a baked glyph atlas
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

#ifndef TP_GUI_TEXT_H
#define TP_GUI_TEXT_H

#include <SDL.h>

/* Monospaced truetype text (JetBrains Mono, ASCII subset) baked once
 * into an alpha atlas by stb_truetype. The font is mono, so column
 * arithmetic stays exact: text_cw() is the fixed advance, text_ch()
 * the line height. */

int  text_init(SDL_Renderer *r, float px);  /* 0 on success */
void text_shutdown(void);
int  text_cw(void);
int  text_ch(void);
void text_draw(SDL_Renderer *r, int x, int y, const char *s,
               SDL_Color c);
void text_drawn(SDL_Renderer *r, int x, int y, const char *s,
                int maxch, SDL_Color c);

#endif
