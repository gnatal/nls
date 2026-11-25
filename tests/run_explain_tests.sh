#!/bin/sh
# --explain tests.
#
# 1. Every command nls can produce is explained in full: for each golden case
#    (tests/cases.tsv) and each template's example (nls --list), the output's
#    first line is the command nls prints without --explain, and no part is
#    marked "(no explanation)".
# 2. tests/explain_cases.tsv: tool<TAB>prompt<TAB>text that must start a
#    line of the explanation (leading spaces ignored).
NL=${1:-./nls}
DIR=$(dirname "$0")
pass=0; fail=0
TAB=$(printf '\t')

check_full() {   # tool prompt
    plain=$("$NL" "$1" "$2" 2>/dev/null) || return 0   # refusals: nothing to explain
    out=$("$NL" --explain "$1" "$2" 2>/dev/null)
    first=$(printf '%s\n' "$out" | head -n 1)
    if [ "$first" != "$plain" ]; then
        fail=$((fail+1))
        printf 'FAIL: nls --explain %s "%s": first line is not the command\n  command: %s\n  got:     %s\n' "$1" "$2" "$plain" "$first"
    elif printf '%s\n' "$out" | grep -qF '(no explanation)'; then
        fail=$((fail+1))
        printf 'FAIL: nls --explain %s "%s" leaves parts unexplained:\n%s\n' "$1" "$2" "$(printf '%s\n' "$out" | grep -F '(no explanation)')"
    else
        pass=$((pass+1))
    fi
}

while IFS="$TAB" read -r tool prompt expected; do
    case "$tool" in ''|'#'*) continue ;; esac
    [ "$expected" = "!ERR" ] && continue
    check_full "$tool" "$prompt"
done < "$DIR/cases.tsv"

examples=$(mktemp "${TMPDIR:-/tmp}/nls-explain.XXXXXX") || exit 1
trap 'rm -f "$examples"' EXIT
"$NL" --list | sed -n 's/^ *nls \([a-z]*\) "\(.*\)"$/\1	\2/p' > "$examples"
while IFS="$TAB" read -r tool prompt; do
    check_full "$tool" "$prompt"
done < "$examples"

while IFS="$TAB" read -r tool prompt want; do
    case "$tool" in ''|'#'*) continue ;; esac
    out=$("$NL" --explain "$tool" "$prompt" 2>/dev/null | sed 's/^ *//')
    found=false
    while IFS= read -r line; do
        case "$line" in "$want"*) found=true; break ;; esac
    done <<OUT
$out
OUT
    if $found; then pass=$((pass+1)); continue; fi
    fail=$((fail+1))
    printf 'FAIL: nls --explain %s "%s"\n  expected line: %s\n  output:\n%s\n' "$tool" "$prompt" "$want" "$(printf '%s\n' "$out" | sed 's/^/    /')"
done < "$DIR/explain_cases.tsv"

printf 'explain: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
