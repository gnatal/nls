CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pedantic -Werror
SRC      := $(wildcard src/*.c)
HDR      := $(wildcard src/*.h)
VERSION  := $(shell sed -n 's/^\#define NL_VERSION *"\(.*\)"/\1/p' src/nl.h)

# Build stamp: every commit gets its own `nls --version`, e.g. 0.1.0+16.g2827e3e
# (commits since tag v$(VERSION), or in total if it isn't tagged; ".dirty" for
# uncommitted changes). A clean build of the tag itself prints $(VERSION) alone,
# and so does a build without git (a source tarball, Homebrew).
ifneq ($(wildcard .git),)
GIT_SHA   := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_TAG   := $(shell git rev-parse -q --verify 'refs/tags/v$(VERSION)^{commit}' 2>/dev/null)
GIT_HEAD  := $(shell git rev-parse HEAD 2>/dev/null)
GIT_DIRTY := $(shell git diff --quiet HEAD -- 2>/dev/null || echo .dirty)
ifneq ($(GIT_SHA),)
ifeq ($(GIT_TAG)$(GIT_DIRTY),$(GIT_HEAD))
VERSION_SUFFIX :=
else ifneq ($(GIT_TAG),)
VERSION_SUFFIX := +$(shell git rev-list --count v$(VERSION)..HEAD).g$(GIT_SHA)$(GIT_DIRTY)
else
VERSION_SUFFIX := +$(shell git rev-list --count HEAD).g$(GIT_SHA)$(GIT_DIRTY)
endif
endif
endif
BUILD_VERSION := $(VERSION)$(VERSION_SUFFIX)
# Rebuild when the stamp changes, even if no source did (a new commit). The
# stale binary is removed too: make 3.81 (macOS) compares whole-second mtimes,
# so a stamp written in the same second as the last build wouldn't look newer.
$(shell [ "$$(cat .build-version 2>/dev/null)" = '$(BUILD_VERSION)' ] || { echo '$(BUILD_VERSION)' > .build-version; rm -f nls; })

# Release archive: dist/nls-<version>-<target>.tar.gz (+ .sha256).
#   make dist                            -> target from this machine, e.g. macos-arm64
#   make dist TARGET=linux-x86_64        -> name it explicitly (used by CI)
UNAME_S  := $(shell uname -s | tr A-Z a-z)
TARGET   ?= $(subst darwin,macos,$(UNAME_S))-$(shell uname -m)
DIST     := nls-$(VERSION)-$(TARGET)

# Per-user install by default: no sudo needed.
#   make install                        -> ~/.local/bin/nls
#   make install PREFIX=/usr/local      -> system-wide (may need sudo)
#   make install BIN_NAME=other         -> install under a different name
PREFIX   ?= $(HOME)/.local
BIN_NAME ?= nls
BINDIR   := $(DESTDIR)$(PREFIX)/bin
# The man page and the completions go under $(PREFIX)/share. With the
# default prefix bash-completion and man find them on their own; zsh needs
# ~/.local/share/zsh/site-functions on $fpath (install prints how).
MANDIR   := $(DESTDIR)$(PREFIX)/share/man/man1
BASHDIR  := $(DESTDIR)$(PREFIX)/share/bash-completion/completions
ZSHDIR   := $(DESTDIR)$(PREFIX)/share/zsh/site-functions

nls: $(SRC) $(HDR) .build-version
	$(CC) $(CFLAGS) -DNL_VERSION_SUFFIX='"$(VERSION_SUFFIX)"' $(SRC) $(LDFLAGS) -o $@

# Every suite, in order; each takes the binary to test. The execution
# suites skip tools that aren't installed.
TESTS := run_tests run_warn_tests run_x_tests run_infer_tests run_paraphrase_tests \
         run_explain_tests run_shell_tests run_completion_tests \
         run_jq_exec run_grep_exec run_find_exec run_tar_exec run_git_exec \
         run_curl_exec run_magick_exec run_ffmpeg_exec

test: nls
	@for t in $(TESTS); do echo "== $$t"; sh tests/$$t.sh ./nls || exit 1; done

test-paraphrase: nls
	sh tests/run_paraphrase_tests.sh ./nls

# The whole suite against a build with AddressSanitizer and UBSan: an
# out-of-bounds read the golden output doesn't show still fails here.
SAN_FLAGS := -std=c11 -g -O1 -Wall -Wextra -pedantic -Werror \
             -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
build/nls-san: $(SRC) $(HDR) .build-version
	@mkdir -p build
	$(CC) $(SAN_FLAGS) -DNL_VERSION_SUFFIX='"$(VERSION_SUFFIX)"' $(SRC) -o $@

test-sanitize: build/nls-san
	@for t in $(TESTS); do echo "== $$t"; \
	  ASAN_OPTIONS=detect_leaks=0 sh tests/$$t.sh build/nls-san || exit 1; done

# Fuzzing (tests/fuzz/fuzz_nls.c): the lexer, matcher, expanders and
# --explain, with an oracle that fails on an unquoted shell character the
# template didn't put there.
#   make fuzz              libFuzzer, coverage-guided, for FUZZ_TIME seconds
#                          (needs a clang with libFuzzer: Linux, or Homebrew llvm
#                          on macOS: make fuzz FUZZ_CC=$$(brew --prefix llvm)/bin/clang)
#   make fuzz-check        any compiler (Apple clang too): replays the corpus,
#                          then FUZZ_RUNS random mutations, under ASan + UBSan
#   build/fuzz-check crash-input     replays one input
LIB_SRC   := $(filter-out src/main.c,$(SRC))
FUZZ_SRC  := tests/fuzz/fuzz_nls.c
FUZZ_CC   ?= clang
FUZZ_TIME ?= 60
FUZZ_RUNS ?= 200000
FUZZ_SAN  := -g -O1 -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all

build/fuzz-corpus: tests/fuzz/make_corpus.sh $(wildcard tests/*.tsv tests/fuzz/regressions/*)
	sh tests/fuzz/make_corpus.sh $@
	@touch $@

build/fuzz-nls: $(FUZZ_SRC) $(LIB_SRC) $(HDR)
	@mkdir -p build
	$(FUZZ_CC) $(FUZZ_SAN) -fsanitize=fuzzer $(FUZZ_SRC) $(LIB_SRC) -o $@

build/fuzz-check: $(FUZZ_SRC) $(LIB_SRC) $(HDR)
	@mkdir -p build
	$(CC) $(FUZZ_SAN) -DNL_FUZZ_MAIN $(FUZZ_SRC) $(LIB_SRC) -o $@

fuzz: build/fuzz-nls build/fuzz-corpus
	@mkdir -p build/fuzz-found
	ASAN_OPTIONS=detect_leaks=0 build/fuzz-nls -max_total_time=$(FUZZ_TIME) -max_len=512 \
	  -dict=tests/fuzz/nls.dict -artifact_prefix=build/ build/fuzz-found build/fuzz-corpus

fuzz-check: build/fuzz-check build/fuzz-corpus
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=abort_on_error=1 \
	  build/fuzz-check -runs=$(FUZZ_RUNS) build/fuzz-corpus

test-ffmpeg: nls
	sh tests/run_ffmpeg_exec.sh ./nls

test-magick: nls
	sh tests/run_magick_exec.sh ./nls

install: nls
	install -d "$(BINDIR)" "$(MANDIR)" "$(BASHDIR)" "$(ZSHDIR)"
	install -m 755 nls "$(BINDIR)/$(BIN_NAME)"
	install -m 644 man/nls.1 "$(MANDIR)/$(BIN_NAME).1"
	install -m 644 completions/nls.bash "$(BASHDIR)/$(BIN_NAME)"
	install -m 644 completions/_nls "$(ZSHDIR)/_$(BIN_NAME)"
	@echo "installed $(BINDIR)/$(BIN_NAME) ($$("$(BINDIR)/$(BIN_NAME)" --version)), its man page and bash/zsh completions"
	@# Another copy earlier on PATH would keep running the old behaviour.
	@found=$$(command -v $(BIN_NAME) 2>/dev/null); \
	if [ -z "$(DESTDIR)" ] && [ -n "$$found" ] && [ "$$found" != "$(BINDIR)/$(BIN_NAME)" ]; then \
	  echo ""; \
	  echo "warning: '$(BIN_NAME)' runs $$found ($$("$$found" --version 2>/dev/null)),"; \
	  echo "         not the copy just installed. Remove it, or put $(BINDIR) first on PATH."; \
	fi
	@case ":$$PATH:" in \
	  *":$(BINDIR):"*) ;; \
	  *) echo ""; \
	     echo "note: $(BINDIR) is not on your PATH. Add it with:"; \
	     echo "  echo 'export PATH=\"$(BINDIR):\$$PATH\"' >> ~/.zshrc && source ~/.zshrc" ;; \
	esac
	@echo ""
	@echo "zsh completion: if $(ZSHDIR) isn't on your fpath, add before compinit in ~/.zshrc:"
	@echo "  fpath=($(ZSHDIR) \$$fpath)"
	@echo "shell integration (Ctrl+G turns the typed line into a command):"
	@echo "  eval \"\$$($(BIN_NAME) --shell zsh)\"    # or bash, in ~/.zshrc / ~/.bashrc"

uninstall:
	rm -f "$(BINDIR)/$(BIN_NAME)" "$(MANDIR)/$(BIN_NAME).1" "$(BASHDIR)/$(BIN_NAME)" \
	      "$(ZSHDIR)/_$(BIN_NAME)"

version:
	@echo $(VERSION)

# What `nls --version` will print for this checkout.
build-version:
	@echo $(BUILD_VERSION)

# Cut a release: make release V=0.2.0  (see RELEASING.md). Commits and tags
# locally; pushing is up to you.
release:
	@sh scripts/release.sh "$(V)"

dist: nls
	rm -rf dist/$(DIST) dist/$(DIST).tar.gz dist/$(DIST).tar.gz.sha256
	mkdir -p dist/$(DIST)
	cp nls README.md dist/$(DIST)/
	cp -R man completions dist/$(DIST)/
	if [ -f LICENSE ]; then cp LICENSE dist/$(DIST)/; fi
	if [ -f CHANGELOG.md ]; then cp CHANGELOG.md dist/$(DIST)/; fi
	tar -C dist -czf dist/$(DIST).tar.gz $(DIST)
	rm -rf dist/$(DIST)
	cd dist && { sha256sum $(DIST).tar.gz 2>/dev/null || shasum -a 256 $(DIST).tar.gz; } > $(DIST).tar.gz.sha256
	@echo "dist/$(DIST).tar.gz"

clean:
	rm -f nls .build-version
	rm -rf dist build

.PHONY: test test-paraphrase test-sanitize fuzz fuzz-check test-ffmpeg test-magick install uninstall version build-version release dist clean
