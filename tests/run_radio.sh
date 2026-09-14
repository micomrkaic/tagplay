#!/bin/sh
# AAC/ICY radio decoder battery: replays REAL captured broadcaster bytes
# (tests/data/*-join.bin, deliberately starting mid-frame like a live
# join) over local HTTP at starved bitrate, through the full
# radio+decoder stack. Requires: libfaad-dev, python3.
# Usage: tests/run_radio.sh    (from the repo root)
set -e
cc -Isrc/core -Isrc/play -std=c17 -O2 -D_GNU_SOURCE -DHAVE_FAAD \
   $(pkg-config --cflags flac libpcre2-8 sdl2 libcurl) \
   -o /tmp/tp_radio_probe tests/radio_probe.c \
   src/play/decoder.c src/play/radio.c src/play/dsp.c \
   src/play/effects.c src/play/engine.c src/core/util.c src/core/track.c \
   $(pkg-config --libs flac libpcre2-8 sdl2 libcurl) -lfaad -lpthread -lm
fail=0
run_case() {
    port=$1; fixture=$2; ct=$3; label=$4
    python3 tests/radioserver.py "$port" "$fixture" "$ct" & srv=$!
    sleep 0.7
    if env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
         /tmp/tp_radio_probe "http://127.0.0.1:$port/s" >/tmp/tp_radio_out 2>&1
    then echo "ok $label: $(tr '\n' ' ' < /tmp/tp_radio_out)"
    else echo "FAIL $label: $(tr '\n' ' ' < /tmp/tp_radio_out)"; fail=1
    fi
    kill $srv 2>/dev/null; wait $srv 2>/dev/null || true
}
run_case 18861 tests/data/val202-join.bin audio/aac  "val202 mid-frame join"
run_case 18862 tests/data/prvi-join.bin   audio/aac  "prvi mid-frame join"
run_case 18863 tests/data/val202-join.bin audio/mpeg "lying content-type"
exit $fail
