#!/bin/sh
# Execution tests for git: compile each prompt with nls, run the command in a
# freshly built throwaway repository, then run a check command and compare
# its output.
#
# tests/git_exec_cases.tsv lines:  setup<TAB>prompt<TAB>check<TAB>expected
#   setup      shell run before the nls command ("-" for none), e.g. to
#              create an untracked file or make a reset to undo
#   check      shell run after it; $OUT holds the command's own output
#   expected   the check's output (lines sorted, whitespace-normalized)
#
# The repository lives in a temp dir with HOME, global and system config
# isolated, so nothing here can touch the user's own repos or settings.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")" && pwd)
command -v git >/dev/null 2>&1 || { echo "git exec: git not installed, skipped"; exit 0; }
W=$(mktemp -d "${TMPDIR:-/tmp}/nls-git.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT

export HOME="$W/home" GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL="$W/home/.gitconfig"
# editor stub: writes a message into an empty commit message, leaves rebase
# todo lists (which already have content) alone
printf '#!/bin/sh\ngrep -qv "^#" "$1" 2>/dev/null && grep -v "^#" "$1" | grep -q . || echo "edited message" > "$1"\n' > "$W/editor"
chmod +x "$W/editor"
export GIT_PAGER=cat PAGER=cat GIT_EDITOR="$W/editor" GIT_TERMINAL_PROMPT=0
export GIT_CEILING_DIRECTORIES="$W"
mkdir -p "$HOME"
git config --global user.name "Test User"
git config --global user.email "test@example.com"
git config --global init.defaultBranch main
git config --global advice.detachedHead false

normalize() { LC_ALL=C sort | tr '\n' ' ' | tr -s ' \t' ' ' | sed 's/^ //; s/ $//'; }

# commit AS DATE MESSAGE
c() {
    GIT_AUTHOR_NAME="$1" GIT_AUTHOR_EMAIL="$1@example.com" GIT_AUTHOR_DATE="$2T12:00:00" \
    GIT_COMMITTER_NAME="$1" GIT_COMMITTER_EMAIL="$1@example.com" GIT_COMMITTER_DATE="$2T12:00:00" \
        git commit -q -m "$3"
}

# History on main (oldest first):
#   initial (alice)    src/a.c=a1, old.txt, README.md, .gitignore
#   add b (bob)        src/a.c=a2, src/b.c (contains "needle")      <- branch "stable"
#   remove old (alice) rm old.txt
#   tweak a (bob)      src/a.c=a3
#   update b (alice)   src/b.c += more
# "feature" = main + "feature work" (feat.txt). Untracked: nothing.
make_repo() {
    cd / && rm -rf "$W/r" && mkdir -p "$W/r" && cd "$W/r" || exit 1
    git init -q .
    mkdir src
    printf 'a1\n' > src/a.c; printf 'old\n' > old.txt; printf '# demo\n' > README.md
    printf 'build/\n' > .gitignore
    git add -A && c alice 2024-01-01 "initial"
    printf 'a2\n' > src/a.c; printf 'needle\n' > src/b.c
    git add -A && c bob 2024-02-01 "add b"
    git branch stable
    git rm -q old.txt && c alice 2024-03-01 "remove old"
    printf 'a3\n' > src/a.c
    git add -A && c bob 2024-04-01 "tweak a"
    printf 'more\n' >> src/b.c
    git add -A && c alice 2024-05-01 "update b"
    git switch -q -c feature
    printf 'feat\n' > feat.txt
    git add -A && c bob 2024-05-02 "feature work"
    git switch -q main
}

pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r setup prompt check expected; do
    case "$setup" in ''|'#'*) continue ;; esac
    make_repo
    [ "$setup" != "-" ] && { eval "$setup" >/dev/null 2>&1 || { fail=$((fail+1)); printf 'FAIL (setup): %s\n' "$setup"; continue; }; }
    cmd=$("$NL" git "$prompt" 2>/dev/null); rc=$?; [ $rc -ne 0 ] && cmd=$("$NL" git "$prompt" 2>&1)
    if [ $rc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (nls error): %s\n  %s\n' "$prompt" "$cmd"; continue
    fi
    OUT=$(eval "$cmd" 2>/dev/null); crc=$?
    if [ $crc -ne 0 ]; then
        fail=$((fail+1)); printf 'FAIL (git exit %d): %s\n  command: %s\n' "$crc" "$prompt" "$cmd"
        continue
    fi
    export OUT
    actual=$(eval "$check" 2>&1 | normalize)
    if [ "$actual" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL: nls git "%s"\n  command:  %s\n  check:    %s\n  expected: %s\n  actual:   %s\n' \
            "$prompt" "$cmd" "$check" "$expected" "$actual"
    fi
done < "$DIR/git_exec_cases.tsv"
printf 'git exec: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
