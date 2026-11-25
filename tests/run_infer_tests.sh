#!/bin/sh
# Tool inference tests (nls "<request>" with no tool).
#
# 1. tests/infer_cases.tsv: each line is  expected-tool<TAB>request ; the
#    tool is the command's first word (mkdir ... && magick counts as magick,
#    ffprobe as ffmpeg). "!ERR" means nls must refuse (exit non-zero).
# 2. Every golden case in tests/cases.tsv, given without its tool: nls may
#    refuse, or say on stderr that it guessed between tools, but a command
#    it prints without a word must be the golden one. A template change that
#    pulls a request to another tool shows up here.
NL=${1:-./nls}
DIR=$(dirname "$0")
pass=0; fail=0
TAB=$(printf '\t')

while IFS="$TAB" read -r want prompt; do
    case "$want" in ''|'#'*) continue ;; esac
    out=$("$NL" -- "$prompt" 2>/dev/null); rc=$?
    if [ "$want" = "!ERR" ]; then
        [ $rc -ne 0 ] && { pass=$((pass+1)); continue; }
    else
        case "$out" in "mkdir "*" && magick "*) tool=magick ;; *) tool=${out%% *} ;; esac
        [ "$tool" = ffprobe ] && tool=ffmpeg
        [ $rc -eq 0 ] && [ "$tool" = "$want" ] && { pass=$((pass+1)); continue; }
    fi
    fail=$((fail+1))
    printf 'FAIL: nls "%s"\n  expected tool: %s\n  actual:        %s (exit %d)\n' "$prompt" "$want" "$out" "$rc"
done < "$DIR/infer_cases.tsv"

err=$(mktemp "${TMPDIR:-/tmp}/nls-infer.XXXXXX") || exit 1
trap 'rm -f "$err"' EXIT
while IFS="$TAB" read -r tool prompt expected; do
    case "$tool" in ''|'#'*) continue ;; esac
    [ "$expected" = "!ERR" ] && continue
    out=$("$NL" -- "$prompt" 2>"$err"); rc=$?
    if [ $rc -ne 0 ] || [ "$out" = "$expected" ] || grep -q 'guessed the tool' "$err"; then
        pass=$((pass+1)); continue
    fi
    fail=$((fail+1))
    printf 'FAIL: nls "%s" (golden for %s) gave another command without a warning\n  golden: %s\n  actual: %s\n' \
        "$prompt" "$tool" "$expected" "$out"
done < "$DIR/cases.tsv"

printf 'inference: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
