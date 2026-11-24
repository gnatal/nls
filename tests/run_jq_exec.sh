#!/bin/sh
# Execution tests for jq: compile each prompt with nls, run the resulting
# command inside tests/fixtures/, and compare its output.
#
# tests/jq_exec_cases.tsv lines:  prompt<TAB>expected output
# Output is normalized before comparing: newlines -> spaces, runs of
# whitespace squeezed, leading/trailing space trimmed.
# An expected value of "!ERR" means nls itself must refuse the prompt.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)

if ! command -v jq >/dev/null 2>&1; then
    echo "jq not installed; skipping jq execution tests"
    exit 0
fi

normalize() { tr '\n' ' ' | tr -s ' \t' ' ' | sed 's/^ //; s/ $//'; }

pass=0; fail=0
TAB=$(printf '\t')
cd "$DIR/fixtures" || exit 1
while IFS="$TAB" read -r prompt expected; do
    case "$prompt" in ''|'#'*) continue ;; esac
    cmd=$("$NL" jq "$prompt" 2>/dev/null); rc=$?; [ $rc -ne 0 ] && cmd=$("$NL" jq "$prompt" 2>&1)
    if [ "$expected" = "!ERR" ]; then
        if [ $rc -ne 0 ]; then pass=$((pass+1)); else
            fail=$((fail+1)); printf 'FAIL: nls jq "%s" should be refused, got: %s\n' "$prompt" "$cmd"; fi
        continue
    fi
    if [ $rc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue
    fi
    actual=$(eval "$cmd" 2>&1 | normalize)
    if [ "$actual" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls jq "%s"\n  command:  %s\n  expected: %s\n  actual:   %s\n' \
            "$prompt" "$cmd" "$expected" "$actual"
    fi
done < "$DIR/jq_exec_cases.tsv"
printf 'jq exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
