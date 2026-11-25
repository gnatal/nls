#!/bin/sh
# Cut a release: bump the version everywhere it's written, turn the
# [Unreleased] changelog entries into the release notes, commit, and tag.
#
#   make release V=0.2.0        (or: sh scripts/release.sh 0.2.0)
#
# Nothing is pushed. The script prints the push command that starts the
# release workflow. See RELEASING.md.
set -eu

die() { echo "release: $*" >&2; exit 1; }

V="${1:-}"
[ -n "$V" ] || die "usage: make release V=X.Y.Z"
echo "$V" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "'$V' is not X.Y.Z"

cd "$(git rev-parse --show-toplevel)"

old="$(sed -n 's/^#define NL_VERSION *"\(.*\)"/\1/p' src/nl.h)"
[ -n "$old" ] || die "NL_VERSION not found in src/nl.h"

git diff --quiet HEAD -- || die "uncommitted changes; commit or stash them first"
git rev-parse -q --verify "refs/tags/v$V" >/dev/null && die "tag v$V already exists"
branch="$(git rev-parse --abbrev-ref HEAD)"
[ "$branch" = main ] || echo "release: note: releasing from '$branch', not main" >&2

# The new version must be higher than the current one, unless the current
# one was never tagged (it then gets its tag now, e.g. the first release).
if [ "$V" != "$old" ] || git rev-parse -q --verify "refs/tags/v$old" >/dev/null; then
    highest="$(printf '%s\n%s\n' "$old" "$V" | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1)"
    [ "$highest" = "$V" ] && [ "$V" != "$old" ] || die "$V is not higher than $old"
fi

# CHANGELOG.md: the entries under [Unreleased] become the [V] section and
# [Unreleased] starts empty again. If [V] already exists (a version written
# up by hand), it's left as it is, but [Unreleased] must then be empty.
unreleased="$(awk '/^## /{on = index($0, "[Unreleased]") > 0; next} on' CHANGELOG.md | grep -c '[^[:space:]]' || true)"
if grep -q "^## \[$V\]" CHANGELOG.md; then
    [ "$unreleased" -eq 0 ] || die "CHANGELOG.md has both [Unreleased] entries and a [$V] section; merge them"
else
    [ "$unreleased" -gt 0 ] || die "CHANGELOG.md has nothing under [Unreleased] to release"
fi

date="$(date +%Y-%m-%d)"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

# Rewrite $1 with the sed script $2, failing if it changed nothing.
edit() {
    sed "$2" "$1" > "$tmp"
    cmp -s "$1" "$tmp" && die "$1: nothing to update (expected: $2)"
    cat "$tmp" > "$1"
}

esc_old="$(echo "$old" | sed 's/\./\\./g')"

if [ "$V" != "$old" ]; then
    edit src/nl.h "s/^\(#define NL_VERSION *\)\"$esc_old\"/\1\"$V\"/"
    edit man/nls.1 "1s/\"nls $esc_old\"/\"nls $V\"/"
    edit docs/index.html "s/nls<\/code> $esc_old\./nls<\/code> $V./; s/nls $esc_old ·/nls $V ·/"
    edit packaging/homebrew/nls.rb "s|/tags/v$esc_old\.tar\.gz|/tags/v$V.tar.gz|; s/^\(  sha256 \)\".*\"/\1\"REPLACE_WITH_THE_SOURCE_TARBALL_SHA256\"/"
fi
# The man page date is the release date.
sed "1s/^\(\.TH NLS 1 \)[0-9-]*/\1$date/" man/nls.1 > "$tmp" && cat "$tmp" > man/nls.1

if ! grep -q "^## \[$V\]" CHANGELOG.md; then
    awk -v v="$V" -v d="$date" '
        { print }
        !done && /^## \[Unreleased\]/ { print ""; print "## [" v "] - " d; done = 1 }
    ' CHANGELOG.md > "$tmp"
    cat "$tmp" > CHANGELOG.md
fi

git add src/nl.h man/nls.1 docs/index.html packaging/homebrew/nls.rb CHANGELOG.md
git commit -q -m "Release $V"
git tag -a "v$V" -m "nls $V"

echo "Committed and tagged v$V. To publish it:"
echo ""
echo "  git push origin $branch v$V"
echo ""
echo "Then update the Homebrew sha256 (see RELEASING.md)."
