#!/bin/sh
# Tests for the shell completions (completions/) and the man page (man/nls.1):
#   - they parse (bash -n, zsh -n, mandoc -T lint when installed);
#   - they list every option of `nls --help` and every supported tool, so
#     a new option or tool can't be forgotten in one of them;
#   - bash: _nls fills COMPREPLY as expected (any bash, 3.2 included);
#   - zsh: Tab in an interactive zsh (in a pseudo-terminal) completes.
NL=$(cd "$(dirname "${1:-./nls}")" && pwd)/$(basename "${1:-./nls}")
DIR=$(cd "$(dirname "$0")/.." && pwd)
BASH_COMP=$DIR/completions/nls.bash
ZSH_COMP=$DIR/completions/_nls
MAN=$DIR/man/nls.1
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); printf 'FAIL: %s\n' "$*"; }

# ---- they parse ----
if bash -n "$BASH_COMP"; then ok; else bad "bash -n $BASH_COMP"; fi
if command -v zsh >/dev/null 2>&1; then
    if zsh -n "$ZSH_COMP"; then ok; else bad "zsh -n $ZSH_COMP"; fi
fi
if command -v mandoc >/dev/null 2>&1; then
    if out=$(mandoc -T lint -W warning "$MAN" 2>&1) && [ -z "$out" ]; then ok
    else bad "mandoc -T lint: $out"; fi
fi

# ---- nothing missing ----
opts=$("$NL" --help | grep -oE -- '--[a-z][a-z-]*' | sort -u)
tools=$("$NL" --help | sed -n 's/^Supported tools: //p' | tr -d ',')
[ -n "$opts" ] && [ -n "$tools" ] || bad "can't read options and tools from nls --help"
for o in $opts; do
    grep -qF -e "$o" "$BASH_COMP" || bad "$BASH_COMP lacks $o"
    grep -qF -e "$o" "$ZSH_COMP" || bad "$ZSH_COMP lacks $o"
    m=$(printf '%s' "$o" | sed 's/-/\\\\-/g')
    grep -q -e "$m" "$MAN" || bad "$MAN lacks $o"
done
for t in $tools; do
    grep -qE "(^| |\")$t( |\")" "$BASH_COMP" || bad "$BASH_COMP lacks the tool $t"
    grep -qF -e "'$t:" "$ZSH_COMP" || bad "$ZSH_COMP lacks the tool $t"
    grep -qxF ".B $t" "$MAN" || bad "$MAN lacks the tool $t"
done
[ $fail -eq 0 ] && ok

# ---- bash: what _nls offers ----
# words before the one being completed (which is empty)<TAB>expected COMPREPLY, sorted
TAB=$(printf '\t')
while IFS="$TAB" read -r words want; do
    case "$words" in ''|'#'*) continue ;; esac
    got=$(bash --norc --noprofile -c '
        . "$1"; shift
        COMP_WORDS=("$@"); COMP_CWORD=$(( $# - 1 ))
        _nls
        printf "%s\n" "${COMPREPLY[@]}" | sort | tr "\n" " "' _ "$BASH_COMP" $words "")
    got=${got% }
    if [ "$got" = "$want" ]; then ok; else bad "bash: nls $words<Tab> gave '$got', expected '$want'"; fi
done <<'EOF'
nls	curl ffmpeg find git grep jq magick tar
nls -x	curl ffmpeg find git grep jq magick tar
nls --list	curl ffmpeg find git grep jq magick tar
nls --shell	bash zsh
nls ffmpeg
nls cut in.mp4
EOF
# a partial word: "nls --ex" and "nls g"
got=$(bash --norc --noprofile -c '. "$1"; COMP_WORDS=(nls --ex); COMP_CWORD=1; _nls; printf "%s " "${COMPREPLY[@]}"' _ "$BASH_COMP")
[ "$got" = "--exec --explain " ] && ok || bad "bash: nls --ex<Tab> gave '$got'"
got=$(bash --norc --noprofile -c '. "$1"; COMP_WORDS=(nls -v g); COMP_CWORD=2; _nls; printf "%s " "${COMPREPLY[@]}"' _ "$BASH_COMP")
[ "$got" = "grep git " ] || [ "$got" = "git grep " ] && ok || bad "bash: nls -v g<Tab> gave '$got'"

# ---- zsh: Tab in a real zsh ----
if command -v zsh >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    W=$(mktemp -d "${TMPDIR:-/tmp}/nls-comp.XXXXXX") || exit 1
    trap 'rm -rf "$W"' EXIT
    mkdir -p "$W/bin" "$W/home"
    ln -s "$NL" "$W/bin/nls"
    while IFS="$TAB" read -r typed want; do
        case "$typed" in ''|'#'*) continue ;; esac
        rm -f "$W/dump"
        python3 -I -c '
import os, pty, sys, time, select
typed, dump, comp, bindir, home = sys.argv[1:6]
env = dict(os.environ, PATH=bindir + ":" + os.environ["PATH"], HOME=home, TERM="dumb", PS1="$ ")
pid, fd = pty.fork()
if pid == 0:
    os.chdir(home)
    os.execvpe("zsh", ["zsh", "-f", "-i"], env)
def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: os.read(fd, 4096)
            except OSError: return
def send(s, wait=0.4):
    os.write(fd, s.encode()); pump(wait)
pump(0.5)
send("fpath=(%s $fpath); autoload -Uz compinit; compinit -u -D; "
     "nls-dump() { print -r -- \"$BUFFER\" > %s; }; zle -N nls-dump; bindkey \"^T\" nls-dump\n"
     % (comp, dump), 1.5)
send(typed, 0.3)
send("\t", 1.0)
send("\x14", 0.5)
for _ in range(30):
    if os.path.exists(dump): break
    pump(0.1)
send("\x15exit\n", 0.3)
try: os.kill(pid, 9)
except OSError: pass
os.waitpid(pid, 0)
' "$typed" "$W/dump" "$DIR/completions" "$W/bin" "$W/home"
        got=$(cat "$W/dump" 2>/dev/null)
        got=${got% }   # zsh adds a space after a completed word
        if [ "$got" = "$want" ]; then ok; else bad "zsh: '$typed'<Tab> gave '$got', expected '$want'"; fi
    done <<'EOF'
nls --sh	nls --shell
nls --shell b	nls --shell bash
nls ffm	nls ffmpeg
nls --list ma	nls --list magick
nls -x --expl	nls -x --explain
EOF
fi

printf 'completions: %d passed, %d failed\n' "$pass" "$fail"
[ $fail -eq 0 ]
