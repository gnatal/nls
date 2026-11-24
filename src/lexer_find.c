/*
 * lexer_find.c — find: files by name, type, size, age, depth and line count.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool exclude;      /* inside "ignoring X, Y and Z"                         */
    bool size_range;   /* after "between 10MB": the next size is the upper bound */
    size_t floor;      /* direction lookback never crosses a token that made a slot */
    size_t root_end;   /* token index just past the last search root ("in src and tests") */
    char time_prefix;  /* 'm' (-mtime/-mmin) or 'a' (-atime/-amin)             */
} FindState;

/* "in X", "under X": what may precede a search root */
static const char *const FIND_IN_WORDS[] = {
    "in", "inside", "under", "within", "through", "from", NULL
};

static const char *const FIND_LINK_NOUNS[] = { "symlinks", "symlink", "links", "link", NULL };

static const char *const FIND_ANY_NOUNS[]  = { "everything", "anything", "entries", "items", NULL };

static const char *const FIND_ACCESS_WORDS[] = { "accessed", "access", "opened", NULL };

/* Which side of a size or age the user means. The nearest one before the
 * value wins; a "not" anywhere in the window flips it. */
static const char *const FIND_PLUS[] = {
    "larger", "bigger", "greater", "more", "over", "above", "exceeding", "exceeds",
    "exceed", "least", "older", "before", "earlier", "longer", "heavier", ">", ">=", NULL
};

static const char *const FIND_MINUS[] = {
    "smaller", "less", "fewer", "under", "below", "most", "up", "within", "last",
    "past", "newer", "younger", "recent", "recently", "since", "after", "lighter",
    "tinier", "<", "<=", NULL
};

static const char *const FIND_NEGATIONS[] = {
    "not", "never", "haven't", "havent", "hasn't", "hasnt", "isn't", "isnt",
    "aren't", "arent", "wasn't", "wasnt", "weren't", "werent", NULL
};

/* After one of these a bare "m" is minutes, not megabytes */
static const char *const FIND_TIME_CONTEXT[] = {
    "last", "past", "within", "older", "newer", "modified", "changed", "edited",
    "updated", "touched", "accessed", "recent", "recently", "since", "younger", NULL
};

/* Words that only glue the request together in find mode */
static const char *const FIND_FILLER[] = {
    "recursively", "recursive", "all", "every", "any", "that", "which", "are", "is",
    "were", "was", "have", "has", "been", "modified", "changed", "edited", "updated",
    "touched", "created", "written", "accessed", "opened", "than", "ago", "old",
    "here", "current", "there", "ones", "size", "sized", "deep", "today", "symbolic",
    "type", "named", "called", "matching", "whose", "name", "or", "but", "max",
    "maximum", "levels", "level", "only", "just", "where", "located", "nested",
    "anywhere", "everywhere", "whole", "tree", "subtree", "big", "large", "small",
    "between", "extension", "extensions", "ending", "ends", "end", "not", NULL
};

/* Power of 1024 for a size unit (0 = bytes ... 4 = TB), or -1. */
static int find_size_unit(const char *u) {
    static const char *const units[5][7] = {
        { "b", "byte", "bytes", NULL },
        { "k", "kb", "kib", "kilobyte", "kilobytes", NULL },
        { "m", "mb", "mib", "meg", "megs", "megabyte", "megabytes" },
        { "g", "gb", "gib", "gig", "gigs", "gigabyte", "gigabytes" },
        { "t", "tb", "tib", "terabyte", "terabytes", NULL },
    };
    for (int p = 0; p < 5; p++)
        for (int k = 0; k < 7 && units[p][k]; k++)
            if (strcmp(u, units[p][k]) == 0) return p;
    return -1;
}

static bool is_whole(double v) { return v == (double)(long long)v; }

/* "+100M", "-102400k": find rounds sizes *up* to whole units, so "-size -1M"
 * only matches empty files. For an upper bound, count in the next smaller
 * unit; fractions are converted down until they are whole. */
static void find_add_size(ParseContext *ctx, double num, int power, char sign) {
    static const char units[] = "ckMG";
    if (num < 0 || num > 1e12) return;
    if (power == 4) { num *= 1024; power = 3; }
    if (sign == '-' && power > 0) { num *= 1024; power--; }
    while (power > 0 && !is_whole(num)) { num *= 1024; power--; }
    long long v = (long long)num;
    if ((double)v < num) v++;
    char buf[32];
    snprintf(buf, sizeof buf, "%c%lld%c", sign, v, units[power]);
    add_slot(ctx, SLOT_SIZE, buf);
}

/* "-mtime -7" for whole days, "-mmin -120" otherwise. */
static void find_add_time(ParseContext *ctx, double num, double unit_min, char sign, char prefix) {
    double total = num * unit_min;
    if (total < 0 || total > 1e9) return;
    char buf[48];
    if (unit_min >= 1440 && is_whole(total / 1440))
        snprintf(buf, sizeof buf, "-%ctime %c%lld", prefix, sign, (long long)(total / 1440));
    else
        snprintf(buf, sizeof buf, "-%cmin %c%lld", prefix, sign, (long long)(total + 0.5));
    add_slot(ctx, SLOT_TIME, buf);
}

static char find_direction(const RawTok *toks, size_t i, const FindState *st, char def) {
    char sign = 0;
    bool neg = false;
    size_t lo = i > st->floor + 5 ? i - 5 : st->floor;
    for (size_t k = i; k > lo; k--) {
        const RawTok *w = &toks[k - 1];
        if (w->quoted) break;
        if (in_list(w->lower, FIND_NEGATIONS)) neg = true;
        else if (!sign && in_list(w->lower, FIND_PLUS)) sign = '+';
        else if (!sign && in_list(w->lower, FIND_MINUS)) sign = '-';
    }
    if (!sign) sign = def;
    if (neg) sign = sign == '+' ? '-' : sign == '-' ? '+' : '!';
    return sign;
}

/* "less than 100 lines" -> "< 100": an awk comparison on NR. "at least",
 * "at most", "up to", "or more", "or fewer" make it inclusive. */
static void find_add_lines(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                           size_t used, const char *num, const FindState *st) {
    const char *p1 = i > st->floor && !toks[i - 1].quoted ? toks[i - 1].lower : "";
    const char *p2 = i > st->floor + 1 && !toks[i - 2].quoted ? toks[i - 2].lower : "";
    const char *a1 = i + used < count ? toks[i + used].lower : "";
    const char *a2 = i + used + 1 < count ? toks[i + used + 1].lower : "";
    const char *op;
    char sign = find_direction(toks, i, st, '=');
    if (strcmp(p1, "least") == 0 || (strcmp(a1, "or") == 0 &&
        (strcmp(a2, "more") == 0 || strcmp(a2, "greater") == 0 || strcmp(a2, "longer") == 0)))
        op = sign == '!' || sign == '-' ? "<" : ">=";
    else if (strcmp(p1, "most") == 0 || (strcmp(p1, "to") == 0 && strcmp(p2, "up") == 0) ||
             (strcmp(a1, "or") == 0 &&
              (strcmp(a2, "less") == 0 || strcmp(a2, "fewer") == 0 || strcmp(a2, "shorter") == 0)))
        op = sign == '!' || sign == '+' ? ">" : "<=";
    else
        op = sign == '+' ? ">" : sign == '-' ? "<" : sign == '!' ? "!=" : "==";
    char buf[48];
    snprintf(buf, sizeof buf, "%s %s", op, num);
    add_slot(ctx, SLOT_LINES, buf);
}

static bool find_time_context(const RawTok *toks, size_t i, const FindState *st) {
    size_t lo = i > st->floor + 5 ? i - 5 : st->floor;
    for (size_t k = i; k > lo; k--)
        if (!toks[k - 1].quoted && in_list(toks[k - 1].lower, FIND_TIME_CONTEXT)) return true;
    return false;
}

static bool find_word_ok(const char *lw) {
    return lw[0] != '\0' && !is_number(lw, false) &&
           !in_list(lw, STOPWORDS) && !in_list(lw, FIND_FILLER) &&
           !in_list(lw, FIND_PLUS) && !in_list(lw, FIND_MINUS) &&
           !in_list(lw, FIND_NEGATIONS) && !in_list(lw, FIND_IN_WORDS) &&
           !in_list(lw, FIND_EXCLUDE_WORDS) && !in_list(lw, FIND_DIR_NOUNS) &&
           !in_list(lw, FIND_FILE_NOUNS) && !in_list(lw, FIND_LINK_NOUNS) &&
           !in_list(lw, FIND_ANY_NOUNS) && strcmp(lw, "hidden") != 0 &&
           strcmp(lw, "past") != 0 && strcmp(lw, "last") != 0 && strcmp(lw, "this") != 0 &&
           find_time_unit(lw, true) == 0 && !is_keyword(TOOL_FIND, lw);
}

static void find_set_kind(ParseContext *ctx, char kind) {
    if (ctx->find_kind_set && ctx->find_kind != kind) ctx->find_kind = 'a';
    else ctx->find_kind = kind;
    ctx->find_kind_set = true;
}

/* A number, maybe with a unit: size, age, depth or plain count.
 * Returns tokens consumed (0: not handled). */
static size_t find_number(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                          FindState *st) {
    const char *lw = toks[i].lower;
    size_t n = number_prefix(lw, false);
    if (n == 0 || n >= 24) return 0;
    char num[24];
    memcpy(num, lw, n);
    num[n] = '\0';
    double v = atof(num);
    const char *suffix = lw + n;

    const char *unit = suffix;
    size_t used = 1;
    if (*suffix == '\0') {
        if (i + 1 < count && !toks[i + 1].quoted) { unit = toks[i + 1].lower; used = 2; }
        else unit = NULL;
    }

    if (unit) {
        const char *after = i + used < count ? toks[i + used].lower : NULL;
        bool aged = after && (strcmp(after, "ago") == 0 || strcmp(after, "old") == 0);
        double minutes = find_time_unit(unit, aged || find_time_context(toks, i, st));
        if (minutes > 0) {
            char def = after && strcmp(after, "old") == 0 ? '+' : '-';
            find_add_time(ctx, v, minutes, find_direction(toks, i, st, def), st->time_prefix);
            return used;
        }
        int power = find_size_unit(unit);
        if (power >= 0) {
            const char *pw = i > st->floor && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;
            char sign;
            if (pw && (strcmp(pw, "between") == 0 || strcmp(pw, "from") == 0)) {
                sign = '+';
                st->size_range = true;
            } else if (st->size_range && pw &&
                       (strcmp(pw, "and") == 0 || strcmp(pw, "to") == 0 || strcmp(pw, "-") == 0)) {
                sign = '-';
                st->size_range = false;
            } else {
                sign = find_direction(toks, i, st, '+');
            }
            find_add_size(ctx, v, power, sign);
            return used;
        }
        if (used == 2 && all_digits(num, n) && n < sizeof ctx->find_depth &&
            (strcmp(unit, "level") == 0 || strcmp(unit, "levels") == 0)) {
            memcpy(ctx->find_depth, num, n + 1);
            return 2;
        }
        if (all_digits(num, n) &&
            (strcmp(unit, "line") == 0 || strcmp(unit, "lines") == 0 || strcmp(unit, "loc") == 0)) {
            find_add_lines(ctx, toks, count, i, used, num, st);
            return used;
        }
    }
    if (*suffix) return 0;

    /* A plain number is a result count only in "first 5", "top 3",
     * "10 largest". Anything else ("has 100 words") is refused later
     * rather than guessed. */
    static const char *const before[] = {
        "first", "top", "limit", "max", "maximum", "largest", "biggest", "heaviest", NULL
    };
    static const char *const after_w[] = { "largest", "biggest", "heaviest", NULL };
    const char *pw = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;
    const char *nw = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    if (all_digits(num, n) && ((pw && in_list(pw, before)) || (nw && in_list(nw, after_w)))) {
        add_slot(ctx, SLOT_NUM, num);
        return 1;
    }
    if (!ctx->stray) ctx->stray = arena_dup(ctx, toks[i].text);
    return 1;
}

/* Classify toks[i] in find mode. Returns the number of tokens consumed
 * (>= 1), or 0 if the token should fall through to intent handling. */
static size_t classify_find(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                            FindState *st) {
    const char *t = toks[i].text;
    const char *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    const char *prev = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;
    size_t at = 0;
    const char *pw = prev_word(toks, i, &at);
    bool after_in = pw && in_list(pw, FIND_IN_WORDS);

    /* 1. Quoted text: a directory after "in", otherwise a name pattern */
    if (toks[i].quoted) {
        if (st->exclude) { find_add_exclude(ctx, t); return 1; }
        bool path = after_in || (strchr(t, '/') && looks_like_file(t, TOOL_GREP));
        add_slot(ctx, path ? SLOT_FILE : SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 2. "ignoring case" -> -iname (checked before "ignoring X") */
    size_t n = grep_modifier_at(ctx, toks, count, i, "i");
    if (n) return n;

    /* 3. Exclusions: "ignoring build", "not in dist", "excluding .git and node_modules" */
    if (in_list(lw, FIND_EXCLUDE_WORDS)) { st->exclude = true; return 1; }
    if (strcmp(lw, "not") == 0 && next &&
        (strcmp(next, "in") == 0 || strcmp(next, "inside") == 0 || strcmp(next, "under") == 0)) {
        st->exclude = true;
        return 2;
    }
    if (st->exclude) {
        n = exclude_item(ctx, toks, count, i, find_word_ok, ".*");
        if (n) return n;
        st->exclude = false;
    }

    /* 4. Search roots: ".", "..", "~", "home directory" */
    if (strcmp(t, ".") == 0 || strcmp(t, "..") == 0 || strcmp(t, "~") == 0 ||
        strcmp(t, "./") == 0 || strcmp(t, "~/") == 0) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }
    if (strcmp(lw, "home") == 0 && ((next && in_list(next, FIND_DIR_NOUNS)) || after_in)) {
        add_slot(ctx, SLOT_FILE, "~");
        return next && in_list(next, FIND_DIR_NOUNS) ? 2 : 1;
    }

    /* 5. Depth: "depth 2", "top level", "not recursively" */
    if ((strcmp(lw, "depth") == 0 || strcmp(lw, "maxdepth") == 0 || strcmp(lw, "-maxdepth") == 0) &&
        next && all_digits(next, strlen(next)) && strlen(next) < sizeof ctx->find_depth) {
        memcpy(ctx->find_depth, next, strlen(next) + 1);
        return 2;
    }
    if (strcmp(lw, "top-level") == 0 || strcmp(lw, "toplevel") == 0 ||
        strcmp(lw, "non-recursive") == 0 || strcmp(lw, "non-recursively") == 0 ||
        strcmp(lw, "nonrecursive") == 0 || strcmp(lw, "nonrecursively") == 0) {
        memcpy(ctx->find_depth, "1", 2);
        return 1;
    }
    if (next && ((strcmp(lw, "top") == 0 && (strcmp(next, "level") == 0 || strcmp(next, "levels") == 0)) ||
                 ((strcmp(lw, "not") == 0 || strcmp(lw, "non") == 0) &&
                  (strcmp(next, "recursive") == 0 || strcmp(next, "recursively") == 0)))) {
        memcpy(ctx->find_depth, "1", 2);
        return 2;
    }

    /* 6. Globs and file types: "*.c", "src/ *.c" (no space), ".py", "c and h files", "python files" */
    const char *star = strstr(t, "*.");
    if (star && is_alnum_word(star + 2, 8) &&
        (star == t || (star[-1] == '/' && !memchr(t, '*', (size_t)(star - t))))) {
        if (star > t) {
            char dir[NL_MAX_INPUT];
            snprintf(dir, sizeof dir, "%.*s", (int)(star - t), t);
            add_slot(ctx, SLOT_FILE, dir);
        }
        add_slot(ctx, SLOT_EXT, lw + (star - t) + 2);
        return 1;
    }
    if (!strchr(t, '/') && strpbrk(t, "*?[")) { add_slot(ctx, SLOT_QUOTED_STR, t); return 1; }
    if (lw[0] == '.' && is_alnum_word(lw + 1, 8) && in_list(lw + 1, GREP_EXTS)) {
        add_slot(ctx, SLOT_EXT, lw + 1);
        return 1;
    }
    if (lw[0] == '.' && lw[1] && lw[1] != '.' && lw[1] != '/' && !strchr(t, '/')) {
        add_slot(ctx, SLOT_QUOTED_STR, t);   /* a dotfile name: .DS_Store, .env */
        return 1;
    }
    if (next && (strcmp(next, "and") == 0 || strcmp(next, "or") == 0) && i + 3 < count &&
        grep_type_ext(lw) && grep_type_ext(toks[i + 2].lower) &&
        in_list(toks[i + 3].lower, FIND_FILE_NOUNS)) {
        add_slot(ctx, SLOT_EXT, grep_type_ext(lw));
        add_slot(ctx, SLOT_EXT, grep_type_ext(toks[i + 2].lower));
        return 3;
    }
    if (next && in_list(next, FIND_FILE_NOUNS) && grep_type_ext(lw)) {
        add_slot(ctx, SLOT_EXT, grep_type_ext(lw));
        return 1;
    }
    if (pw && (strcmp(pw, "extension") == 0 || strcmp(pw, "extensions") == 0 ||
               strcmp(pw, "ext") == 0)) {
        const char *e = lw[0] == '.' ? lw + 1 : lw;
        if (is_alnum_word(e, 8)) { add_slot(ctx, SLOT_EXT, e); return 1; }
    }

    /* 7. Numbers: sizes, ages, depth levels, counts */
    n = find_number(ctx, toks, count, i, st);
    if (n) return n;

    /* 8. Ages without a number: "last week", "past hour", "today" */
    static const char *const one_unit[] = { "last", "past", "this", "a", "an", "one", NULL };
    double minutes = find_time_unit(lw, true);
    if (minutes > 0 && strcmp(lw, "m") != 0 && prev && in_list(prev, one_unit)) {
        find_add_time(ctx, 1, minutes, find_direction(toks, i, st, '-'), st->time_prefix);
        return 1;
    }
    if (strcmp(lw, "today") == 0) {
        find_add_time(ctx, 1, 1440, find_direction(toks, i, st, '-'), st->time_prefix);
        return 1;
    }

    /* 9. Name patterns: "named Makefile", "called notes.txt" */
    if (pw && (strcmp(pw, "named") == 0 || strcmp(pw, "called") == 0 ||
               strcmp(pw, "matching") == 0 ||
               (strcmp(pw, "is") == 0 && at > 0 && strcmp(toks[at - 1].lower, "name") == 0))) {
        add_slot(ctx, SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 10. "in the src folder", "in directory logs", "in the current directory" */
    if (next && in_list(next, FIND_DIR_NOUNS)) {
        if (strcmp(lw, "current") == 0 || strcmp(lw, "this") == 0 || strcmp(lw, "working") == 0)
            return 2;
        if (after_in && find_word_ok(lw)) { add_slot(ctx, SLOT_FILE, t); return 2; }
    }
    if (in_list(lw, FIND_DIR_NOUNS) && after_in && next && find_word_ok(next)) {
        add_slot(ctx, SLOT_FILE, toks[i + 1].text);
        return 2;
    }
    /* "node_modules directories", "build folders": directories with that name */
    if (next && in_list(next, FIND_DIR_NOUNS) && find_word_ok(lw)) {
        add_slot(ctx, SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 11. Paths; a bare file name is a name pattern unless it follows "in"
     * (or "and" right after another root: "in src and tests") */
    const char *ext = extension_of(t);
    bool another_root = st->root_end && (st->root_end == i ||
                        (prev && st->root_end == i - 1 &&
                         (strcmp(prev, "and") == 0 || strcmp(prev, "or") == 0)));
    if (strchr(t, '/') || ((after_in || another_root) && find_word_ok(lw))) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }
    if (looks_like_file(t, TOOL_GREP) || (ext && in_list(ext, GREP_EXTS))) {
        add_slot(ctx, SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 12. "hidden files" -> -name '.*' */
    if (strcmp(lw, "hidden") == 0) { add_slot(ctx, SLOT_QUOTED_STR, ".*"); return 1; }

    /* 13. What to look for: files, directories, links (kept as intent) */
    if (in_list(lw, FIND_FILE_NOUNS)) { find_set_kind(ctx, 'f'); add_intent(ctx, "files"); return 1; }
    if (in_list(lw, FIND_DIR_NOUNS))  { find_set_kind(ctx, 'd'); add_intent(ctx, "directories"); return 1; }
    if (in_list(lw, FIND_LINK_NOUNS)) { find_set_kind(ctx, 'l'); add_intent(ctx, "links"); return 1; }
    if (in_list(lw, FIND_ANY_NOUNS))  { find_set_kind(ctx, 'a'); return 1; }

    /* 14. Glue words carry no intent */
    if (in_list(lw, FIND_FILLER) || in_list(lw, FIND_PLUS) || in_list(lw, FIND_MINUS) ||
        in_list(lw, FIND_NEGATIONS) || strcmp(lw, "last") == 0 || strcmp(lw, "past") == 0)
        return 1;
    return 0;
}

void nl_lex_find(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    FindState fst = { false, false, 0, 0, 'm' };
    for (size_t i = 0; i < count; i++)
        if (!toks[i].quoted && in_list(toks[i].lower, FIND_ACCESS_WORDS)) fst.time_prefix = 'a';

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        uint8_t before = ctx->slot_count;
        size_t used = classify_find(ctx, toks, count, i, &fst);
        if (ctx->slot_count != before) {
            fst.floor = i + (used ? used : 1);
            if (ctx->slots[ctx->slot_count - 1].type == SLOT_FILE) fst.root_end = fst.floor;
        }
        if (used) { i += used - 1; continue; }
        lex_intent(ctx, toks[i].lower);
    }
}
