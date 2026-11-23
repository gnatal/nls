#!/bin/sh
# Execution tests for grep: compile each prompt with nls, run the resulting
# command inside tests/fixtures/grep/, and compare its (sorted) output.
#
# tests/grep_exec_cases.tsv lines:  prompt<TAB>expected output
# Output is normalized before comparing: newlines -> spaces, runs of
# whitespace squeezed, leading/trailing space trimmed.
# An expected value of "!ERR" means nls itself must refuse the prompt.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)

if ! command -v grep >/dev/null 2>&1; then
    echo "grep not installed; skipping grep execution tests"
    exit 0
fi

normalize() { tr '\n' ' ' | tr -s ' \t' ' ' | sed 's/^ //; s/ $//'; }

pass=0; fail=0
TAB=$(printf '\t')
cd "$DIR/fixtures/grep" || exit 1
while IFS="$TAB" read -r prompt expected; do
    case "$prompt" in ''|'#'*) continue ;; esac
    cmd=$("$NL" grep "$prompt" 2>&1); rc=$?
    if [ "$expected" = "!ERR" ]; then
        if [ $rc -ne 0 ]; then pass=$((pass+1)); else
            fail=$((fail+1)); printf 'FAIL: nls grep "%s" should be refused, got: %s\n' "$prompt" "$cmd"; fi
        continue
    fi
    if [ $rc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue
    fi
    actual=$(eval "$cmd" 2>&1 | LC_ALL=C sort | normalize)
    if [ "$actual" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls grep "%s"\n  command:  %s\n  expected: %s\n  actual:   %s\n' \
            "$prompt" "$cmd" "$expected" "$actual"
    fi
done < "$DIR/grep_exec_cases.tsv"
printf 'grep exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
