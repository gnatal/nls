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
# Inputs that once failed: replayed every time (see regressions/README).
for f in "$(dirname "$0")"/regressions/*; do
    case "$f" in */README) ;; *) cp "$f" "$OUT"/ ;; esac
done
ls "$OUT" | wc -l | awk '{print $1 " seeds in '"$OUT"'"}'
