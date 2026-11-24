# Changelog

All notable changes to nls. Versions follow [semantic versioning](https://semver.org).
The release workflow uses the section matching the tag as the release notes.

## [Unreleased]

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
