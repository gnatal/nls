# High-Impact Feature Roadmap for `nls`

`nls` already has strong engineering fundamentals (~2 ms latency, ~250 KB zero-dependency C11 binary, 909 integration tests, `Ctrl+G` shell integration, and destructive dry-run guards). To capture immediate attention on Hacker News, X/Twitter, and GitHub, the highest-leverage additions focus on **zero-friction visual proof** and **shareable terminal output**.

---

## 1. Live WebAssembly Playground in the Hero Section
**Impact:** Highest landing-page conversion  
**Complexity:** Low–Medium (pure C11 stdlib compiles cleanly to WASM)

Developers often bounce from CLI landing pages when the first step requires `git clone && make`. Because `nls` is pure C11 with zero external dependencies, it can compile via Emscripten into a `<150 KB` WebAssembly module that runs directly in the browser without filesystem stubs.

### Key Elements
* **Interactive Hero Terminal:** Place an input box right beneath *"Plain English in. The exact command out."* with one-click preset pills (e.g., `"undo last 2 commits"`, `"gif from demo.mp4 2s to 5s"`, `"delete .DS_Store"`).
* **Live Latency Flex:** Display a real-time execution badge right next to the output:
  `Compiled in 180 μs · 0 network requests · 0 tokens`
* **Why it works:** Letting visitors type arbitrary English and watch valid POSIX commands snap into place instantaneously proves the "no AI, ~2 ms" claim far better than static benchmark tables.

---

## 2. Interactive `--explain` Flag (The Anti-`explainshell` Flex)
**Impact:** High social shareability (screenshot-worthy terminal output)  
**Complexity:** Low (leverages existing template slots)

While `explainshell.com` translates cryptic flags into English in a browser, `nls --explain` can generate the exact command *and* render an ASCII breakdown directly in the terminal.

### Example Output
```bash
$ nls --explain ffmpeg "make a gif from demo.mp4 between 2 and 5 seconds"

ffmpeg -ss 2 -to 5 -i demo.mp4 -vf 'fps=10,scale=480:-1:flags=lanczos,...' -loop 0 demo.gif
       │     │     │              │                                         └─ loop forever
       │     │     │              └─ 10fps, 480px wide, custom 256-color palette (prevents dithering artifacts)
       │     │     └─ input file
       │     └─ stop at 5s
       └─ fast keyframe seek to 2s
```

### Implementation Notes
* Since the 208 existing templates already know what each slot represents, attaching static annotation strings to template definitions requires minimal extra parsing logic.
* Computing column offsets of rendered tokens allows a clean single-pass right-to-left box-drawing renderer (`│`, `└─`).

---

## 3. Split-Screen "LLM vs. `nls` Live Race" Demo
**Impact:** High viral appeal for landing page / launch post  
**Complexity:** Low (recorded `asciinema` / VHS cast or interactive web simulation)

Turn the static comparison table on the landing page into a side-by-side terminal race:
* **Left Pane (`nls`):** Runs a batch of 10 tricky commands (`git` bisect/reset, `find -prune`, `tar --strip-components`, `curl` with `$TOKEN`), finishes all 10 in **~20 ms total**, catches a destructive operation with a dry-run diff, and passes `shellcheck`.
* **Right Pane (LLM CLI Helper):** Spins on a network loader for ~3.5 seconds on prompt #1, hallucinates an invalid flag on `tar`, or exposes a raw secret in shell history.

---

## 4. "Live Peek" Context-Aware Completions in `Ctrl+G`
**Impact:** "Magic" local UX without cloud latency  
**Complexity:** Medium

Extend the existing local JSON key inspection (`jq`) to peek at local Git refs and common directories during `Ctrl+G` expansion:
* **Git Branches:** Typing `"switch to the login branch"` + `Ctrl+G` inspects `.git/refs/heads/` and fuzzy-matches `feat/user-login-v2`.
* **Local Archives/Media:** Typing `"extract the archive in downloads"` + `Ctrl+G` resolves `~/Downloads/release-1.0.tgz` automatically.

---

## 5. Zero-Friction Distribution (`brew` + Prebuilt Static Binaries)
**Impact:** Removes install drop-off when traffic spikes  
**Complexity:** Low

Add a GitHub Actions release workflow that cross-compiles static binaries for:
* `darwin-arm64` / `darwin-x86_64`
* `linux-x86_64` / `linux-arm64`

Pair it with a Homebrew tap (`brew install gnatal/tap/nls`) and a single-line install command so visitors can test `nls` in under 5 seconds without needing a local C toolchain.