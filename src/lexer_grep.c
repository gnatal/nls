/*
 * lexer_grep.c — grep: search file contents: patterns, file types, modifiers.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Words that only glue the sentence together in grep mode: -r and -E are
 * always on, so "recursively", "regex", "lines" etc. carry no intent. */
static const char *const GREP_FILLER[] = {
    "lines", "line", "containing", "contain", "contains", "contained", "matching",
    "match", "matches", "mentioning", "mention", "mentions", "that", "where", "have",
    "has", "having", "string", "strings", "text", "pattern", "patterns", "regex",
    "regexp", "expression", "term", "keyword", "word", "words", "all", "every", "any",
    "current", "directory", "directories", "folder", "folders", "dir", "recursively",
    "recursive", "subfolders", "subdirectories", "here", "everywhere", "do", "does",
    "is", "are", "there", "inside", "under", "within", "from", "by", "results",
    "result", "or", "numbers", "number", "extended", NULL
};

/* After one of these, a bare word is the search pattern: "search for TODO",
 * "lines containing error", "the word foo". */
static const char *const GREP_PATTERN_TRIGGERS[] = {
    "for", "find", "grep", "search", "containing", "contain", "contains",
    "matching", "match", "matches", "mentioning", "mention", "mentions", "with",
    "word", "string", "text", "pattern", "term", "keyword", "regex", "expression",
    "without", "lacking", NULL
};

/* "in X", "under X": what may precede a directory name or a quoted path */
static const char *const GREP_IN_WORDS[] = {
    "in", "inside", "under", "within", "through", "from", "the", NULL
};

static const char *const GREP_DIR_NOUNS[] = { "folder", "directory", "dir", NULL };

/* "<type> files": nouns that turn the previous word into an --include filter */
static const char *const GREP_FILE_NOUNS[] = {
    "files", "file", "sources", "source", "code", "scripts", "script", NULL
};

/* Can a bare word be a search pattern (rather than a word of the request)? */
static bool grep_pattern_word_ok(ToolId tool, const char *lw) {
    return !in_list(lw, STOPWORDS) && !in_list(lw, GREP_FILLER) &&
           !in_list(lw, GREP_FILE_NOUNS) && !in_list(lw, GREP_DIR_NOUNS) &&
           !is_keyword(tool, lw);
}

/* Classify toks[i] in grep mode. Returns the number of tokens consumed
 * (>= 1), or 0 if the token should fall through to intent handling. */
static size_t classify_grep(ParseContext *ctx, ToolId tool, const RawTok *toks,
                            size_t count, size_t i) {
    const char *t = toks[i].text;
    const char *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    const char *prev = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;

    /* 1. Modifier phrases: "ignore case", "whole word", "not containing" */
    size_t n = grep_modifier_at(ctx, toks, count, i, NULL);
    if (n) return n;

    /* 2. Current/parent/home directory */
    if (strcmp(t, ".") == 0 || strcmp(t, "..") == 0 || strcmp(t, "~") == 0 || strcmp(t, "./") == 0) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }

    /* 3. Globs: "*.py" -> --include; "src/ *.py" (no space) -> src/ + --include */
    const char *star = strstr(t, "*.");
    if (star && is_alnum_word(star + 2, 8) &&
        (star == t || (star[-1] == '/' && !memchr(t, '*', (size_t)(star - t))))) {
        if (star > t) {
            char dir[NL_MAX_INPUT];
            snprintf(dir, sizeof dir, "%.*s", (int)(star - t), t);
            add_slot(ctx, SLOT_FILE, dir);
        }
        add_slot(ctx, SLOT_EXT, lw + (star - t) + 2);
        return next && in_list(next, GREP_FILE_NOUNS) ? 2 : 1; /* "*.py files" */
    }

    /* 4. ".py" / ".py files": a file type, unless it's a dotfile (.env) */
    if (lw[0] == '.' && is_alnum_word(lw + 1, 8)) {
        bool noun = next && in_list(next, GREP_FILE_NOUNS);
        if (noun || in_list(lw + 1, GREP_EXTS)) {
            add_slot(ctx, SLOT_EXT, lw + 1);
            return noun ? 2 : 1;
        }
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }

    /* 5. "python files", "c code", "log files" */
    if (next && in_list(next, GREP_FILE_NOUNS)) {
        const char *ext = grep_type_ext(lw);
        if (ext) { add_slot(ctx, SLOT_EXT, ext); return 2; }
    }

    /* 6. "in the src folder", "in directory logs" */
    if (next && in_list(next, GREP_DIR_NOUNS) && prev && in_list(prev, GREP_IN_WORDS) &&
        grep_pattern_word_ok(tool, lw)) {
        add_slot(ctx, SLOT_FILE, t);
        return 2;
    }
    if (prev && in_list(prev, GREP_DIR_NOUNS) && i > 1 && in_list(toks[i - 2].lower, GREP_IN_WORDS) &&
        grep_pattern_word_ok(tool, lw)) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }

    /* 7. Paths */
    const char *ext = extension_of(t);
    if (looks_like_file(t, tool) || (ext && in_list(ext, GREP_EXTS))) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }

    /* 8. Bare pattern after a trigger word: "search for TODO", "or FIXME" */
    size_t at = 0;
    const char *pw = prev_word(toks, i, &at);
    bool have_pattern = nl_count_slots(ctx, SLOT_QUOTED_STR) > 0;
    bool trigger = pw && (in_list(pw, GREP_PATTERN_TRIGGERS) ||
                          (strcmp(pw, "or") == 0 && have_pattern) ||
                          /* "how many times does TODO appear" */
                          ((strcmp(pw, "does") == 0 || strcmp(pw, "do") == 0) && next &&
                           (strncmp(next, "appear", 6) == 0 || strncmp(next, "occur", 5) == 0)) ||
                          (strcmp(pw, "of") == 0 && at > 0 &&
                           (strcmp(toks[at - 1].lower, "occurrences") == 0 ||
                            strcmp(toks[at - 1].lower, "instances") == 0 ||
                            strcmp(toks[at - 1].lower, "mentions") == 0)));
    if (trigger && grep_pattern_word_ok(tool, lw)) {
        /* "with 3 lines of context": a number before "lines" is a count */
        bool numeric = is_number(t, false);
        static const char *const counted[] = { "lines", "line", "matches", "match", "results", NULL };
        if (!(numeric && next && in_list(next, counted))) {
            add_slot(ctx, SLOT_QUOTED_STR, t);
            return 1;
        }
    }

    /* 9. Numbers: context lines, max matches */
    if (is_number(t, false)) { add_slot(ctx, SLOT_NUM, t); return 1; }

    /* 10. Glue words carry no intent */
    if (in_list(lw, GREP_FILLER) || in_list(lw, GREP_DIR_NOUNS)) return 1;
    return 0;
}

void nl_lex_grep(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        const char *t = toks[i].text;
        const char *prev = i > 0 ? toks[i - 1].lower : NULL;

        if (toks[i].quoted) {
            /* the pattern, unless it's a path given after "in" or after the
             * pattern: "find 'TODO' in 'my notes/'" */
            bool after_in = prev && !toks[i - 1].quoted && in_list(prev, GREP_IN_WORDS);
            bool file = (after_in || nl_count_slots(ctx, SLOT_QUOTED_STR) > 0) &&
                        looks_like_file(t, TOOL_GREP);
            add_slot(ctx, file ? SLOT_FILE : SLOT_QUOTED_STR, t);
            continue;
        }
        size_t used = classify_grep(ctx, TOOL_GREP, toks, count, i);
        if (used) { i += used - 1; continue; }
        lex_intent(ctx, toks[i].lower);
    }
}
