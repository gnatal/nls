#!/bin/sh
# Execution tests for find: compile each prompt with nls, run the command on
# a freshly built tree, and compare the (sorted) result.
#
# tests/find_exec_cases.tsv lines:  prompt<TAB>expected
#   expected            the command's output (lines sorted, whitespace-normalized)
#   removed: <paths>    for destructive prompts: the paths that are gone afterwards.
#                       The prompt's dry run (from `nls -v`) is run first and must
#                       list exactly those paths (a listed directory covers its
#                       contents). Deleting empty directories also takes parents
#                       emptied along the way, which no dry run can list, so the
#                       tree has no nested empty directories.
#   !ERR                nls itself must refuse the prompt
# The tree is rebuilt in a temp dir for every case: sizes and modification
# times can't be stored in git.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/nls-find.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT

normalize() { tr '\n' ' ' | tr -s ' \t' ' ' | sed 's/^ //; s/ $//'; }

make_tree() {
    cd / && rm -rf "$W/t" && mkdir -p "$W/t" && cd "$W/t" || exit 1
    mkdir -p src/build src/lib node_modules/pkg emptydir logs
    printf 'int main(void) { return 0; }\n' > src/main.c
    printf '#define X 1\n' > src/util.h
    printf 'int gen;\n' > src/build/gen.c
    printf 'x\n' > src/app.js
    printf 'x\n' > node_modules/pkg/index.js
    printf 'x\n' > .DS_Store
    printf 'x\n' > src/.DS_Store
    printf 'notes\n' > README.md
    : > empty.txt
    dd if=/dev/zero of=src/lib/big.bin bs=1024 count=2048 2>/dev/null
    printf 'new\n' > logs/new.log
    printf '1\n2\n3\n4\n5\n' > logs/long.txt
    printf 'old\n' > logs/old.log
    touch -t 202001010000 logs/old.log
}

pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r prompt expected; do
    case "$prompt" in ''|'#'*) continue ;; esac
    make_tree
    cmd=$("$NL" find "$prompt" 2>/dev/null); rc=$?; [ $rc -ne 0 ] && cmd=$("$NL" find "$prompt" 2>&1)
    if [ "$expected" = "!ERR" ]; then
        if [ $rc -ne 0 ]; then pass=$((pass+1)); else
            fail=$((fail+1)); printf 'FAIL: nls find "%s" should be refused, got: %s\n' "$prompt" "$cmd"; fi
        continue
    fi
    if [ $rc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue
    fi
    case "$expected" in
    'removed: '*)
        expected=${expected#removed: }
        dry=$("$NL" -v find "$prompt" 2>&1 >/dev/null | sed -n 's/^dry run: //p')
        if [ -z "$dry" ]; then
            fail=$((fail+1)); printf 'FAIL (no dry run): nls find "%s"\n' "$prompt"; continue
        fi
        eval "$dry" 2>/dev/null | sed 's|^\./||' > "$W/listed"
        command find . | LC_ALL=C sort > "$W/before"
        eval "$cmd" >/dev/null 2>&1
        command find . | LC_ALL=C sort > "$W/after"
        actual=$(LC_ALL=C comm -23 "$W/before" "$W/after" | normalize)
        LC_ALL=C comm -23 "$W/before" "$W/after" | sed 's|^\./||' > "$W/removed"
        # paths removed but not listed (nor inside a listed directory), and
        # paths listed but not removed
        off=$(awk 'FILENAME == ARGV[1] { listed[$0] = 1; next }
                   { gone[$0] = 1; ok = $0 in listed
                     for (p in listed) if (index($0, p "/") == 1) ok = 1
                     if (!ok) bad = bad " " $0 " (not listed)" }
                   END { for (p in listed) if (!(p in gone)) bad = bad " " p " (not removed)"
                         print bad }' "$W/listed" "$W/removed")
        if [ -n "$off" ]; then
            fail=$((fail+1))
            printf 'FAIL (dry run): nls find "%s"\n  dry run: %s\n  mismatch:%s\n' "$prompt" "$dry" "$off"
            continue
        fi ;;
    *)
        actual=$(eval "$cmd" 2>&1 | LC_ALL=C sort | normalize) ;;
    esac
    if [ "$actual" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls find "%s"\n  command:  %s\n  expected: %s\n  actual:   %s\n' \
            "$prompt" "$cmd" "$expected" "$actual"
    fi
done < "$DIR/find_exec_cases.tsv"
printf 'find exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
