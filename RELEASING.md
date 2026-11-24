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

## Cutting a release

1. Bump `NL_VERSION` in `src/nl.h`.
2. In `CHANGELOG.md`, move the `[Unreleased]` entries under a new
   `## [X.Y.Z] - YYYY-MM-DD` heading. This section becomes the release notes,
   and the workflow fails if it's missing.
3. Optionally, do a dry run first: Actions → Release → *Run workflow* builds and
   tests on all platforms without publishing.
4. Commit, tag and push:

   ```sh
   git commit -am "Release X.Y.Z"
   git tag vX.Y.Z
   git push origin main vX.Y.Z
   ```

   The workflow refuses a tag that doesn't match `NL_VERSION`.

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
