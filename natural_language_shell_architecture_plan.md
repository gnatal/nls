# Natural Language Shell (`nl`) — Technical Architecture & MVP Plan

## 1. Overview & Design Philosophy

`nl` is a **100% offline, zero-dependency, Ahead-of-Time (AOT) natural language CLI compiler** written strictly in **C11 using only the C standard library** (`<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<stdint.h>`, `<stdbool.h>`, `<time.h>`).

Instead of calling external LLM APIs at runtime—which introduces multi-second latency, API key management, privacy leaks, and execution risk—`nl` compiles a pre-generated knowledge base of parameterized command templates directly into static C arrays inside the binary.

### Core Goals
* **Sub-Millisecond Execution:** Cold start and compilation take `< 0.5ms`, making it fast enough to embed inside tight shell loops.
* **Strict C11 Standard Library Only:** No POSIX `<regex.h>`, no SQLite, no OpenSSL, and no third-party runtime dependencies. Builds cleanly on Linux, macOS, and Windows.
* **Safe by Default:** Prints the compiled command to `stdout` for inspection or command substitution (`$(nl ...)`), requiring explicit opt-in (`-x`) to execute.
* **MVP Target Tools:** `jq`, `grep`, and `ffmpeg`.

---

## 2. System Architecture

```text
User Input: nl ffmpeg "cut input.mp4 from 00:10 to 00:25 and scale to 720p"
     │
     ▼
┌─────────────────────────────────────────┐
│ 1. Lexer & Typed Slot Extractor         │  Extracts literals -> {FILE}, {TIME}, {RES}, {NUM}, {STR}
│    Produces normalized token stream     │  Skeleton: ["cut", "from", "to", "scale", "720p"]
└────────────────────┬────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────┐
│ 2. Compile-Time Static Template Index   │  Scoped by target tool (`ffmpeg`, `jq`, `grep`)
│    Slot Gate + Weighted Token Overlap   │  Finds highest-scoring template + checks required slots
└────────────────────┬────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────┐
│ 3. Safe Argv Assembler & Dry-Run Output │  Substitutes slots into flag template, validates safety
│    Prints exact command (or -x exec)    │  Output: ffmpeg -ss 00:10 -to 00:25 -i input.mp4 ...
└─────────────────────────────────────────┘
```

---

## 3. Core Data Structures (`nl.h`)

To keep memory deterministic and avoid dynamic heap fragmentation, every compilation runs inside fixed stack/arena buffers.

```c
#ifndef NL_H
#define NL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NL_MAX_SLOTS      16
#define NL_MAX_TOKENS     32
#define NL_MAX_CMD_LEN    4096

typedef enum {
    TOOL_UNKNOWN = 0,
    TOOL_FFMPEG,
    TOOL_JQ,
    TOOL_GREP
} ToolId;

typedef enum {
    SLOT_NONE = 0,
    SLOT_FILE,       // e.g., input.mp4, users.json, src/
    SLOT_EXT,        // e.g., mp3, mp4, gif, json, c
    SLOT_TIME,       // e.g., 00:10, 01:20:30, 15s
    SLOT_RES,        // e.g., 720p, 1080p, 1920x1080
    SLOT_BITRATE,    // e.g., 128k, 320k, 2M
    SLOT_NUM,        // e.g., 10, 60, 3
    SLOT_QUOTED_STR, // e.g., 'ERROR', "TODO"
    SLOT_JSON_PATH   // e.g., .users[].email, .status
} SlotType;

typedef struct {
    SlotType type;
    const char *value; // Null-terminated slice stored in arena buffer
} ExtractedSlot;

typedef struct {
    ToolId tool;
    const char *id;               // e.g., "ffmpeg_cut_scale"
    const char *keywords[12];     // Normalized intent tokens: {"cut", "trim", "scale", "resize"}
    uint8_t keyword_count;
    SlotType required_slots[6];   // Slots that MUST be present to match
    uint8_t required_count;
    const char *flag_template;    // e.g., "ffmpeg -ss {TIME:0} -to {TIME:1} -i {FILE:0} -vf scale=-2:{RES:0} {OUT_FILE}"
} CommandTemplate;

typedef struct {
    ToolId tool;
    ExtractedSlot slots[NL_MAX_SLOTS];
    uint8_t slot_count;
    const char *intent_tokens[NL_MAX_TOKENS];
    uint8_t token_count;
} ParseContext;

#endif /* NL_H */
```

---

## 4. The 4-Stage Compilation Pipeline

### Stage 1: Deterministic Slot Extractor (State-Machine Lexer)
A single-pass lexer scans whitespace-delimited tokens and classifies literals into typed slots before normalizing remaining words into intent tokens:

1. **Quoted Strings (`'...'` or `"..."`):** Captured first as `SLOT_QUOTED_STR` (critical for `grep` search patterns and `jq` string comparisons).
2. **Timestamps (`HH:MM:SS`, `MM:SS`, or `\d+s`):** Digits separated by `:` or ending in `s` → `SLOT_TIME`.
3. **Resolutions (`\d+p` or `\d+x\d+`):** e.g., `720p`, `1080p`, `1280x720` → `SLOT_RES`.
4. **Bitrates (`\d+[kKmM]`):** e.g., `128k`, `320k`, `5M` → `SLOT_BITRATE`.
5. **JSON Paths (starts with `.` followed by identifier/brackets):** e.g., `.items[].name`, `.status` → `SLOT_JSON_PATH` (evaluated when `tool == TOOL_JQ`).
6. **Files & Extensions:**
   * Tokens containing `.` with known extensions (`mp4`, `mkv`, `mov`, `mp3`, `wav`, `gif`, `json`, `log`, `txt`, `c`, `h`, `py`, `js`, `md`) or trailing `/` → `SLOT_FILE`.
   * Standalone format tokens (`mp3`, `gif`, `mp4`, `webp`, `wav`) → `SLOT_EXT`.
7. **Plain Numbers:** Pure ASCII digits → `SLOT_NUM`.
8. **Intent Tokens:** Remaining words are lowercased, stripped of trailing punctuation, filtered against a compact stopword list (`"to"`, `"from"`, `"the"`, `"a"`, `"an"`, `"and"`, `"with"`, `"in"`, `"of"`, `"for"`), and appended to `intent_tokens[]`.

### Stage 2: Scoring & Template Matcher
Filters the static `TEMPLATES[]` array by `ToolId`, then evaluates candidates:
1. **Hard Slot Gate:** Verifies that all `required_slots` exist in `ParseContext`. If a template matches keywords well but lacks a required slot (e.g., missing `{FILE:0}` or `{QUOTED_STR:0}`), the matcher records the missing slot so `nl` can emit an actionable error message if no valid template wins.
2. **Weighted Token Overlap:** Compares `intent_tokens[]` against `template.keywords[]`:
   * **Exact match:** `+10` points
   * **4-character prefix stem match:** `+6` points (so `"cutting"` matches `"cut"`, `"converting"` matches `"convert"`, `"searching"` matches `"search"`)
3. **Output Filename Synthesis (`{OUT_FILE}`):** When a template references `{OUT_FILE}` and the user did not supply a second filename, `nl` derives the output path automatically from `{FILE:0}` and `{EXT:0}` (e.g., `input.mkv` + `mp4` → `input.mp4`, or `demo.mov` + `gif` → `demo.gif`).

### Stage 3: Safe Template Expander
Walks `flag_template` character-by-character into `char out_cmd[NL_MAX_CMD_LEN]`:
* Resolves indexed slot placeholders (`{FILE:0}`, `{TIME:1}`, `{JSON_PATH:0}`).
* Supports inline default fallbacks using pipe syntax (`{BITRATE:0|192k}`, `{FILE:0|.}`), substituting the fallback literal when the user omits optional parameters.
* Applies safe POSIX-compatible single-quote escaping (`'...'`) around user-supplied file paths and search strings so spaces or shell metacharacters never cause unintended word splitting or injection.

### Stage 4: Execution & CLI UX
Using pure C11 `<stdlib.h>` `system()`:
* **Default Mode (`nl <tool> "<prompt>"`):** Prints the compiled command cleanly to `stdout` and returns exit code `0`.
* **Execute Mode (`nl -x <tool> "<prompt>"`):** Prints `→ <compiled_command>` to `stderr`, waits for confirmation (`[Enter to run / Ctrl+C to abort]`, bypassable with `-y`), and invokes `system(out_cmd)`.

---

## 5. Starter Template Matrix (MVP Baseline)

| Tool | Intent Keywords | Flag Template |
| :--- | :--- | :--- |
| **`ffmpeg`** | `cut`, `trim`, `from`, `to` | `ffmpeg -ss {TIME:0} -to {TIME:1} -i {FILE:0} -c copy {OUT_FILE}` |
| **`ffmpeg`** | `scale`, `resize`, `width`, `resolution` | `ffmpeg -i {FILE:0} -vf "scale=-2:{RES:0}" -c:a copy {OUT_FILE}` |
| **`ffmpeg`** | `extract`, `audio`, `mp3` | `ffmpeg -i {FILE:0} -vn -acodec libmp3lame -b:a {BITRATE:0\|192k} {OUT_FILE}` |
| **`ffmpeg`** | `gif`, `fps`, `animated` | `ffmpeg -i {FILE:0} -vf "fps={NUM:0\|10},scale={NUM:1\|640}:-1:flags=lanczos" {OUT_FILE}` |
| **`ffmpeg`** | `compress`, `shrink`, `smaller` | `ffmpeg -i {FILE:0} -vcodec libx264 -crf 28 -preset fast {OUT_FILE}` |
| **`ffmpeg`** | `convert`, `to` | `ffmpeg -i {FILE:0} {OUT_FILE}` |
| **`jq`** | `keys`, `fields`, `top` | `jq 'keys' {FILE:0}` |
| **`jq`** | `extract`, `get`, `field`, `list` | `jq -r '.[] \| {JSON_PATH:0}' {FILE:0}` |
| **`jq`** | `count`, `length`, `how`, `many` | `jq 'length' {FILE:0}` |
| **`jq`** | `filter`, `where`, `equals`, `match` | `jq '[.[] \| select({JSON_PATH:0} == {QUOTED_STR:0})]' {FILE:0}` |
| **`grep`** | `recursive`, `folder`, `directory`, `find` | `grep -rnE {QUOTED_STR:0} {FILE:0\|.}` |
| **`grep`** | `ignore`, `case`, `insensitive` | `grep -rniE {QUOTED_STR:0} {FILE:0\|.}` |
| **`grep`** | `count`, `matches`, `lines` | `grep -rcE {QUOTED_STR:0} {FILE:0\|.}` |
| **`grep`** | `only`, `filenames`, `files` | `grep -rlE {QUOTED_STR:0} {FILE:0\|.}` |

---

## 6. Repository Layout & Build Pipeline

```text
nl/
├── Makefile              # cc -std=c11 -O2 -Wall -Wextra -Werror src/*.c -o nl
├── README.md             # Usage examples, CLI flags, and architecture notes
├── src/
│   ├── nl.h              # Core enums, arena limits, and struct definitions
│   ├── main.c            # CLI flag parsing (-x, -y, --list, --version) & dispatch
│   ├── lexer.c           # Pure C11 state-machine slot extractor & stemmer
│   ├── matcher.c         # Scoring engine, slot verification, missing-slot diagnostics
│   ├── expander.c        # Template substitution, default fallbacks, shell escaping
│   └── templates.c       # Compiled static array of CommandTemplate entries
└── tools/
    └── gen_templates.py  # Optional offline build script to compile .spell files into templates.c
```

### Build Command
```bash
cc -std=c11 -O2 -Wall -Wextra -pedantic src/main.c src/lexer.c src/matcher.c src/expander.c src/templates.c -o nl
```

---

## 7. Implementation Roadmap

1. **Milestone 1 — Core Lexer & Slot Extractor (`lexer.c`):** Implement fixed-buffer arena allocation, quoted-string parsing, timestamp/resolution/bitrate/JSON-path recognizers, and stopword filtering.
2. **Milestone 2 — Matcher & Expander (`matcher.c`, `expander.c`):** Implement slot-gated keyword scoring, `{SLOT:IDX|DEFAULT}` substitution, `{OUT_FILE}` synthesis, and safe single-quote escaping.
3. **Milestone 3 — Template Pack & CLI (`templates.c`, `main.c`):** Populate the initial 20+ templates across `ffmpeg`, `jq`, and `grep`, add `-x` / `-y` / `--list` flags, and verify zero warnings under `-Wall -Wextra -pedantic`.
4. **Milestone 4 — Offline `.spell` Compiler (`tools/gen_templates.py`):** Build an optional build-time code generator so new rules can be authored in plain-text `.spell` files and compiled into `templates.c` without touching C source code.