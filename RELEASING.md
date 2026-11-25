# Releasing nls

Releases are built and published by `.github/workflows/release.yml` when a
`vX.Y.Z` tag is pushed. Each release ships:

| Asset | Built on |
|---|---|
| `nls-X.Y.Z-linux-x86_64.tar.gz` | ubuntu-24.04, statically linked |
| `nls-X.Y.Z-linux-arm64.tar.gz` | ubuntu-24.04-arm, statically linked |
| `nls-X.Y.Z-macos-universal.tar.gz` | macos-15, arm64 + x86_64, macOS 11+ |
| `SHA256SUMS` | checksums of the above (`install.sh` verifies them) |

The full test suite (`make test`) runs on every platform before anything is
published. The execution suites skip tools a runner doesn't have.

## Versions between releases

You don't bump the version on every commit. The build stamps the commit
instead, so each commit's binary reports its own version:

```sh
$ nls --version        # 16 commits after v0.1.0
nls 0.1.0+16.g2827e3e
$ nls --version        # with uncommitted changes
nls 0.1.0+16.g2827e3e.dirty
$ nls --version        # built from the tag v0.1.0
nls 0.1.0
```

`make build-version` prints the stamp for the current checkout. If `v0.1.0`
isn't tagged, the count is all the commits. A build without git (a source
tarball, Homebrew) prints the bare `NL_VERSION`.

## Cutting a release

1. Make sure everything going into the release is listed under `[Unreleased]`
   in `CHANGELOG.md`. That section becomes the release notes.
2. Optionally, do a dry run first: Actions → Release → *Run workflow* builds and
   tests on all platforms without publishing.
3. Run:

   ```sh
   make release V=X.Y.Z
   ```

   From a clean tree, this sets `NL_VERSION` in `src/nl.h` and updates the
   version in the man page, `docs/index.html` and the Homebrew formula. It also
   moves the `[Unreleased]` entries under `## [X.Y.Z] - YYYY-MM-DD`, then commits
   "Release X.Y.Z" and creates the annotated tag `vX.Y.Z`. It refuses a
   version that isn't higher than the current one, and nothing is pushed.
4. Push:

   ```sh
   git push origin main vX.Y.Z
   ```

   The workflow refuses a tag that doesn't match `NL_VERSION`, and a binary
   that doesn't report exactly `nls X.Y.Z`.

To build an archive locally: `make dist` (→ `dist/nls-X.Y.Z-<os>-<arch>.tar.gz`).

## Homebrew

`packaging/homebrew/nls.rb` builds from the source tarball of a tag. To publish
it as a tap (once):

1. Create a public repo named `homebrew-tap` under the same account.
2. Copy the formula to `Formula/nls.rb` in it.

Users then run `brew install gnatal/tap/nls`.

For each release, update `url` to the new tag and set `sha256`:

```sh
curl -fsSL https://github.com/gnatal/nls/archive/refs/tags/vX.Y.Z.tar.gz | shasum -a 256
```

Then check it with `brew install --build-from-source ./Formula/nls.rb && brew test nls`.
