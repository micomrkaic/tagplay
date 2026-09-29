/* This file is part of tagplay.
 * Copyright (C) 2026  Mico
 * GPL-3.0-or-later; see COPYING.
 *
 * The WASM build has no libcurl and no cross-origin luck: browsers
 * refuse most radio streams (CORS), so the honest port ships without
 * the dial. These stubs satisfy the decoder's radio references; a
 * station URL simply fails to open with a clear message.
 */

#ifdef __EMSCRIPTEN__

#include "radio.h"
#include <stddef.h>

#include <stdint.h>

radio_stream *radio_open(const char *url) { (void)url; return NULL; }
long radio_read(radio_stream *r, uint8_t *buf, size_t max,
                int timeout_ms) {
    (void)r; (void)buf; (void)max; (void)timeout_ms;
    return -1;
}
int radio_title(radio_stream *r, char *out, size_t sz) {
    (void)r; (void)out; (void)sz;
    return 0;
}
void radio_close(radio_stream *r) { (void)r; }
const char *radio_content_type(radio_stream *r) {
    (void)r;
    return "";
}

#else
typedef int radio_stub_native_translation_unit_is_empty;
#endif
