#!/bin/sh
# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.
# Minimal regression checks against the fixture library. Usage: tests/run.sh [libdir]
set -e
LIB=${1:-testlib}
TP="./tagplay -n"
fail=0
chk() { # desc expected_count query...
  desc=$1; want=$2; shift 2
  got=$($TP -q "$@" "$LIB" 2>/dev/null | wc -l)
  if [ "$got" -eq "$want" ]; then echo "ok   $desc"
  else echo "FAIL $desc: want $want got $got"; fail=1; fi
}
chk "all tracks"              9 'length<0:30'
chk "bare substring bach"     4 'bach'
chk "implicit AND"            1 'bach violin'
chk "regex + negation"        3 'artist~"bach" & !offenbach'
chk "numeric year"            3 'year<1800'
chk "rate exact"              1 'rate=96000'
chk "format pseudo-field"     2 'format=wav'
chk "multi-valued artist"     1 'artist="Yo-Yo Ma"'
chk "id3v2 mp3"               1 'radiohead'
chk "path-fallback tags"      1 'thunder'
chk "OR"                      2 'gould | radiohead'
chk "quoted expression"       3 "'year < 1800'"
chk "quoted phrase substring" 1 "'No. 1: Adagio'"
exit $fail
