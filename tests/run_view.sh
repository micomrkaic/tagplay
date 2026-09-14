#!/bin/sh
# tagview battery: EXIF + IPTC + XMP + PNG metadata through scan,
# merge, and the query engine. Usage: tests/run_view.sh PHOTODIR
set -e
DIR="${1:-phototest}"
[ -x ./tagview ] || { echo "build tagview first"; exit 1; }
fail=0
chk() {
    got=$(./tagview -n -q "$1" "$DIR" 2>&1 >/dev/null | sed 's/ images//')
    if [ "$got" = "$2" ]; then echo "ok  [$1] = $2"
    else echo "FAIL [$1] want $2 got $got"; fail=1; fi
}
chk 'tag=alps' 3
chk 'tag=alps & year<2000' 1
chk 'camera~canon' 2
chk 'camera~iphone & tag=lake' 1
chk 'width>4000' 3
chk 'mpix>10' 4
chk 'format=png' 1
chk 'tag=terminal' 1
chk 'city=Bohinj' 1
chk 'tag=summer' 1
chk 'tag=alps & tag=hiking' 1
chk 'artist=Mico' 1
chk 'title~ljubljana' 1
chk 'year=2021' 2
chk 'tag~.' 6
exit $fail
