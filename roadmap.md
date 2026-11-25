# nls roadmap

Planned improvements, roughly in priority order. The first two sections
address the two ways nls can hurt a user: a confidently wrong command, and a
wrong command that gets run.

## 1. Matching quality

Both passes are done (see "Done"). Open issues go here as they turn up.

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

- **CI on every push and pull request.** The release workflow only tests
  when a tag is pushed. A `ci.yml` that runs `make test` on macOS and Linux
  for every push and pull request would catch regressions earlier.
- **Paraphrase tests.** Keep a file of varied wordings for each template
  that must keep matching it, so a template change can't quietly pull those
  prompts elsewhere (like the GIF tie in section 1).
- **Spot a stale installed binary.** A copy of nls installed with
  `make install` keeps old behaviour after the source changes, which looks
  like a regression (a fixed jq range bug kept showing). `nls --version`
  could print the git commit it was built from, so the two can be compared.
- **Fuzzing.** Run a libFuzzer/AFL harness on `nl_lex` + `nl_expand`. The
  lexer works on fixed-size buffers, and an injection or overflow in the
  quoting is the worst bug this tool could have.
- **Publish the Homebrew tap.** Create the `homebrew-tap` repo from
  `packaging/homebrew/nls.rb` (see `RELEASING.md`).

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

- **Per-tool modules.** `lexer.c` and `expander.c` were split into
  `lexer_<tool>.c` / `expander_<tool>.c` behind small interfaces
  (`lexer_int.h`, `expander_int.h`). Output is unchanged.
- **Releases.** A tag-triggered GitHub workflow builds static Linux
  (x86_64, arm64) and universal macOS binaries, runs the tests and publishes
  them with checksums. Also added: `make dist`, `install.sh` and a Homebrew
  formula.
- **License.** MIT.
