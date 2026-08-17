# tagplay

Library scanner, tag cache, query engine, live-filtering REPL, playback,
manual track selection, and saved playlists. Decoder vtable (FLAC / WAV /
MP3) feeds an audio thread through the DSP insert chain into SDL2
(CoreAudio on macOS, ALSA/PipeWire on Linux) at each track's native rate.
Gapless within same-rate runs (device stays open); rate changes drain and
reopen. MP3 plays gapless via minimp3_ex's LAME delay/padding trim.

Portable: no glibc-isms (qsort_r replaced by a thread-local shim), SDL2
audio. Builds on Linux now; macOS should need only `brew install flac
pcre2 sdl2 pkg-config` and `make`.

Working title `tagplay`; rename at will (one string in `main.c`, the
Makefile target, and the cache dir).

## Build

    make            # Linux: libflac-dev libpcre2-dev libsdl2-dev
                    # macOS: brew install flac pcre2 sdl2 pkg-config
    make install    # copies to ~/.local/bin

## Run

    tagplay ~/music                  # interactive REPL, live match count
    tagplay -q 'bach violin' ~/music # one-shot: print matching paths
    tagplay -q 'year<1800' -s year,album,track -t ~/music   # sorted, tabular
    tagplay -n ...                   # ignore cache, scan fresh

First run scans everything (FLAC via libFLAC metadata chain; WAV via RIFF
fmt/LIST-INFO; MP3 via built-in ID3v2.3/2.4 text-frame reader + Xing/CBR
duration). Results go to ~/.cache/tagplay/cache.bin; later runs re-read
tags only for files whose mtime or size changed.

## Query syntax

    bare words              case-insensitive substring over all tags + path;
                            juxtaposition is implicit AND: bach violin partita
    field ~ "regex"         PCRE2, case-insensitive, UTF-8
    field = value           exact, case-insensitive; != negates
    year<1990  length>=3:00  rate=96000  track<=3     numeric; mm:ss works
    & | ! ( )               boolean; ',' is a synonym for '&'

Fields: any tag key (ARTIST, ALBUM, GENRE, COMPOSER, ...) plus pseudo-fields
`path`, `format` (flac/wav/mp3), `length`, `rate`, `channels`, `year`,
`track`, `disc`. Multi-valued tags match if ANY value matches; `!=` means
no value matches. Untagged files get TITLE/ALBUM/ARTIST synthesized from
their path and `SOURCE=path` so you can find them: `source=path`.

Quote regexes containing spaces, parens, or `|`. Wrapping a whole
expression in quotes is forgiven: `'year < 1970'` parses as the
comparison (quotes are stripped when the quoted text contains an
operator); operator-free quoted phrases like `'dark side'` stay literal
substring searches.

## REPL

Type — the count and preview update per keystroke; count shows `N → M` on
change and dims while the expression is mid-edit (unparseable). The
display uses the whole terminal: the result list grows to the window
height and titles run the full width. While playing, the status area
shows an ASCII VU meter (post-DSP peak, dB-scaled, per channel) and a
90s-CD-player marquee that scrolls long artist/title lines; both update
at 10 Hz. The count
line shows both populations: `132 tracks · 9h14m   selected: 17 · 1h02m`.
**Enter
plays the selection if one exists, else the current result set** (replaces
the queue, clears the line). A
status line shows track, position, queue slot, rate, and dsp mode, and
refreshes once a second while playing. Ctrl-U clears, Ctrl-W deletes a
word, arrows/Home/End/Delete work.

    Tab                      cycles query -> list -> queue view (when
                             something is queued) -> query. Esc returns to
                             the query from anywhere.

    Queue view               opens automatically when you press play: shows
                             the actual play queue with a ▶ marker on the
                             current track, total and remaining duration
                             ("queue: 50 · 3h12m · 1h04m left"). j/k/arrows
                             move the cursor; Enter jumps playback to the
                             cursored track; Space pauses/resumes; left and
                             right arrows seek ∓/+10 s; r restarts the
                             track; s stops (queue kept); J/K move the
                             cursored track down/up to reorder the queue,
                             and :save from here saves the queue in its
                             current (reordered) order. +/-/m volume work
                             here too; typing drops you back into search.

    List mode (via Tab)      curation over search results: rows show "  3 [x] artist — title";
                             j/k/arrows/PgUp/PgDn/g/G move, Space toggles [x]
                             (and advances), a adds all matches, i inverts the
                             selection within the matches, c clears, Enter
                             plays, ':' starts a command, Esc/Tab back.
                             Selection survives query changes — search, tick,
                             search again, tick more: that's playlist building.
    :save name               save selection (or matches if none) as .m3u
    :load name               load playlist into the selection
    :lists                   show saved playlists      :clear  drop selection
    :ls                      list all matches (with [x] markers)
    :p  :n  :b  :stop        pause/resume, next, prev, stop
    Ctrl-P / Ctrl-N / Ctrl-B same, without touching the query line
    :seek 1:23               seek in the current track
    :vol 80                  volume percent (0-200, software gain); bare :vol
                             reports. In list mode: + / - nudge by 5%, m mutes
                             (remembers and restores the previous level).
                             Current volume shows in the playing status line.
    :dsp tube 0.4            dsp mode + amount; :dsp off
    :sort year,album,track   sort spec; -field for descending; :sort clears
    :stats                   tag-key frequency across the library
    :help                    syntax reminder
    :q                       quit

If no sound device opens, tagplay runs a "null output" that paces at
realtime (status shows NO AUDIO DEVICE) — useful for testing over ssh.
Set TAGPLAY_DEBUG=1 to log keys/commands to /tmp/tagplay.log.

## Layout

    src/util.c       vectors, strings, file slurp, duration formatting
    src/track.c      track model: multi-valued tags, table
    src/tags_flac.c  libFLAC metadata chain + path-fallback synthesis
    src/tags_wav.c   RIFF walk: fmt/data/LIST-INFO
    src/tags_mp3.c   minimal ID3v2.3/2.4 (unsync, UTF-16, TXXX, multi-value)
                     + MPEG header/Xing duration
    src/scan.c       recursive walk, magic-byte probe, cache-aware dispatch
    src/cache.c      versioned binary cache, atomic rename on save
    src/query.c      lexer → tolerant recursive-descent parser → AST →
                     PCRE2 evaluator; multi-field sort
    src/repl.c       raw-mode line editor, per-keystroke re-query, preview,
                     transport commands, 1 Hz status refresh (select timeout)
    src/decoder.c    decoder vtable: libFLAC stream decoder, WAV reader
                     (16/24/32-bit + float), minimp3_ex; all emit
                     interleaved float32; seek on every format
    src/dsp.c        insert chain socket for Audiotard: gain + demo "tube"
                     tanh saturation; state survives track boundaries
    src/player.c     audio thread (pthread), command mailbox, SDL_QueueAudio
                     push output with 0.5 s high-watermark throttle, queue
                     flush on transport commands, per-track native rate,
                     gapless same-rate, null-output fallback
    src/main.c       args, cache orchestration, -q one-shot, -D debug decode

The query engine never touches audio or the terminal: query.c maps
(string, table) → index list and is unit-testable standalone.

## Tests

tests/make_fixtures.sh builds a synthetic library (tagged FLAC at three
sample rates, a multi-valued-ARTIST FLAC, tagged + untagged WAV, an
ID3v2 MP3) and tests/run.sh exercises the engine through -q.

Playlists are plain .m3u (EXTM3U + EXTINF + absolute paths) in
~/.config/tagplay/playlists/ — readable by any other player, and loading
matches paths via realpath so relative scans still resolve.

## Audiotard (wired in)

The DSP is audiotard 0.6.6: effects.c / engine.c vendored verbatim
(GPL-3, hence COPYING), driven by tagplay's own port of audiotard's
streaming producer (the wasm worker / GTK live-mode recipe): 4096-frame
blocks rendered with 16384 frames of pre-roll context (2048 for
shaper-only) so filters, delay lines and noise envelopes settle on real
signal; 512-frame crossfades at seams; a 512-frame pad past the emitted
region because render tails are FIR-edge-corrupted; wow/flutter LFOs
phase-continuous across blocks via t0; one constant RMS-match gain
(x0.708 headroom) measured on the first block. Causal streaming costs
~100 ms of one-time priming latency (emitted as silence, absorbed by the
SDL queue). Pipeline state survives same-format track joins, so the
"tape" rolls through gapless boundaries; it resets only on real rate or
channel changes.

    :dsp tube 0.5     biased-tanh waveshaper, 8x oversampled
    :dsp tape 0.5     wow/flutter/drift, hiss, head bump, HF loss
    :dsp vinyl 0.5    wow/drift, crackle (Poisson, 3 kHz ring), pink hiss
    :dsp off

amount 0.5 == audiotard's calibrated defaults; the knob scales the
modulation depths and noise levels around them (noise in dB, depths
linearly).

## Roadmap

  next  Audiotard wired into dsp.c; per-playlist dsp presets
  later saved *queries* as smart playlists; richer TUI if the itch comes

## License

GPL-3.0-or-later; see COPYING. effects.c/engine.c are audiotard 0.6.6
(same author, same license). minimp3 (third_party header, vendored as
src/minimp3*.h) is CC0/public domain and keeps its own notice.

## Publishing

./publish.sh ["commit message"] pushes to github.com/micomrkaic/tagplay
(SSH remote by default; TAGPLAY_HTTPS=1 for HTTPS).
