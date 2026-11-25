#!/bin/sh
# Install a prebuilt nls release.
#
#   curl -fsSL https://raw.githubusercontent.com/gnatal/nls/main/install.sh | sh
#
# Environment:
#   NLS_VERSION=0.1.0     a specific release (default: the latest)
#   PREFIX=/usr/local     install to $PREFIX/bin (default: ~/.local; may need sudo)
#   BIN_NAME=other        install under a different command name
set -eu

REPO="gnatal/nls"
PREFIX="${PREFIX:-$HOME/.local}"
BIN_NAME="${BIN_NAME:-nls}"
BINDIR="$PREFIX/bin"

die() { echo "nls install: $*" >&2; exit 1; }

case "$(uname -s)" in
    Darwin) target="macos-universal" ;;
    Linux)
        case "$(uname -m)" in
            x86_64 | amd64) target="linux-x86_64" ;;
            aarch64 | arm64) target="linux-arm64" ;;
            *) die "no prebuilt binary for Linux $(uname -m); build from source: make && make install" ;;
        esac ;;
    *) die "no prebuilt binary for $(uname -s); build from source: make && make install" ;;
esac

command -v curl >/dev/null 2>&1 || die "curl is required"

if [ -n "${NLS_VERSION:-}" ]; then
    tag="v${NLS_VERSION#v}"
else
    # github.com/<repo>/releases/latest redirects to .../releases/tag/<tag>
    url="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest")" ||
        die "couldn't find the latest release"
    tag="${url##*/}"
    case "$tag" in v[0-9]*) ;; *) die "couldn't find the latest release (got '$tag')" ;; esac
fi
version="${tag#v}"
name="nls-$version-$target"
base="https://github.com/$REPO/releases/download/$tag"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "downloading $name.tar.gz"
curl -fsSL -o "$tmp/$name.tar.gz" "$base/$name.tar.gz" || die "download failed: $base/$name.tar.gz"
curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS" || die "download failed: $base/SHA256SUMS"

expected="$(awk -v f="$name.tar.gz" '$2 == f { print $1 }' "$tmp/SHA256SUMS")"
[ -n "$expected" ] || die "$name.tar.gz is not listed in SHA256SUMS"
if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$tmp/$name.tar.gz" | awk '{ print $1 }')"
else
    actual="$(shasum -a 256 "$tmp/$name.tar.gz" | awk '{ print $1 }')"
fi
[ "$actual" = "$expected" ] || die "checksum mismatch for $name.tar.gz"

tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
mkdir -p "$BINDIR"
install -m 755 "$tmp/$name/nls" "$BINDIR/$BIN_NAME"
echo "installed $BINDIR/$BIN_NAME ($("$BINDIR/$BIN_NAME" --version))"
# The man page and completions (releases from 0.2.0 on carry them).
if [ -f "$tmp/$name/man/nls.1" ]; then
    mkdir -p "$PREFIX/share/man/man1" "$PREFIX/share/bash-completion/completions" \
             "$PREFIX/share/zsh/site-functions"
    install -m 644 "$tmp/$name/man/nls.1" "$PREFIX/share/man/man1/$BIN_NAME.1"
    install -m 644 "$tmp/$name/completions/nls.bash" "$PREFIX/share/bash-completion/completions/$BIN_NAME"
    install -m 644 "$tmp/$name/completions/_nls" "$PREFIX/share/zsh/site-functions/_$BIN_NAME"
    echo "installed the man page and bash/zsh completions under $PREFIX/share"
fi
echo ""
echo "shell integration (Ctrl+G turns the typed line into a command), in ~/.zshrc or ~/.bashrc:"
echo "  eval \"\$($BIN_NAME --shell zsh)\"    # or: --shell bash"

case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo ""
       echo "note: $BINDIR is not on your PATH. Add it to your shell profile:"
       echo "  export PATH=\"$BINDIR:\$PATH\"" ;;
esac
