#!/bin/sh
# Golden tests: each line of tests/cases.tsv is  tool<TAB>prompt<TAB>expected
# An expected value of "!ERR" means nls must exit non-zero.
NL=${1:-./nls}
DIR=$(dirname "$0")
pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r tool prompt expected; do
    case "$tool" in ''|'#'*) continue ;; esac
    actual=$("$NL" "$tool" "$prompt" 2>/dev/null); rc=$?
    if [ "$expected" = "!ERR" ]; then
        if [ $rc -ne 0 ]; then pass=$((pass+1)); continue; fi
    elif [ $rc -eq 0 ] && [ "$actual" = "$expected" ]; then
        pass=$((pass+1)); continue
    fi
    fail=$((fail+1))
    printf 'FAIL: nls %s "%s"\n  expected: %s\n  actual:   %s (exit %d)\n' "$tool" "$prompt" "$expected" "$actual" "$rc"
done < "$DIR/cases.tsv"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
