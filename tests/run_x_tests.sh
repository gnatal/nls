#!/bin/sh
# Tests for -x: run nls with an answer typed at its confirmation prompt, in
# a fresh git repository, then check the exit status, what nls printed and
# what happened to the files.
#
# tests/x_cases.tsv lines:  setup<TAB>answer<TAB>arguments<TAB>exit<TAB>check
#   setup      shell run in the tree first ("-" for none); it may cd elsewhere
#   answer     the line typed at the prompt: "enter" for an empty line, "-"
#              for no input at all
#   arguments  nls's arguments, as shell words
#   exit       nls's expected exit status
#   check      shell that must succeed afterwards ("-" for none); $OUT and
#              $ERR are files holding nls's stdout and stderr
#
# The tree, rebuilt for every case:
#   keep.txt src/a.c .gitignore ("*.tmp")   committed as "init"
#   a.log b.log sub/c.log                   untracked
#   x.tmp                                   ignored
# nls reads its answer from the terminal when there is one, so every run is
# detached from it (python3's os.setsid()) and reads the answer from stdin.
# HOME and git's global and system config are isolated, as in run_git_exec.sh.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)
command -v git >/dev/null 2>&1 || { echo "x tests: git not installed, skipped"; exit 0; }
if command -v python3 >/dev/null 2>&1; then
    notty() {
        python3 -I -c '
import os, sys
if os.getpgrp() == os.getpid():   # a process group leader cannot setsid(): fork first
    pid = os.fork()
    if pid:
        st = os.waitpid(pid, 0)[1]
        sys.exit(os.WEXITSTATUS(st) if os.WIFEXITED(st) else 128 + os.WTERMSIG(st))
os.setsid()
os.execvp(sys.argv[1], sys.argv[1:])' "$@"
    }
elif ! (: </dev/tty) 2>/dev/null; then
    notty() { "$@"; }   # no terminal to detach from
else
    echo "x tests: python3 not installed, skipped"; exit 0
fi

W=$(mktemp -d "${TMPDIR:-/tmp}/nls-x.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT

export HOME="$W/home" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL="$W/home/.gitconfig"
export GIT_PAGER=cat PAGER=cat GIT_TERMINAL_PROMPT=0 GIT_CEILING_DIRECTORIES="$W"
mkdir -p "$HOME"
git config --global user.name "Test User"
git config --global user.email "test@example.com"
git config --global init.defaultBranch main

make_tree() {
    cd / && rm -rf "$W/t" "$W/plain" "$W/origin.git" && mkdir -p "$W/t" && cd "$W/t" || exit 1
    git init -q .
    mkdir src sub
    printf 'keep\n' > keep.txt; printf 'int x;\n' > src/a.c; printf '*.tmp\n' > .gitignore
    git add -A && git commit -q -m init
    printf 'a\n' > a.log; printf 'b\n' > b.log; printf 'c\n' > sub/c.log; printf 'x\n' > x.tmp
}

OUT="$W/out" ERR="$W/err"
pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r setup answer args want check; do
    case "$setup" in ''|'#'*) continue ;; esac
    make_tree
    if [ "$setup" != "-" ] && ! eval "$setup" </dev/null >/dev/null 2>&1; then
        fail=$((fail+1)); printf 'FAIL (setup): %s\n' "$setup"; continue
    fi
    eval "set -- $args"
    case "$answer" in
        -)     notty "$NL" "$@" </dev/null >"$OUT" 2>"$ERR" ;;
        enter) printf '\n' | notty "$NL" "$@" >"$OUT" 2>"$ERR" ;;
        *)     printf '%s\n' "$answer" | notty "$NL" "$@" >"$OUT" 2>"$ERR" ;;
    esac
    rc=$?
    why=""
    if [ "$rc" != "$want" ]; then
        why="exit $rc, expected $want"
    elif [ "$check" != "-" ] && ! (eval "$check") </dev/null >/dev/null 2>&1; then
        why="check failed: $check"
    fi
    if [ -z "$why" ]; then pass=$((pass+1)); continue; fi
    fail=$((fail+1))
    printf 'FAIL: nls %s  (answer: %s)\n  %s\n  stdout: %s\n  stderr:\n%s\n' \
        "$args" "$answer" "$why" "$(cat "$OUT")" "$(sed 's/^/    /' "$ERR")"
done < "$DIR/x_cases.tsv"
printf -- '-x: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
