#!/bin/sh
# Paraphrase tests: each line of tests/paraphrase_cases.tsv is
#   tool<TAB>template id<TAB>prompt
# and nls must pick that template for the prompt (the "match:" line of -v)
# and build a command. A tool of "-" leaves it out: nls must also infer the
# template's tool. The point is the wording, not the command (cases.tsv
# checks that): a change to one template's keywords can't quietly pull
# another template's prompts away.
#
# A tool prefixed with "!" marks a known miss: it isn't a failure, but one
# that starts matching is reported so the "!" can come off.
NL=${1:-./nls}
DIR=$(dirname "$0")
pass=0; fail=0; miss=0; fixed=0
TAB=$(printf '\t')
while IFS="$TAB" read -r tool id prompt; do
    case "$tool" in ''|'#'*) continue ;; esac
    known=false
    case "$tool" in '!'*) known=true; tool=${tool#!} ;; esac
    if [ "$tool" = "-" ]; then
        err=$("$NL" -v "$prompt" 2>&1 >/dev/null); rc=$?
    else
        err=$("$NL" -v "$tool" "$prompt" 2>&1 >/dev/null); rc=$?
    fi
    got=$(printf '%s\n' "$err" | sed -n 's/^match: \([^ ]*\).*/\1/p' | tail -n 1)
    if [ $rc -eq 0 ] && [ "$got" = "$id" ]; then
        if $known; then
            fixed=$((fixed+1))
            printf 'now matches (take the "!" off): nls %s "%s" -> %s\n' "$tool" "$prompt" "$id"
        else
            pass=$((pass+1))
        fi
        continue
    fi
    if $known; then miss=$((miss+1)); continue; fi
    fail=$((fail+1))
    printf 'FAIL: nls %s "%s"\n  expected: %s\n  matched:  %s (exit %d)\n' \
        "$tool" "$prompt" "$id" "${got:-(none)}" "$rc"
done < "$DIR/paraphrase_cases.tsv"
printf 'paraphrase: %d passed, %d failed (%d known misses, %d of them now matching)\n' \
    "$pass" "$fail" "$((miss+fixed))" "$fixed"
[ $fail -eq 0 ]
