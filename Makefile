CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pedantic -Werror
SRC      := src/main.c src/lexer.c src/matcher.c src/expander.c src/templates.c src/inspect.c

# Per-user install by default: no sudo needed.
#   make install                        -> ~/.local/bin/nls
#   make install PREFIX=/usr/local      -> system-wide (may need sudo)
#   make install BIN_NAME=other         -> install under a different name
PREFIX   ?= $(HOME)/.local
BIN_NAME ?= nls
BINDIR   := $(DESTDIR)$(PREFIX)/bin

nls: $(SRC) src/nl.h
	$(CC) $(CFLAGS) $(SRC) -o $@

test: nls
	sh tests/run_tests.sh ./nls
	sh tests/run_jq_exec.sh ./nls
	sh tests/run_grep_exec.sh ./nls
	sh tests/run_find_exec.sh ./nls
	sh tests/run_ffmpeg_exec.sh ./nls

test-ffmpeg: nls
	sh tests/run_ffmpeg_exec.sh ./nls

install: nls
	install -d "$(BINDIR)"
	install -m 755 nls "$(BINDIR)/$(BIN_NAME)"
	@echo "installed $(BINDIR)/$(BIN_NAME)"
	@case ":$$PATH:" in \
	  *":$(BINDIR):"*) ;; \
	  *) echo ""; \
	     echo "note: $(BINDIR) is not on your PATH. Add it with:"; \
	     echo "  echo 'export PATH=\"$(BINDIR):\$$PATH\"' >> ~/.zshrc && source ~/.zshrc" ;; \
	esac

uninstall:
	rm -f "$(BINDIR)/$(BIN_NAME)"

clean:
	rm -f nls

.PHONY: test test-ffmpeg install uninstall clean
