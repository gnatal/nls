#!/bin/sh
# Warning tests: each line of tests/warn_cases.tsv is  tool<TAB>prompt<TAB>expected
# `expected` must appear in nls's stderr ("a|b": any of them); "-" means
# stderr must be empty. The command on stdout is checked by run_tests.sh.
NL=${1:-./nls}
DIR=$(dirname "$0")
pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r tool prompt expected; do
    case "$tool" in ''|'#'*) continue ;; esac
    err=$("$NL" "$tool" "$prompt" 2>&1 >/dev/null)
    ok=false
    if [ "$expected" = "-" ]; then
        [ -z "$err" ] && ok=true
    else
        rest=$expected
        while [ -n "$rest" ]; do
            alt=${rest%%|*}
            case "$err" in *"$alt"*) ok=true ;; esac
            [ "$alt" = "$rest" ] && break
            rest=${rest#*|}
        done
    fi
    if $ok; then pass=$((pass+1)); continue; fi
    fail=$((fail+1))
    printf 'FAIL: nls %s "%s"\n  expected stderr: %s\n  actual stderr:\n%s\n' "$tool" "$prompt" "$expected" "$err"
done < "$DIR/warn_cases.tsv"
printf 'warnings: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
