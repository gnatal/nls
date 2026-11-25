# bash completion for nls (bash 3.2 or later).
# Installed by `make install` into share/bash-completion/completions/, where
# bash-completion finds it; or source it from ~/.bashrc.
_nls() {
    local cur prev i word tool=""
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}
    local tools="ffmpeg jq grep find tar git curl magick"
    local opts="-x --exec -y --yes --force -e --explain -v --verbose -l --list --shell
                -h --help -V --version"
    COMPREPLY=()

    case $prev in
        -l|--list) COMPREPLY=($(compgen -W "$tools" -- "$cur")); return 0 ;;
        --shell)   COMPREPLY=($(compgen -W "zsh bash" -- "$cur")); return 0 ;;
    esac

    # The first word that isn't an option is the tool, or the start of a
    # request whose tool nls infers. After it, only file names help.
    for ((i = 1; i < COMP_CWORD; i++)); do
        word=${COMP_WORDS[i]}
        case $word in
            --) tool=-; break ;;
            -*) ;;
            *)  tool=$word; break ;;
        esac
    done
    if [ -n "$tool" ]; then
        return 0   # -o default: file names
    fi
    case $cur in
        -*) COMPREPLY=($(compgen -W "$opts" -- "$cur")) ;;
        *)  COMPREPLY=($(compgen -W "$tools" -- "$cur")) ;;
    esac
    return 0
}
complete -o default -F _nls nls
