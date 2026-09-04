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

/* The audio application's answers to the core's questions. */

#include "app.h"
#include "tags.h"
#include "art_audio.h"
#include "console.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

const char *fmt_name(int f) {
    switch (f) {
    case FMT_FLAC:  return "flac";
    case FMT_WAV:   return "wav";
    case FMT_MP3:   return "mp3";
    case FMT_RADIO: return "radio";
    default:        return "?";
    }
}

static int play_want_path(const char *fname) {
    const char *dot = strrchr(fname, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".wav") ||
           !strcasecmp(dot, ".mp3");
}

static int play_probe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t m[12] = { 0 };
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    if (n >= 4 && !memcmp(m, "fLaC", 4)) return FMT_FLAC;
    if (n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WAVE", 4)) return FMT_WAV;
    if (n >= 3 && !memcmp(m, "ID3", 3)) return FMT_MP3;
    if (n >= 2 && m[0] == 0xFF && (m[1] & 0xE0) == 0xE0) return FMT_MP3;
    return -1;
}

static int play_read_item(track *t, int fmt) {
    (void)fmt; /* the readers set t->fmt themselves */
    switch (fmt) {
    case FMT_FLAC: return tags_read_flac(t);
    case FMT_WAV:  return tags_read_wav(t);
    default:       return tags_read_mp3(t);
    }
}

static int play_cacheable(const track *t) {
    return t->fmt != FMT_RADIO;   /* stations live in their own file */
}

static double pf_length(const track *t)   { return t->duration; }
static double pf_rate(const track *t)     { return t->sample_rate; }
static double pf_channels(const track *t) { return t->channels; }

static const pseudo_field play_fields[] = {
    { "length",   pf_length },
    { "rate",     pf_rate },
    { "channels", pf_channels },
};

static uint8_t *play_art_read(const track *t, size_t *len) {
    return audio_art_read(t, len);
}

static const tp_app PLAY_APP = {
    .name          = "tagplay",
    .cache_file    = "cache.bin",
    .cache_version = 0,
    .want_path     = play_want_path,
    .probe         = play_probe,
    .read_item     = play_read_item,
    .cacheable     = play_cacheable,
    .fmt_name      = fmt_name,
    .fields        = play_fields,
    .nfields       = sizeof play_fields / sizeof *play_fields,
    .art_read      = play_art_read,
    .id_keys       = (const char *const []){
        "TITLE", "ARTIST", "ALBUMARTIST", "COMPOSER", "PERFORMER",
        "CONDUCTOR", "ORCHESTRA", "ENSEMBLE", "ALBUM", "DATE",
        "TRACKNUMBER", "DISCNUMBER", "GENRE", NULL },
    .identity      = console_hook_identity,
    .detail_header = console_detail_header,
    .ui_pulse      = console_pulse,
    .ui_sig        = console_sig,
    .poll_msg      = console_poll_msg,
    .status_rows   = console_status_rows,
    .status        = console_status,
    .alt_len       = console_alt_len,
    .alt_home      = console_alt_home,
    .alt_view      = console_alt_view,
    .alt_key       = console_alt_key,
    .alt_snapshot  = console_alt_snapshot,
    .on_enter      = console_on_enter,
    .global_key    = console_global_key,
    .command       = console_command,
};

const tp_app *APP = &PLAY_APP;
