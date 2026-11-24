# Changelog

All notable changes to nls. Versions follow [semantic versioning](https://semver.org).
The release workflow uses the section matching the tag as the release notes.

## [Unreleased]

### Matching quality

- Ties are no longer settled silently by template order. A close runner-up
  matched by a word the winner ignored is listed on stderr ("this request
  also fits: …") next to the command it would produce, and a match that
  rests on one keyword while another meaningful word went unused is flagged
  as low-confidence. For such matches `-y` no longer skips the confirmation.
- Tie-breaking prefers templates matched by real words over filler:
  `nls ffmpeg "make video.mp4 smaller"` now compresses instead of making a GIF.
- Built-in synonyms ("shrink/reduce/smaller → compress", "erase/remove →
  delete", "rollback → revert", …) and light suffix stripping (-s, -ing,
  -ed, comparative -er), still compiled in and offline.
- jq: conditions can be combined with "and", "but" or "or":
  `where .age > 30 and .status is active`. When both are used, "and" binds
  tighter than "or", and the grouping is printed on stderr.
- jq: ranges. `where .age between 20 and 40` now gives
  `.age >= 20 and .age <= 40` (it used to give `.age == 20`).
- git: `nls git "what changed yesterday"` gives `git log --since='yesterday' -p`
  instead of an error.

## [0.1.0] - 2026-11-24

First release.

- Natural-language requests compiled to exact commands for **ffmpeg**, **jq**,
  **grep**, **find**, **tar**, **git**, **curl** and **ImageMagick** (`magick`,
  also reachable as `nls convert …`).
- Fully offline: no LLM, no network, no runtime dependencies; C11 standard
  library only, every template compiled into the binary.
- `-x` runs the command after confirmation, `-v` explains slots, tokens and
  template scores, `-l` lists templates with example prompts.
- jq peeks at the input file to use its field names and refuse templates that
  don't fit its shape.
- Prebuilt binaries for Linux (x86_64, arm64; static) and macOS (universal).
