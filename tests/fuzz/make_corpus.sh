#!/bin/sh
# Seed corpus for the fuzzer: one file per test prompt, byte 0 picking the
# tool the way fuzz_nls.c reads it (tool id - 1), then the prompt.
#   sh tests/fuzz/make_corpus.sh build/fuzz-corpus
OUT=${1:?usage: make_corpus.sh <dir>}
DIR=$(dirname "$0")/..
mkdir -p "$OUT"
TAB=$(printf '\t')
n=0
cat "$DIR"/*.tsv | while IFS="$TAB" read -r tool prompt _rest; do
    case "$tool" in
        ffmpeg) b=0 ;; jq) b=1 ;; grep) b=2 ;; find) b=3 ;;
        tar) b=4 ;; git) b=5 ;; curl) b=6 ;; magick) b=7 ;;
        *) continue ;;
    esac
    [ -n "$prompt" ] || continue
    n=$((n+1))
    { printf "\\$(printf %03o "$b")"; printf '%s' "$prompt"; } > "$OUT/seed-$n"
done
# Spoken requests (--listen): byte 0's high bit set, so the harness
# normalizes them first.
while IFS="$TAB" read -r b said; do
    n=$((n+1))
    { printf "\\$(printf %03o $((b + 128)))"; printf '%s' "$said"; } > "$OUT/voice-$n"
done <<'SPOKEN'
5	Undo the last two commits, keep changes staged.
0	Cut input dot mp4 from 00:10 to 00:25 and scale to seven twenty p.
1	Get dot name from users dot JSON where dot age is greater than thirty.
4	Extract backup dot tar dot gz into restore slash.
3	Um, find files larger than one hundred megabytes in tilde slash downloads.
2	Search src slash for my underscore var dash two, ignoring case.
0	[BLANK_AUDIO] speed up clip dot mp4 by one point five (upbeat music)
6	Git, post data dot json to https colon slash slash api dot example dot com.
7	resize hero dot png to width eight hundred quality eighty-five
SPOKEN
# Inputs that once failed: replayed every time (see regressions/README).
for f in "$(dirname "$0")"/regressions/*; do
    case "$f" in */README) ;; *) cp "$f" "$OUT"/ ;; esac
done
ls "$OUT" | wc -l | awk '{print $1 " seeds in '"$OUT"'"}'
