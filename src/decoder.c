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

#include "decoder.h"
#include <FLAC/stream_decoder.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct decoder_ops {
    long (*read)(decoder *d, float *buf, long max_frames);
    int  (*seek)(decoder *d, double seconds);
    void (*close)(decoder *d);
} decoder_ops;

struct decoder {
    const decoder_ops *ops;
    int    rate, channels;
    double duration;
    long   pos_frames;

    /* FLAC */
    FLAC__StreamDecoder *fl;
    float *pend;          /* pending interleaved samples from last write cb */
    long   pend_frames, pend_off;
    int    fl_error;

    /* WAV */
    FILE  *wf;
    long   wav_data_off, wav_data_len; /* bytes */
    int    wav_bits, wav_float;

    /* MP3 */
    mp3dec_ex_t mp3;
    int    mp3_open;
};

/* ---------------- FLAC ---------------- */
static FLAC__StreamDecoderWriteStatus fl_write(const FLAC__StreamDecoder *fl,
        const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *ud) {
    (void)fl;
    decoder *d = ud;
    unsigned n = frame->header.blocksize, ch = frame->header.channels;
    unsigned bps = frame->header.bits_per_sample;
    float scale = 1.0f / (float)(1u << (bps - 1));
    d->pend = xrealloc(d->pend, sizeof(float) * n * ch);
    for (unsigned i = 0; i < n; i++)
        for (unsigned c = 0; c < ch; c++)
            d->pend[i * ch + c] = (float)buffer[c][i] * scale;
    d->pend_frames = n;
    d->pend_off = 0;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void fl_meta(const FLAC__StreamDecoder *fl, const FLAC__StreamMetadata *m, void *ud) {
    (void)fl;
    decoder *d = ud;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
        d->rate = (int)m->data.stream_info.sample_rate;
        d->channels = (int)m->data.stream_info.channels;
        if (d->rate)
            d->duration = (double)m->data.stream_info.total_samples / d->rate;
    }
}
static void fl_err(const FLAC__StreamDecoder *fl,
                   FLAC__StreamDecoderErrorStatus st, void *ud) {
    (void)fl; (void)st;
    ((decoder *)ud)->fl_error = 1;
}

static long flac_read(decoder *d, float *buf, long max_frames) {
    long done = 0;
    while (done < max_frames) {
        if (d->pend_off < d->pend_frames) {
            long take = d->pend_frames - d->pend_off;
            if (take > max_frames - done) take = max_frames - done;
            memcpy(buf + done * d->channels,
                   d->pend + d->pend_off * d->channels,
                   sizeof(float) * (size_t)take * (size_t)d->channels);
            d->pend_off += take;
            done += take;
            continue;
        }
        FLAC__StreamDecoderState st = FLAC__stream_decoder_get_state(d->fl);
        if (st == FLAC__STREAM_DECODER_END_OF_STREAM) break;
        if (!FLAC__stream_decoder_process_single(d->fl) || d->fl_error) {
            if (done == 0) return -1;
            break;
        }
    }
    d->pos_frames += done;
    return done;
}
static int flac_seek(decoder *d, double sec) {
    FLAC__uint64 target = (FLAC__uint64)(sec * d->rate);
    d->pend_frames = d->pend_off = 0;
    if (!FLAC__stream_decoder_seek_absolute(d->fl, target)) {
        FLAC__stream_decoder_flush(d->fl);
        return -1;
    }
    d->pos_frames = (long)target;
    return 0;
}
static void flac_close(decoder *d) {
    if (d->fl) {
        FLAC__stream_decoder_finish(d->fl);
        FLAC__stream_decoder_delete(d->fl);
    }
    free(d->pend);
}
static const decoder_ops FLAC_OPS = { flac_read, flac_seek, flac_close };

static int flac_open(decoder *d, const char *path) {
    d->fl = FLAC__stream_decoder_new();
    if (!d->fl) return -1;
    if (FLAC__stream_decoder_init_file(d->fl, path, fl_write, fl_meta, fl_err, d)
        != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return -1;
    if (!FLAC__stream_decoder_process_until_end_of_metadata(d->fl)) return -1;
    if (!d->rate || !d->channels) return -1;
    d->ops = &FLAC_OPS;
    return 0;
}

/* ---------------- WAV ---------------- */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static long wav_read(decoder *d, float *buf, long max_frames) {
    int bpsamp = d->wav_bits / 8;
    int frame_bytes = bpsamp * d->channels;
    long remain_frames = (d->wav_data_len / frame_bytes) - d->pos_frames;
    if (remain_frames <= 0) return 0;
    if (max_frames > remain_frames) max_frames = remain_frames;
    size_t nbytes = (size_t)max_frames * (size_t)frame_bytes;
    uint8_t *raw = xmalloc(nbytes);
    size_t got = fread(raw, 1, nbytes, d->wf);
    long frames = (long)(got / (size_t)frame_bytes);
    long nsamp = frames * d->channels;
    for (long i = 0; i < nsamp; i++) {
        const uint8_t *p = raw + (size_t)i * (size_t)bpsamp;
        float v;
        if (d->wav_float) {
            float f;
            memcpy(&f, p, 4);
            v = f;
        } else if (d->wav_bits == 16) {
            int16_t s = (int16_t)(p[0] | p[1] << 8);
            v = (float)s / 32768.0f;
        } else if (d->wav_bits == 24) {
            int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                                  (uint32_t)p[2] << 24) >> 8;
            v = (float)s / 8388608.0f;
        } else { /* 32-bit int */
            int32_t s;
            memcpy(&s, p, 4);
            v = (float)s / 2147483648.0f;
        }
        buf[i] = v;
    }
    free(raw);
    d->pos_frames += frames;
    return frames;
}
static int wav_seek(decoder *d, double sec) {
    int frame_bytes = (d->wav_bits / 8) * d->channels;
    long target = (long)(sec * d->rate);
    long max = d->wav_data_len / frame_bytes;
    if (target > max) target = max;
    if (fseek(d->wf, d->wav_data_off + target * frame_bytes, SEEK_SET)) return -1;
    d->pos_frames = target;
    return 0;
}
static void wav_close(decoder *d) { if (d->wf) fclose(d->wf); }
static const decoder_ops WAV_OPS = { wav_read, wav_seek, wav_close };

static int wav_open(decoder *d, const char *path) {
    d->wf = fopen(path, "rb");
    if (!d->wf) return -1;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, d->wf) != 12 || memcmp(hdr, "RIFF", 4) ||
        memcmp(hdr + 8, "WAVE", 4))
        return -1;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, d->wf) != 8) return -1;
        uint32_t sz = rd32le(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t f[40];
            uint32_t take = sz < sizeof f ? sz : (uint32_t)sizeof f;
            if (fread(f, 1, take, d->wf) != take) return -1;
            if (sz > take) fseek(d->wf, sz - take, SEEK_CUR);
            uint16_t tag = rd16le(f);
            d->channels = rd16le(f + 2);
            d->rate = (int)rd32le(f + 4);
            d->wav_bits = rd16le(f + 14);
            d->wav_float = (tag == 3);
            if (tag == 0xFFFE && sz >= 40) { /* extensible: subformat first 2 bytes */
                uint16_t sub = rd16le(f + 24);
                d->wav_float = (sub == 3);
            }
            if (d->wav_bits != 16 && d->wav_bits != 24 && d->wav_bits != 32)
                return -1;
        } else if (!memcmp(ck, "data", 4)) {
            d->wav_data_off = ftell(d->wf);
            d->wav_data_len = (long)sz;
            break;
        } else {
            fseek(d->wf, sz + (sz & 1), SEEK_CUR);
        }
    }
    if (!d->rate || !d->channels) return -1;
    int frame_bytes = (d->wav_bits / 8) * d->channels;
    d->duration = (double)(d->wav_data_len / frame_bytes) / d->rate;
    d->ops = &WAV_OPS;
    return 0;
}

/* ---------------- MP3 (minimp3_ex, float output, gapless via LAME info) --- */
static long mp3_read(decoder *d, float *buf, long max_frames) {
    size_t want = (size_t)max_frames * (size_t)d->channels;
    size_t got = mp3dec_ex_read(&d->mp3, buf, want);
    long frames = (long)(got / (size_t)d->channels);
    d->pos_frames += frames;
    return frames;
}
static int mp3_seek(decoder *d, double sec) {
    uint64_t target = (uint64_t)(sec * d->rate) * (uint64_t)d->channels;
    if (mp3dec_ex_seek(&d->mp3, target)) return -1;
    d->pos_frames = (long)(sec * d->rate);
    return 0;
}
static void mp3_close(decoder *d) { if (d->mp3_open) mp3dec_ex_close(&d->mp3); }
static const decoder_ops MP3_OPS = { mp3_read, mp3_seek, mp3_close };

static int mp3_open(decoder *d, const char *path) {
    if (mp3dec_ex_open(&d->mp3, path, MP3D_SEEK_TO_SAMPLE)) return -1;
    d->mp3_open = 1;
    d->rate = d->mp3.info.hz;
    d->channels = d->mp3.info.channels;
    if (d->rate)
        d->duration = (double)(d->mp3.samples / (uint64_t)d->channels) / d->rate;
    d->ops = &MP3_OPS;
    return 0;
}

/* ---------------- public ---------------- */
decoder *decoder_open(const char *path, audio_fmt fmt) {
    decoder *d = xmalloc(sizeof *d);
    memset(d, 0, sizeof *d);
    int rc = (fmt == FMT_FLAC) ? flac_open(d, path)
           : (fmt == FMT_WAV)  ? wav_open(d, path)
           : (fmt == FMT_MP3)  ? mp3_open(d, path) : -1;
    if (rc) { decoder_close(d); return NULL; }
    return d;
}
long decoder_read(decoder *d, float *buf, long max_frames) {
    return d->ops->read(d, buf, max_frames);
}
int decoder_seek(decoder *d, double sec) {
    if (sec < 0) sec = 0;
    return d->ops->seek(d, sec);
}
void decoder_close(decoder *d) {
    if (!d) return;
    if (d->ops) d->ops->close(d);
    else {
        if (d->fl) { FLAC__stream_decoder_delete(d->fl); }
        if (d->wf) fclose(d->wf);
        if (d->mp3_open) mp3dec_ex_close(&d->mp3);
        free(d->pend);
    }
    free(d);
}
int decoder_rate(const decoder *d) { return d->rate; }
int decoder_channels(const decoder *d) { return d->channels; }
double decoder_duration(const decoder *d) { return d->duration; }
double decoder_position(const decoder *d) {
    return d->rate ? (double)d->pos_frames / d->rate : 0;
}
