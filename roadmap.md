# nls roadmap

Planned improvements, roughly in priority order. The first two sections
address the two ways nls can hurt a user: a confidently wrong command, and a
wrong command that gets run.

## 1. Matching quality

Both passes are done (see "Done"). Open issues go here as they turn up:

- 26 wordings that pick the wrong template, or none, are listed as known
  misses at the end of `tests/paraphrase_cases.tsv`. None of them gives a
  wrong command silently: 9 are refused and 17 are flagged as
  low-confidence or ambiguous. The paraphrase runner says when one starts
  matching.

## 2. Safety around `-x`

Done (see "Done"). Open issues go here as they turn up.

## 3. User experience

Done (see "Done"). Open issues go here as they turn up:

- A fish version of the shell integration and completions (not written:
  fish wasn't available to test them).
- Inference refuses requests with no clue to the tool ("what changed
  yesterday", "go back to main"). More clue words could pick these up, but
  every word added is a chance to pull another tool's request away.

## 4. Engineering

Mostly done (see "Done"). Open:

- **Publish the Homebrew tap.** Create the `homebrew-tap` repo from
  `packaging/homebrew/nls.rb` (see `RELEASING.md`). This needs a pushed
  `v0.1.0` tag first: the formula's `sha256` is that tag's source tarball.
- The fuzzer's oracle allows a few characters per tool that the builders
  emit themselves (`builder_specials()` in `tests/fuzz/fuzz_nls.c`: find's
  `\(`, magick's `&&`, git's `"$(...)"`, curl's `"$TOKEN"`). It doesn't
  check those for that tool. A stricter oracle would follow each slot value
  through the expander instead.

## 5. More tools

From `part2.md`, ordered by how often people have to look up the syntax:

1. `openssl`
2. `rsync`
3. `sed` / `awk`
4. `docker`
5. `ssh` / `scp`

With the per-tool layout, each tool is a `lexer_<tool>.c`, an optional
`expander_<tool>.c` and its templates (see "Adding a tool" in the README).
Matching quality (section 1) should come first, though: every new template
makes ties more likely.

## Done

- **Matching quality.** Close runners-up are listed on stderr and
  low-confidence matches are flagged; `-y` asks for both. Ties prefer
  templates matched by real words over filler (`make video.mp4 smaller` now
  compresses). A built-in synonym table and light stemming were added. jq
  conditions combine with `and`/`but`/`or` ("and" binds tighter, and the
  grouping is shown on stderr). `between X and Y` / `within X and Y` gives
  an inclusive range, reversed bounds are put in order, and ranges combine
  with other conditions. git falls through to a template
  that can take a date (`what changed yesterday` → `git log --since -p`).
  Second pass: a jq path after "where"/"whose"/"if" is always the
  condition, and an unknown comparator ("around") or an unclear choice of
  condition path is refused rather than guessed. `from X to Y` is a range,
  and `outside X and Y` / `not between` have their own template.
  "either … or" / "both … and" group jq conditions explicitly. grep takes a
  bare existing folder after "in" as the path and refuses a missing one.
  magick falls through like git (`combine … into anim.gif at 10fps`
  animates). Several images into a `.gif` animate even without an fps,
  and grep's "count" comes before a pattern (`count TODO in src/`).

- **Safety around `-x`.** Destructive templates (find deletes,
  `git reset --hard`, `git restore`, `git clean`, `git branch -D`,
  `git push --delete`, force pushes, `curl -X DELETE`) are tagged in
  `DANGERS[]` (`src/templates.c`) and marked in `--list`. `-x` says what
  they destroy and runs a dry run first (`find … -print`, `git clean -n`,
  `git push --dry-run`, `git diff --stat`). A failed dry run stops the
  command, and only a typed `y` runs it. `-y` skips that only with
  `--force`, and never for an uncertain match.

- **User experience.** The tool can be left out: the request is compiled
  for every tool and scored with clues from the request (files, URLs,
  archives, git words), and nls refuses or warns rather than guess
  (`tests/infer_cases.tsv`). `--explain` breaks every command nls can
  generate down part by part (checked by `tests/run_explain_tests.sh`).
  `nls --shell zsh|bash` binds Ctrl+G to turn the typed request into the
  command on the line. bash/zsh completions and a man page are installed by
  `make install`, `install.sh` and the Homebrew formula.

- **Engineering.** `.github/workflows/ci.yml` runs `make test` on Linux
  and macOS for every push and pull request, plus `make test-sanitize`
  (ASan + UBSan) and a two-minute libFuzzer run. `tests/paraphrase_cases.tsv`
  holds about 400 varied wordings, at least one per template, that must keep
  picking their template; known misses are tracked there too. `nls
  --version` prints the commit it was built from, and `make install` warns
  when another `nls` comes first on `PATH`. `tests/fuzz/fuzz_nls.c` fuzzes
  lex → match → expand (every expandable template and its dry run) →
  `--explain`, failing on unbalanced quotes, control characters, or an
  unquoted shell character the template didn't put there (`make fuzz`,
  `make fuzz-check` without libFuzzer). Its first runs found two bugs: an
  escape sequence passing into the printed command, and a read before an
  empty tar `into ''` name. Both are fixed and kept in
  `tests/fuzz/regressions/`.

- **Per-tool modules.** `lexer.c` and `expander.c` were split into
  `lexer_<tool>.c` / `expander_<tool>.c` behind small interfaces
  (`lexer_int.h`, `expander_int.h`). Output is unchanged.
- **Releases.** A tag-triggered GitHub workflow builds static Linux
  (x86_64, arm64) and universal macOS binaries, runs the tests and publishes
  them with checksums. Also added: `make dist`, `install.sh` and a Homebrew
  formula.
- **License.** MIT.
