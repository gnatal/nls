# `nls` — Expansion Roadmap & Next-Tier Command Specifications (Part 2)

## 1. Selection Criteria for New Tools

To preserve `nls`'s core strengths—zero runtime dependencies, pure C11 standard library compliance, and sub-millisecond compilation—new tools are prioritized by three criteria:

1. **High Flag-Amnesia Tax:** Syntax that developers routinely look up (`find` expression ordering, `tar` flag order, `git` history surgery, `openssl` subcommands).
2. **High Daily Frequency:** Core terminal workflows across backend engineering, DevOps, and local data wrangling.
3. **High Slot Reuse:** Seamless compatibility with the existing `lexer.c` slot types (`FILE`, `EXT`, `TIME`, `NUM`, `QUOTED_STR`, `RES`, `FACTOR`) with minimal new lexer surface area.

---

## 2. Tier 1: Drop-In Fits for Existing Lexer (Highest ROI)

### 2.1 `find` — File & Directory Discovery

`find` is one of the most frequently searched Unix commands because predicate precedence (`-prune` before `-name`, `+` vs. `-` prefixes for size and mtime) is error-prone.

* **Reused Slots:** `FILE` (search root), `EXT` (`*.c`, `*.log`), `TIME` (`7 days` $\rightarrow$ `-mtime -7`), `NUM` (`100MB` $\rightarrow$ `-size +100M`), `QUOTED_STR` (name or path pattern).
* **Minor Lexer Enhancement:** Recognize byte-size suffixes (`KB`, `MB`, `GB`, `K`, `M`, `G`) into a `SLOT_SIZE` token (normalized to `find`'s `k`, `M`, `G` unit suffixes).

| Natural Language Prompt | Compiled Command |
| :--- | :--- |
| `nls find "files larger than 100MB modified in last 7 days"` | `find . -type f -size +100M -mtime -7` |
| `nls find "c files in src ignoring build"` | `find src -path '*/build' -prune -o -type f -name '*.c' -print` |
| `nls find "empty directories in ."` | `find . -type d -empty` |
| `nls find "delete .DS_Store files recursively"` | `find . -name '.DS_Store' -type f -delete` |

*(Note: Destructive templates like `-delete` automatically showcase `nls`'s `-x` interactive confirmation guardrail.)*

---

### 2.2 `tar` — Archives & Compression

Eliminates flag-order mistakes (`-f` placement, compression flags `-z`/`-J`/`--zstd`, and `--exclude` ordering).

* **Reused Slots:** `FILE` (source directory or target archive), `EXT` (`.tar.gz`, `.tgz`, `.tar.xz`, `.tar.zst`), `QUOTED_STR` (excluded folder or glob patterns).

| Natural Language Prompt | Compiled Command |
| :--- | :--- |
| `nls tar "compress src/ into backup.tar.gz excluding .git and node_modules"` | `tar --exclude='.git' --exclude='node_modules' -czvf backup.tar.gz src/` |
| `nls tar "extract archive.tar.xz into dist/"` | `tar -xJvf archive.tar.xz -C dist/` |
| `nls tar "list contents of backup.tar.gz"` | `tar -ztvf backup.tar.gz` |

---

### 2.3 `git` — History Surgery, Search & Recovery

Targets non-trivial `git` operations where developers hesitate due to repository state risk.

* **Reused Slots:** `NUM` (`last 3 commits` $\rightarrow$ `HEAD~3`), `FILE` (target file path), `QUOTED_STR` (pickaxe search string, commit message, or branch name), `TIME` (`7 days` $\rightarrow$ `--since='7 days ago'`).

| Natural Language Prompt | Compiled Command |
| :--- | :--- |
| `nls git "undo last 2 commits keep changes staged"` | `git reset --soft HEAD~2` |
| `nls git "commits touching src/lexer.c in last 7 days"` | `git log --since='7 days ago' -p -- src/lexer.c` |
| `nls git "find commit that added 'inspect.c'"` | `git log -S 'inspect.c' --oneline` |
| `nls git "squash last 4 commits"` | `git reset --soft HEAD~4 && git commit` |

---

## 3. Tier 2: High-Clout Tools & Minimal Slot Additions

### 3.1 `curl` — HTTP API Testing & Downloads

Removes friction around method flags, JSON headers, payload file references (`@file`), and status-code formatting.

* **New Lexer Slot (`SLOT_URL`):** Recognizes tokens starting with `http://` or `https://` using the safe character allowlist `[A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%-]`, POSIX single-quoted on expansion.

| Natural Language Prompt | Compiled Command |
| :--- | :--- |
| `nls curl "post data.json to https://api.example.com/users"` | `curl -sS -X POST -H 'Content-Type: application/json' -d @data.json 'https://api.example.com/users'` |
| `nls curl "check response status of https://example.com"` | `curl -sSIL -o /dev/null -w '%{http_code}\n' 'https://example.com'` |
| `nls curl "download https://example.com/file.zip as out.zip"` | `curl -fL 'https://example.com/file.zip' -o out.zip` |

---

### 3.2 `magick` (`convert`) — ImageMagick Batch Operations

Mirrors `ffmpeg`'s mental model (`RES`, `FACTOR`, `EXT`, `FILE`) for static images, sprites, and web assets.

* **Reused Slots:** `FILE`, `EXT` (`webp`, `png`, `jpg`, `avif`), `RES` (`800x600`, `width 640`), `NUM` (`quality 85`).

| Natural Language Prompt | Compiled Command |
| :--- | :--- |
| `nls magick "resize hero.png to width 800 convert to webp quality 85"` | `magick hero.png -resize 800x -quality 85 hero.webp` |
| `nls magick "make sprite.png transparent background"` | `magick sprite.png -fuzz 5% -transparent white sprite_transparent.png` |

---

### 3.3 `openssl` — TLS Certificates & Key Generation

Solves `openssl`'s fragmented subcommand syntax (`x509`, `s_client`, `rand`) for backend and infrastructure engineers.

* **New Lexer Slot (`SLOT_HOST`):** Recognizes domain-like tokens (`example.com`, `api.internal.local`) validated against strict DNS hostname characters `[A-Za-z0-9.-]`.

| Natural Language Prompt | Compiled Command |
| :--- | :--- |
| `nls openssl "check cert expiry for example.com"` | `echo \| openssl s_client -servername example.com -connect example.com:443 2>/dev/null \| openssl x509 -noout -dates` |
| `nls openssl "generate 32 byte random hex"` | `openssl rand -hex 32` |
| `nls openssl "inspect cert.pem"` | `openssl x509 -in cert.pem -text -noout` |

---

## 4. Tier 3: Alias Routing & Lightweight Text Processing

To expand utility without bloating the binary:

1. **Tool Aliases in `main.c`:**
   * `rg` / `ripgrep` $\rightarrow$ routes to `grep` templates with modern ripgrep flag emission (`rg -i`, `rg -l`, `rg -t py`).
   * `ffprobe` $\rightarrow$ lightweight media inspection templates (`ffprobe -v error -show_format -show_streams {FILE:0}`).
2. **Column Extraction (`awk`):**
   * Reuses `SLOT_NUM` and `SLOT_FILE` for common log parsing:
     * `nls awk "print column 2 and 4 of access.log"` $\rightarrow$ `awk '{print $2, $4}' access.log`
     * `nls awk "sum column 3 of metrics.txt"` $\rightarrow$ `awk '{sum += $3} END {print sum}' metrics.txt`

---

## 5. Implementation Priority

1. **Phase 1 (`find`, `tar`, `git`):** Zero new external concepts; add `SLOT_SIZE` to `src/lexer.c`, append templates to `src/templates.c`, and add golden + sandboxed execution tests to `tests/`.
2. **Phase 2 (`curl`, `openssl`):** Add strict allowlisted `SLOT_URL` and `SLOT_HOST` recognizers in `src/lexer.c` with matching safety assertions in `src/expander.c`.
3. **Phase 3 (`magick`, `awk`, `rg` alias):** Expand template coverage and expose `nls --list <tool>` filtering per tool category.