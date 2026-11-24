#!/bin/sh
# Execution tests for tar: compile each prompt with nls, run the command in a
# freshly built tree, then run a check command and compare its output.
#
# tests/tar_exec_cases.tsv lines:  prompt<TAB>check<TAB>expected
#   check      shell command run after the nls command (e.g. list the archive
#              it created, or the folder it extracted into); $OUT holds the
#              command's own output
#   expected   the check's output (lines sorted, whitespace-normalized)
# Archive members are listed with a trailing "/" for directories stripped,
# so GNU tar and bsdtar list the same way.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/nls-tar.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
export COPYFILE_DISABLE=1   # macOS bsdtar: no ._ AppleDouble members

normalize() { sed 's|/$||' | LC_ALL=C sort | tr '\n' ' ' | tr -s ' \t' ' ' | sed 's/^ //; s/ $//'; }

have_zstd=0; command -v zstd >/dev/null 2>&1 && have_zstd=1

make_tree() {
    cd / && rm -rf "$W/t" && mkdir -p "$W/t" && cd "$W/t" || exit 1
    mkdir -p src/.git src/node_modules/pkg docs logs dist "my files" stage/release-1.0/bin
    printf 'int main(void) { return 0; }\n' > src/main.c
    printf '#define X 1\n' > src/util.h
    printf 'ref\n' > src/.git/HEAD
    printf 'x\n' > src/node_modules/pkg/index.js
    printf 'debug\n' > src/debug.log
    printf 'docs\n' > docs/readme.md
    printf 'a\n' > logs/app.log
    printf 'hi\n' > "my files/note.txt"
    printf '#!/bin/sh\n' > stage/release-1.0/bin/app
    printf 'readme\n' > stage/release-1.0/README
    (cd stage && tar -czf ../release.tgz release-1.0 && tar -cJf ../data.tar.xz release-1.0 \
        && tar -cf ../plain.tar release-1.0)
    [ $have_zstd -eq 1 ] && (cd stage && tar --zstd -cf ../logs.tar.zst release-1.0)
    rm -rf stage
}

pass=0; fail=0; skip=0
TAB=$(printf '\t')
while IFS="$TAB" read -r prompt check expected; do
    case "$prompt" in ''|'#'*) continue ;; esac
    case "$prompt" in *zst*) [ $have_zstd -eq 0 ] && { skip=$((skip+1)); continue; } ;; esac
    make_tree
    cmd=$("$NL" tar "$prompt" 2>/dev/null); rc=$?; [ $rc -ne 0 ] && cmd=$("$NL" tar "$prompt" 2>&1)
    if [ $rc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue
    fi
    OUT=$(eval "$cmd" 2>/dev/null); crc=$?
    if [ $crc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (tar exit %d): %s\n  command: %s\n' "$crc" "$prompt" "$cmd"
        continue
    fi
    actual=$(eval "$check" 2>&1 | normalize)
    if [ "$actual" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls tar "%s"\n  command:  %s\n  check:    %s\n  expected: %s\n  actual:   %s\n' \
            "$prompt" "$cmd" "$check" "$expected" "$actual"
    fi
done < "$DIR/tar_exec_cases.tsv"
printf 'tar exec: %d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ $fail -eq 0 ]
