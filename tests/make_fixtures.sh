#!/bin/sh
# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.
# Build a synthetic test library under $1 (default ./testlib).
# Needs: flac, lame, python3
set -e
DEST=${1:-testlib}
py() { python3 -c "$1"; }
mkwav() { # path rate [tagged]
  python3 - "$1" "$2" "$3" <<'PY'
import struct, math, os, sys
path, rate, tagged = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1"
n = rate
data = b''.join(struct.pack('<h', int(3000*math.sin(2*math.pi*440*i/rate))) for i in range(n))
chunks  = b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate*2, 2, 16)
chunks += b'data' + struct.pack('<I', len(data)) + data
if tagged:
    body = b'INFO'
    for k, v in {'IART':'Nature','INAM':'Morning Swell','IPRD':'Sea Sessions',
                 'IGNR':'Field Recording','ICRD':'2021'}.items():
        vb = v.encode() + b'\0'
        if len(vb) % 2: vb += b'\0'
        body += k.encode() + struct.pack('<I', len(vb)) + vb
    chunks += b'LIST' + struct.pack('<I', len(body)) + body
os.makedirs(os.path.dirname(path), exist_ok=True)
open(path, 'wb').write(b'RIFF' + struct.pack('<I', 4+len(chunks)) + b'WAVE' + chunks)
PY
}
mkstereo() { # tmp wav for encoders: rate
  python3 - "$1" <<'PY'
import struct, math, sys
rate = int(sys.argv[1]); n = rate
data = b''.join(struct.pack('<hh', *(int(3000*math.sin(2*math.pi*440*i/rate)),)*2) for i in range(n))
open('/tmp/_fixture.wav','wb').write(
    b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '
    +struct.pack('<IHHIIHH',16,1,2,rate,rate*4,4,16)
    +b'data'+struct.pack('<I',len(data))+data)
PY
}
mkflac() { # path artist album title genre date track rate
  mkdir -p "$(dirname "$1")"; mkstereo "$8"
  flac -s -f -o "$1" -T "ARTIST=$2" -T "ALBUM=$3" -T "TITLE=$4" \
       -T "GENRE=$5" -T "DATE=$6" -T "TRACKNUMBER=$7" /tmp/_fixture.wav
}
mkflac "$DEST/Bach/Sonatas and Partitas/01 Partita No 2 Chaconne.flac" \
  "Johann Sebastian Bach" "Sonatas and Partitas" "Partita No. 2: Chaconne" Baroque 1720 1 44100
mkflac "$DEST/Bach/Sonatas and Partitas/02 Sonata No 1 Adagio.flac" \
  "Johann Sebastian Bach" "Sonatas and Partitas" "Sonata No. 1: Adagio" Baroque 1720 2 44100
mkflac "$DEST/Bach/Violin Concertos/01 Concerto in A minor.flac" \
  "Johann Sebastian Bach" "Violin Concertos" "Violin Concerto in A minor, BWV 1041" Baroque 1730 1 96000
mkflac "$DEST/Offenbach/Overtures/01 Orpheus.flac" \
  "Jacques Offenbach" "Overtures" "Orpheus in the Underworld: Overture" Romantic 1858 1 44100
mkflac "$DEST/Glenn Gould/Goldberg 1981/01 Aria.flac" \
  "Glenn Gould" "Goldberg Variations 1981" "Goldberg Variations, BWV 988: Aria" Baroque 1981 1 48000
mkdir -p "$DEST/Duos"; mkstereo 44100
flac -s -f -o "$DEST/Duos/01 Duo.flac" -T "ARTIST=Anne-Sophie Mutter" -T "ARTIST=Yo-Yo Ma" \
     -T "TITLE=Imaginary Duo" -T "ALBUM=Duets" -T "GENRE=Chamber" -T "DATE=2019" /tmp/_fixture.wav
mkwav "$DEST/Field Recordings/Sea/01 Morning Swell.wav" 44100 1
mkwav "$DEST/Nature/Storms/02 Thunder at Dusk.wav" 48000 0
mkdir -p "$DEST/Radiohead/OK Computer"; mkstereo 44100
lame --quiet --tt "Paranoid Android" --ta "Radiohead" --tl "OK Computer" \
     --ty 1997 --tn 2 --tg "Alternative" /tmp/_fixture.wav \
     "$DEST/Radiohead/OK Computer/02 Paranoid Android.mp3"
echo "fixtures in $DEST"
