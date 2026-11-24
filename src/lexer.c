/*
 * lexer.c — single-pass, context-aware slot extractor.
 *
 * The input prompt is split into raw tokens (whitespace-delimited, with
 * '...' / "..." grouping). Each raw token is then classified into a typed
 * slot (FILE, TIME, RES, ...) or normalized into an intent token used for
 * template scoring. Classification may peek one token ahead (units such as
 * "10 seconds") or look one/two tokens behind ("width 640", "is bob").
 *
 * All extracted strings live in ParseContext.arena; nothing is heap-allocated.
 */
#include "nl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;   /* original text (case preserved), punctuation-trimmed */
    char *lower;  /* lowercased copy */
    bool quoted;
} RawTok;

/* ------------------------------------------------------------------ */
/* Small string helpers                                               */
/* ------------------------------------------------------------------ */

static bool in_list(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(s, list[i]) == 0) return true;
    return false;
}

static const char *const STOPWORDS[] = {
    "to", "the", "a", "an", "and", "with", "in", "of", "for", "at", "on",
    "into", "as", "please", "me", "my", "this", "that", "it", "i", "can",
    "you", "using", "then", "so", "its", "file", NULL
};

static const char *const FFMPEG_EXTS[] = {
    "mp4", "mkv", "mov", "avi", "webm", "flv", "wmv", "m4v", "mpg", "mpeg",
    "ts", "3gp", "mp3", "wav", "aac", "m4a", "flac", "ogg", "opus", "wma",
    "gif", "png", "jpg", "jpeg", "webp", "bmp", NULL
};

static const char *const JQ_EXTS[] = {
    "json", "jsonl", "ndjson", "geojson", "har", NULL
};

static const char *const SEC_UNITS[]  = { "s", "sec", "secs", "second", "seconds", NULL };
static const char *const MIN_UNITS[]  = { "min", "mins", "minute", "minutes", NULL };
static const char *const HOUR_UNITS[] = { "h", "hr", "hrs", "hour", "hours", NULL };

static const char *const STD_HEIGHTS[] = {
    "144", "240", "360", "480", "540", "720", "1080", "1440", "2160", NULL
};

/* jq: words after which a bare word is taken as a string value */
static const char *const JQ_VALUE_TRIGGERS[] = {
    "is", "equals", "==", "=", "!=", "contains", "containing", "contain",
    "includes", "include", "has", "named", "called", "matches", "match",
    "matching", "like", "not", "isnt", "isn't", NULL
};

/* jq: words that are never captured as bare string values */
static const char *const JQ_RESERVED[] = {
    "greater", "less", "more", "fewer", "not", "than", "equal", "equals",
    "to", "at", "above", "below", "over", "under", "least", "most", "bigger",
    "smaller", "larger", "higher", "lower", "older", "younger", "exactly",
    "in", "from", "a", "an", "the", "and", "or", "of", "is", "exceeds",
    "after", "before", "where", "whose", "with", NULL
};

/* jq: a JSON path followed by one of these is a *condition* path */
static const char *const JQ_COND_FOLLOWERS[] = {
    "is", "equals", "equal", "==", "=", "!=", ">", "<", ">=", "<=",
    "contains", "containing", "contain", "includes", "include", "has",
    "matches", "match", "matching", "like", "not", "isnt", "isn't",
    "greater", "less", "more", "fewer", "above", "below", "over", "under",
    "bigger", "smaller", "larger", "higher", "lower", "older", "younger",
    "exceeds", "after", "before", "at", "exists", "exist", "present",
    "defined", NULL
};

/* Length of a leading -?\d+(\.\d+)? match, or 0. */
/* jq: a JSON path preceded by one of these is a *key* path (sort/max key),
 * ordered first just like condition paths */
static const char *const JQ_KEY_LEADERS[] = {
    "by", "highest", "lowest", "largest", "smallest", "biggest", "max", "min",
    "maximum", "minimum", "oldest", "youngest", "cheapest", "most", "least", NULL
};

/* jq: a field path preceded by one of these is a condition ("users with emails") */
static const char *const JQ_COND_LEADERS[] = {
    "have", "has", "having", "with", "without", "missing", "lacking", "no", NULL
};

static size_t number_prefix(const char *s, bool allow_sign) {
    size_t i = 0;
    if (allow_sign && s[i] == '-') i++;
    size_t digits_start = i;
    while (isdigit((unsigned char)s[i])) i++;
    if (i == digits_start) return 0;
    if (s[i] == '.' && isdigit((unsigned char)s[i + 1])) {
        i++;
        while (isdigit((unsigned char)s[i])) i++;
    }
    return i;
}

static bool is_number(const char *s, bool allow_sign) {
    size_t n = number_prefix(s, allow_sign);
    return n > 0 && s[n] == '\0';
}

static bool all_digits(const char *s, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* [H]H:MM[:SS][.frac] */
static bool is_timestamp(const char *s) {
    size_t i = 0, groups = 0;
    while (s[i]) {
        size_t start = i;
        while (isdigit((unsigned char)s[i])) i++;
        size_t len = i - start;
        if (len == 0) return false;
        if (groups > 0 && len != 2) return false;
        if (groups == 0 && len > 3) return false;
        groups++;
        if (s[i] == ':') { i++; continue; }
        if (s[i] == '.' && groups >= 2) {
            i++;
            if (!isdigit((unsigned char)s[i])) return false;
            while (isdigit((unsigned char)s[i])) i++;
            return s[i] == '\0' && groups <= 3;
        }
        return s[i] == '\0' && groups >= 2 && groups <= 3;
    }
    return false;
}

static const char *extension_of(const char *s) {
    const char *slash = strrchr(s, '/');
    const char *base = slash ? slash + 1 : s;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return NULL;
    return dot + 1;
}

static bool ext_in(const char *ext, const char *const *list) {
    char low[16];
    size_t n = strlen(ext);
    if (n == 0 || n >= sizeof low) return false;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)ext[i]);
    return in_list(low, list);
}

/* Does `s` look like a filesystem path for the given tool? */
static bool looks_like_file(const char *s, ToolId tool) {
    if (s[0] == '\0') return false;
    const char *ext = extension_of(s);
    if (ext) {
        if (ext_in(ext, tool == TOOL_JQ ? JQ_EXTS : FFMPEG_EXTS)) return true;
        /* generic name.ext: ext is 2-5 alnum chars with a letter; name not numeric */
        size_t elen = strlen(ext);
        bool has_alpha = false, ok = elen >= 2 && elen <= 5;
        for (size_t i = 0; ok && i < elen; i++) {
            if (!isalnum((unsigned char)ext[i])) ok = false;
            if (isalpha((unsigned char)ext[i])) has_alpha = true;
        }
        const char *slash = strrchr(s, '/');
        const char *base = slash ? slash + 1 : s;
        if (ok && has_alpha && !all_digits(base, (size_t)(ext - 1 - base))) return true;
    }
    if (strchr(s, '/')) {
        size_t n = strlen(s);
        if (s[0] == '.' || s[0] == '/' || s[0] == '~' || s[n - 1] == '/') return true;
    }
    return false;
}

/* jq path: starts with '.', then ident/[ ; chars limited so it is safe
 * to splice inside a single-quoted jq program. */
static bool is_json_path(const char *s) {
    if (s[0] != '.') return false;
    if (!(isalpha((unsigned char)s[1]) || s[1] == '_' || s[1] == '[')) return false;
    for (size_t i = 1; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '[' || c == ']' || c == '?'))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Arena / context helpers                                            */
/* ------------------------------------------------------------------ */

static const char *arena_dup(ParseContext *ctx, const char *s) {
    size_t n = strlen(s) + 1;
    if (ctx->arena_used + n > sizeof ctx->arena) { ctx->overflow = true; return NULL; }
    char *dst = ctx->arena + ctx->arena_used;
    memcpy(dst, s, n);
    ctx->arena_used += n;
    return dst;
}

static void add_slot(ParseContext *ctx, SlotType type, const char *value) {
    if (ctx->slot_count >= NL_MAX_SLOTS) { ctx->overflow = true; return; }
    const char *v = arena_dup(ctx, value);
    if (!v) return;
    ctx->slots[ctx->slot_count].type = type;
    ctx->slots[ctx->slot_count].value = v;
    ctx->slot_count++;
}

static void add_intent(ParseContext *ctx, const char *word) {
    if (ctx->token_count >= NL_MAX_TOKENS) { ctx->overflow = true; return; }
    const char *v = arena_dup(ctx, word);
    if (!v) return;
    ctx->intent_tokens[ctx->token_count++] = v;
}

static void format_seconds(char *buf, size_t size, double secs) {
    snprintf(buf, size, "%.10g", secs);
}

/* ------------------------------------------------------------------ */
/* Raw tokenizer                                                      */
/* ------------------------------------------------------------------ */

static void trim_punct(char *s, bool keep_dots) {
    size_t n = strlen(s);
    while (n > 0 && strchr(",;!?)", s[n - 1])) s[--n] = '\0';
    while (!keep_dots && n > 1 && s[n - 1] == '.' && s[n - 2] != '.') s[--n] = '\0';
    if (s[0] == '(') memmove(s, s + 1, n);
}

static size_t tokenize(char *buf, RawTok *toks, size_t max, char *lowbuf, bool *overflow) {
    size_t count = 0;
    char *p = buf;
    char *lw = lowbuf;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count >= max) { *overflow = true; break; }
        RawTok *t = &toks[count];
        t->quoted = false;
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            t->text = p;
            t->quoted = true;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0';
        } else {
            t->text = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
            trim_punct(t->text, false);
        }
        size_t n = strlen(t->text);
        for (size_t i = 0; i <= n; i++) lw[i] = (char)tolower((unsigned char)t->text[i]);
        t->lower = lw;
        lw += n + 1;
        if (t->quoted || t->text[0] != '\0') count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Classification                                                     */
/* ------------------------------------------------------------------ */

/* ffmpeg-only literal forms with fused units. Returns true if handled. */
static bool classify_ffmpeg_literal(ParseContext *ctx, const char *t, const char *lw) {
    char buf[64];

    if (is_timestamp(t)) { add_slot(ctx, SLOT_TIME, t); return true; }

    if (strcmp(lw, "4k") == 0) { add_slot(ctx, SLOT_RES, "-2:2160"); return true; }
    if (strcmp(lw, "2k") == 0) { add_slot(ctx, SLOT_RES, "-2:1440"); return true; }
    if (strcmp(lw, "8k") == 0) { add_slot(ctx, SLOT_RES, "-2:4320"); return true; }

    size_t n = number_prefix(lw, false);
    if (n == 0 || n >= 32) return false;
    char num[32];
    memcpy(num, lw, n);
    num[n] = '\0';
    const char *suffix = lw + n;

    if (*suffix == '\0') return false; /* plain number: handled by caller */

    if (in_list(suffix, SEC_UNITS)) { add_slot(ctx, SLOT_TIME, num); return true; }
    if (in_list(suffix, MIN_UNITS)) {
        format_seconds(buf, sizeof buf, atof(num) * 60.0);
        add_slot(ctx, SLOT_TIME, buf);
        return true;
    }
    if (in_list(suffix, HOUR_UNITS)) {
        format_seconds(buf, sizeof buf, atof(num) * 3600.0);
        add_slot(ctx, SLOT_TIME, buf);
        return true;
    }
    if (strcmp(suffix, "p") == 0 && all_digits(num, n)) {
        snprintf(buf, sizeof buf, "-2:%s", num);
        add_slot(ctx, SLOT_RES, buf);
        return true;
    }
    if (suffix[0] == 'x' && all_digits(num, n) && all_digits(suffix + 1, strlen(suffix + 1))
        && strlen(suffix + 1) < 8) {
        snprintf(buf, sizeof buf, "%s:%s", num, suffix + 1);
        add_slot(ctx, SLOT_RES, buf);
        return true;
    }
    if (strcmp(suffix, "x") == 0) { add_slot(ctx, SLOT_FACTOR, num); return true; }
    if (strcmp(suffix, "fps") == 0) {
        add_slot(ctx, SLOT_FPS, num);
        add_intent(ctx, "fps");
        return true;
    }
    if ((suffix[0] == 'k' || suffix[0] == 'm') &&
        (suffix[1] == '\0' || strcmp(suffix + 1, "bps") == 0 || strcmp(suffix + 1, "b") == 0)) {
        snprintf(buf, sizeof buf, "%s%c", num, suffix[0] == 'k' ? 'k' : 'M');
        add_slot(ctx, SLOT_BITRATE, buf);
        return true;
    }
    return false;
}

/* ffmpeg plain number: uses one token of lookahead / two of lookbehind.
 * Returns number of extra tokens consumed (0 or 1). */
static int classify_ffmpeg_number(ParseContext *ctx, const char *num,
                                  const char *prev, const char *prev2, const char *next) {
    char buf[64];
    if (next) {
        if (in_list(next, SEC_UNITS)) { add_slot(ctx, SLOT_TIME, num); return 1; }
        if (in_list(next, MIN_UNITS)) {
            format_seconds(buf, sizeof buf, atof(num) * 60.0);
            add_slot(ctx, SLOT_TIME, buf);
            return 1;
        }
        if (in_list(next, HOUR_UNITS)) {
            format_seconds(buf, sizeof buf, atof(num) * 3600.0);
            add_slot(ctx, SLOT_TIME, buf);
            return 1;
        }
        if (strcmp(next, "fps") == 0) {
            add_slot(ctx, SLOT_FPS, num);
            add_intent(ctx, "fps");
            return 1;
        }
        if (strcmp(next, "kbps") == 0) {
            snprintf(buf, sizeof buf, "%sk", num);
            add_slot(ctx, SLOT_BITRATE, buf);
            return 1;
        }
    }
    bool integer = all_digits(num, strlen(num));
    if (prev && integer) {
        if (strcmp(prev, "width") == 0 || strcmp(prev, "wide") == 0) {
            snprintf(buf, sizeof buf, "%s:-2", num);
            add_slot(ctx, SLOT_RES, buf);
            return 0;
        }
        if (strcmp(prev, "height") == 0 || strcmp(prev, "tall") == 0) {
            snprintf(buf, sizeof buf, "-2:%s", num);
            add_slot(ctx, SLOT_RES, buf);
            return 0;
        }
        static const char *const res_lead[] = {
            "to", "at", "scale", "resize", "scaled", "resized", "downscale", "upscale", NULL
        };
        if (in_list(prev, res_lead) && in_list(num, STD_HEIGHTS)) {
            snprintf(buf, sizeof buf, "-2:%s", num);
            add_slot(ctx, SLOT_RES, buf);
            return 0;
        }
    }
    if (prev && (strcmp(prev, "fps") == 0 || strcmp(prev, "framerate") == 0 ||
                 (strcmp(prev, "rate") == 0 && prev2 && strcmp(prev2, "frame") == 0))) {
        add_slot(ctx, SLOT_FPS, num);
        return 0;
    }
    add_slot(ctx, SLOT_NUM, num);
    return 0;
}

/* Seconds multiplier if `sep second [unit]` closes a time range whose unit
 * should also apply to the number before `sep` ("1 to 3 seconds",
 * "2 and 5s", "1-2 minutes"); 0 otherwise. */
static double range_unit(const char *sep, const char *second, const char *after) {
    static const char *const seps[] = { "to", "and", "until", "through", "-", NULL };
    if (!in_list(sep, seps)) return 0;
    size_t n = number_prefix(second, false);
    if (n == 0) return 0;
    const char *unit = second[n] ? second + n : after;
    if (!unit) return 0;
    if (in_list(unit, SEC_UNITS)) return 1;
    if (in_list(unit, MIN_UNITS)) return 60;
    if (in_list(unit, HOUR_UNITS)) return 3600;
    return 0;
}

/* jq: put condition/key paths (followed by a comparator, or preceded by
 * "by") first, so templates can use {JSON_PATH:0} for the condition or
 * sort key and {JSON_PATH:1} for an optional projection. Stable within
 * each group. */
static void reorder_jq_paths(ParseContext *ctx, const bool *is_cond) {
    const char *conds[NL_MAX_SLOTS], *others[NL_MAX_SLOTS];
    size_t nc = 0, no = 0, k = 0;
    for (size_t i = 0; i < ctx->slot_count; i++) {
        if (ctx->slots[i].type != SLOT_JSON_PATH) continue;
        if (is_cond[k]) conds[nc++] = ctx->slots[i].value;
        else others[no++] = ctx->slots[i].value;
        k++;
    }
    size_t ci = 0, oi = 0;
    for (size_t i = 0; i < ctx->slot_count; i++) {
        if (ctx->slots[i].type != SLOT_JSON_PATH) continue;
        ctx->slots[i].value = ci < nc ? conds[ci++] : others[oi++];
    }
}

/* Is `w` a keyword of any template for `tool`? Such words keep their
 * meaning as intent even if the JSON happens to have a same-named field. */
static bool is_keyword(ToolId tool, const char *w) {
    for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
        if (NL_TEMPLATES[i].tool != tool) continue;
        for (size_t k = 0; k < NL_MAX_KEYWORDS && NL_TEMPLATES[i].keywords[k]; k++)
            if (strcmp(NL_TEMPLATES[i].keywords[k], w) == 0) return true;
    }
    return false;
}

/* Map a word to a known JSON field: exact, or singular of a plural
 * ("emails" -> email, "statuses" -> status, "countries" -> country). */
static const char *field_for_word(const JsonInfo *json, const char *w) {
    if (!json) return NULL;
    size_t n = strlen(w);
    for (size_t i = 0; i < json->field_count; i++) {
        const char *f = json->fields[i];
        size_t fl = strlen(f);
        char low[32];
        for (size_t k = 0; k <= fl; k++) low[k] = (char)tolower((unsigned char)f[k]);
        if (strcmp(low, w) == 0) return f;
        if (n == fl + 1 && w[n - 1] == 's' && strncmp(low, w, fl) == 0) return f;
        if (n == fl + 2 && strcmp(w + fl, "es") == 0 && strncmp(low, w, fl) == 0) return f;
        if (fl >= 2 && n == fl + 2 && low[fl - 1] == 'y' && strcmp(w + fl - 1, "ies") == 0 &&
            strncmp(low, w, fl - 1) == 0) return f;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* grep                                                               */
/* ------------------------------------------------------------------ */

/* Modifier phrases. Matched on consecutive raw tokens, consumed (they are
 * not intent tokens) and recorded as flag letters in ctx->flags. */
static const struct { char flag; const char *words[4]; } GREP_MODS[] = {
    { 'i', { "ignore", "case" } },       { 'i', { "ignoring", "case" } },
    { 'i', { "ignores", "case" } },      { 'i', { "case", "insensitive" } },
    { 'i', { "case", "insensitively" } },{ 'i', { "case-insensitive" } },
    { 'i', { "insensitive" } },          { 'i', { "insensitively" } },
    { 'i', { "caseless" } },             { 'i', { "any", "case" } },
    { 'i', { "regardless", "of", "case" } },
    { 'w', { "whole", "word" } },        { 'w', { "whole", "words" } },
    { 'w', { "whole-word" } },           { 'w', { "full", "word" } },
    { 'w', { "full", "words" } },        { 'w', { "exact", "word" } },
    { 'w', { "as", "a", "word" } },
    { 'F', { "fixed", "string" } },      { 'F', { "fixed", "strings" } },
    { 'F', { "fixed-string" } },         { 'F', { "literal" } },
    { 'F', { "literally" } },            { 'F', { "no", "regex" } },
    { 'F', { "not", "a", "regex" } },    { 'F', { "not", "regex" } },
    { 'F', { "plain", "text" } },        { 'F', { "exact", "string" } },
    { 'F', { "exact", "text" } },
    { 'v', { "not", "containing" } },    { 'v', { "not", "contain" } },
    { 'v', { "not", "matching" } },      { 'v', { "not", "match" } },
    { 'v', { "don't", "contain" } },     { 'v', { "dont", "contain" } },
    { 'v', { "doesn't", "contain" } },   { 'v', { "doesnt", "contain" } },
    { 'v', { "don't", "match" } },       { 'v', { "dont", "match" } },
    { 'v', { "doesn't", "match" } },     { 'v', { "doesnt", "match" } },
    { 'v', { "without" } },              { 'v', { "lacking" } },
    { 'v', { "invert" } },               { 'v', { "inverted" } },
};

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

/* Extensions that name a file type on their own (".py", "c files"); also
 * the 1-char ones the generic name.ext rule in looks_like_file misses. */
static const char *const GREP_EXTS[] = {
    "c", "h", "m", "r", "s", "cc", "cpp", "cxx", "hpp", "hh", "py", "js", "mjs",
    "cjs", "ts", "jsx", "tsx", "go", "rs", "rb", "java", "kt", "swift", "php",
    "cs", "sh", "bash", "zsh", "pl", "lua", "sql", "md", "txt", "log", "json",
    "yml", "yaml", "toml", "xml", "html", "htm", "css", "scss", "vue", "ini",
    "cfg", "conf", "csv", "tsv", "ex", "exs", "hs", "scala", "dart", NULL
};

static const struct { const char *name; const char *ext; } GREP_LANGS[] = {
    { "python", "py" },     { "javascript", "js" }, { "typescript", "ts" },
    { "rust", "rs" },       { "golang", "go" },     { "ruby", "rb" },
    { "kotlin", "kt" },     { "markdown", "md" },   { "shell", "sh" },
    { "header", "h" },      { "headers", "h" },     { "c++", "cpp" },
    { "csharp", "cs" },     { "c#", "cs" },         { "perl", "pl" },
    { "text", "txt" },      { "elixir", "ex" },     { "haskell", "hs" },
    { "config", "conf" },   { NULL, NULL }
};

static void add_flag(ParseContext *ctx, char f) {
    size_t n = strlen(ctx->flags);
    if (strchr(ctx->flags, f) || n + 1 >= sizeof ctx->flags) return;
    ctx->flags[n] = f;
    ctx->flags[n + 1] = '\0';
}

/* Length of the modifier phrase starting at toks[i], or 0. */
static size_t grep_modifier_at(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                               const char *only) {
    for (size_t m = 0; m < sizeof GREP_MODS / sizeof GREP_MODS[0]; m++) {
        if (only && !strchr(only, GREP_MODS[m].flag)) continue;
        size_t n = 0;
        while (n < 4 && GREP_MODS[m].words[n]) {
            if (i + n >= count || toks[i + n].quoted ||
                strcmp(toks[i + n].lower, GREP_MODS[m].words[n]) != 0) break;
            n++;
        }
        if (n > 0 && (n == 4 || !GREP_MODS[m].words[n])) {
            add_flag(ctx, GREP_MODS[m].flag);
            return n;
        }
    }
    return 0;
}

/* A file-type word: "py", "python", "c++" -> extension, or NULL. */
static const char *grep_type_ext(const char *w) {
    for (size_t i = 0; GREP_LANGS[i].name; i++)
        if (strcmp(w, GREP_LANGS[i].name) == 0) return GREP_LANGS[i].ext;
    return in_list(w, GREP_EXTS) ? w : NULL;
}

static bool is_alnum_word(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n > max) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i])) return false;
    return true;
}

/* The word before toks[i], skipping articles/quantifiers ("for all TODO"). */
static const char *grep_prev_word(const RawTok *toks, size_t i, size_t *at) {
    static const char *const skip[] = { "the", "a", "an", "all", "any", "every", NULL };
    while (i > 0) {
        i--;
        if (toks[i].quoted) return NULL;
        if (!in_list(toks[i].lower, skip)) { if (at) *at = i; return toks[i].lower; }
    }
    return NULL;
}

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
    const char *pw = grep_prev_word(toks, i, &at);
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

/* ------------------------------------------------------------------ */
/* find                                                               */
/* ------------------------------------------------------------------ */

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

/* "ignoring build", "excluding .git and node_modules" */
static const char *const FIND_EXCLUDE_WORDS[] = {
    "ignoring", "ignore", "excluding", "exclude", "except", "skip", "skipping",
    "omit", "omitting", "outside", "without", NULL
};

static const char *const FIND_DIR_NOUNS[] = {
    "directories", "directory", "dirs", "dir", "folders", "folder",
    "subdirectories", "subdirs", "subfolders", NULL
};
static const char *const FIND_FILE_NOUNS[] = { "files", "file", NULL };
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

static double find_time_unit(const char *u, bool m_is_minutes) {
    static const char *const mins[]   = { "min", "mins", "minute", "minutes", NULL };
    static const char *const hours[]  = { "h", "hr", "hrs", "hour", "hours", NULL };
    static const char *const days[]   = { "d", "day", "days", NULL };
    static const char *const weeks[]  = { "w", "wk", "wks", "week", "weeks", NULL };
    static const char *const months[] = { "mo", "month", "months", NULL };
    static const char *const years[]  = { "y", "yr", "yrs", "year", "years", NULL };
    if (in_list(u, mins) || (m_is_minutes && strcmp(u, "m") == 0)) return 1;
    if (in_list(u, hours))  return 60;
    if (in_list(u, days))   return 1440;
    if (in_list(u, weeks))  return 7 * 1440;
    if (in_list(u, months)) return 30 * 1440;
    if (in_list(u, years))  return 365 * 1440;
    return 0;
}

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

static void find_add_exclude(ParseContext *ctx, const char *t) {
    char buf[NL_MAX_INPUT];
    if (t[0] == '.' && t[1] == '/') t += 2;
    snprintf(buf, sizeof buf, "%s", t);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    if (n) add_slot(ctx, SLOT_EXCLUDE, buf);
}

/* One item of "ignoring X, Y and Z". Returns tokens consumed, 0 to leave
 * exclude mode. `word_ok` decides whether a bare word can be a name;
 * "hidden" becomes `hidden_pat`. Shared by find and tar. */
static size_t exclude_item(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                           bool (*word_ok)(const char *), const char *hidden_pat) {
    const char *t = toks[i].text, *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    static const char *const glue[] = { "and", "or", "the", "any", "all", "also", NULL };
    if (toks[i].quoted) { find_add_exclude(ctx, t); return 1; }
    if (in_list(lw, glue) || in_list(lw, FIND_DIR_NOUNS) || in_list(lw, FIND_FILE_NOUNS))
        return 1;
    if (strcmp(lw, "hidden") == 0) { add_slot(ctx, SLOT_EXCLUDE, hidden_pat); return 1; }

    char buf[32];
    const char *ext = NULL;
    if (strncmp(lw, "*.", 2) == 0 && is_alnum_word(lw + 2, 8)) ext = lw + 2;
    else if (lw[0] == '.' && is_alnum_word(lw + 1, 8) && in_list(lw + 1, GREP_EXTS)) ext = lw + 1;
    else if (next && in_list(next, FIND_FILE_NOUNS)) ext = grep_type_ext(lw);
    if (ext) {
        snprintf(buf, sizeof buf, "*.%s", ext);
        add_slot(ctx, SLOT_EXCLUDE, buf);
        return 1;
    }
    if (strchr(t, '/') || t[0] == '.' || strchr(t, '*') || word_ok(lw)) {
        find_add_exclude(ctx, t);
        return 1;
    }
    return 0;
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
    const char *pw = grep_prev_word(toks, i, &at);
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

/* ------------------------------------------------------------------ */
/* tar                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    bool exclude;       /* inside "excluding .git and node_modules"            */
    bool extracting;    /* an extract verb is present: "in dist" is the destination */
    size_t file_end;    /* token index just past the last FILE ("src and docs") */
} TarState;

static const struct { const char *suffix; char comp; } TAR_SUFFIXES[] = {
    { ".tar.gz", 'z' },  { ".tgz", 'z' },  { ".taz", 'z' },
    { ".tar.bz2", 'j' }, { ".tbz2", 'j' }, { ".tbz", 'j' }, { ".tb2", 'j' },
    { ".tar.xz", 'J' },  { ".txz", 'J' },
    { ".tar.zst", 'Z' }, { ".tar.zstd", 'Z' }, { ".tzst", 'Z' },
    { ".tar", '-' },
};

/* Compression named by a tar suffix: 'z' gzip, 'j' bzip2, 'J' xz, 'Z' zstd,
 * '-' none (.tar); 0 if `name` doesn't end in one. With `need_base` the
 * suffix must follow a file name ("backup.tgz", not ".tgz"). */
char nl_tar_compression(const char *name, bool need_base) {
    size_t n = strlen(name);
    for (size_t k = 0; k < sizeof TAR_SUFFIXES / sizeof TAR_SUFFIXES[0]; k++) {
        size_t sl = strlen(TAR_SUFFIXES[k].suffix);
        if (n < sl) continue;
        const char *tail = name + n - sl;
        bool eq = true;
        for (size_t c = 0; c < sl && eq; c++)
            if (tolower((unsigned char)tail[c]) != TAR_SUFFIXES[k].suffix[c]) eq = false;
        if (!eq) continue;
        if (need_base && (tail == name || tail[-1] == '/')) continue;
        return TAR_SUFFIXES[k].comp;
    }
    return 0;
}

/* "gzip", "xz", ".tar.zst", "tgz" -> the archive suffix (no leading dot) */
static const char *tar_format_word(const char *lw) {
    static const struct { const char *word; const char *ext; } words[] = {
        { "gzip", "tar.gz" },  { "gz", "tar.gz" },   { "gzipped", "tar.gz" },
        { "gzip'd", "tar.gz" },{ "bzip2", "tar.bz2" },{ "bz2", "tar.bz2" },
        { "bzip", "tar.bz2" }, { "bzipped", "tar.bz2" },{ "xz", "tar.xz" },
        { "lzma", "tar.xz" },  { "zstd", "tar.zst" }, { "zst", "tar.zst" },
        { "zstandard", "tar.zst" }, { "uncompressed", "tar" }, { NULL, NULL }
    };
    for (size_t k = 0; words[k].word; k++)
        if (strcmp(lw, words[k].word) == 0) return words[k].ext;
    /* "tar" alone is the verb ("tar up src"), ".tar" the format */
    if (strcmp(lw, "tar") == 0 || strlen(lw) >= 15) return NULL;
    char dotted[16];
    const char *w = lw[0] == '.' ? lw : (snprintf(dotted, sizeof dotted, ".%s", lw), dotted);
    for (size_t k = 0; k < sizeof TAR_SUFFIXES / sizeof TAR_SUFFIXES[0]; k++)
        if (strcmp(w, TAR_SUFFIXES[k].suffix) == 0) {
            switch (TAR_SUFFIXES[k].comp) {
            case 'z': return "tar.gz";
            case 'j': return "tar.bz2";
            case 'J': return "tar.xz";
            case 'Z': return "tar.zst";
            default:  return "tar";
            }
        }
    return NULL;
}

static const char *const TAR_EXTRACT_VERBS[] = {
    "extract", "unpack", "untar", "unarchive", "decompress", "unzip", "uncompress",
    "expand", "restore", "unbundle", "inflate", NULL
};

/* After one of these a bare word is a source ("compress src", "backup of logs") */
static const char *const TAR_SOURCE_LEADERS[] = {
    "compress", "archive", "pack", "bundle", "tar", "backup", "back", "zip", "package",
    "save", "from", "of", NULL
};

/* "into X", "as X": the extraction folder, or the archive name when creating */
static const char *const TAR_DEST_LEADERS[] = {
    "into", "to", "as", "named", "called", "onto", NULL
};

/* "in X": a source when creating, the destination when extracting */
static const char *const TAR_IN_WORDS[] = { "in", "inside", "under", "within", NULL };

static const char *const TAR_QUIET[] = {
    "quietly", "quiet", "silently", "silent", "non-verbose", "nonverbose", NULL
};

/* "strip the top folder", "without the leading directory" */
static const char *const TAR_TOP_WORDS[] = {
    "top", "top-level", "toplevel", "leading", "root", "outer", "outermost", "parent",
    "wrapper", "wrapping", "containing", "first", NULL
};
static const char *const TAR_STRIP_UNITS[] = {
    "component", "components", "level", "levels", "folder", "folders", "directory",
    "directories", "dir", "dirs", "path", "paths", NULL
};

static const char *const TAR_FILLER[] = {
    "up", "everything", "all", "every", "files", "whole", "entire", "recursively",
    "recursive", "new", "compressed", "compression", "verbose", "verbosely", "output",
    "here", "current", "working", "contents", "content", "tarball", "tarballs",
    "level", "levels", "them", "please", "out", "is", "are", "what's", "whats", "what", NULL
};

static bool tar_word_ok(const char *lw) {
    return lw[0] != '\0' && !is_number(lw, false) && !in_list(lw, STOPWORDS) &&
           !in_list(lw, TAR_FILLER) && !in_list(lw, FIND_EXCLUDE_WORDS) &&
           !in_list(lw, FIND_DIR_NOUNS) && !in_list(lw, FIND_FILE_NOUNS) &&
           !in_list(lw, TAR_DEST_LEADERS) && !in_list(lw, TAR_IN_WORDS) &&
           !in_list(lw, TAR_QUIET) && !in_list(lw, TAR_EXTRACT_VERBS) &&
           !in_list(lw, TAR_SOURCE_LEADERS) && strcmp(lw, "hidden") != 0 &&
           !tar_format_word(lw) && !is_keyword(TOOL_TAR, lw);
}

/* A path or name: an archive if it has a tar suffix, otherwise a source /
 * member, or the destination after "into" (and after "in" when extracting). */
static void tar_add_path(ParseContext *ctx, const RawTok *toks, size_t i, TarState *st,
                         size_t used) {
    const char *t = toks[i].text;
    if (nl_tar_compression(t, true)) { add_slot(ctx, SLOT_ARCHIVE, t); return; }
    size_t at = 0;
    const char *pw = grep_prev_word(toks, i, &at);
    /* skip a dir noun: "into the folder dist" */
    if (pw && in_list(pw, FIND_DIR_NOUNS)) pw = grep_prev_word(toks, at, &at);
    bool dest = pw && (in_list(pw, TAR_DEST_LEADERS) ||
                       (st->extracting && in_list(pw, TAR_IN_WORDS)));
    if (dest) {
        if (st->extracting && (strcmp(t, ".") == 0 || strcmp(t, "./") == 0)) return; /* -C . */
        add_slot(ctx, SLOT_DEST, t);
        return;
    }
    add_slot(ctx, SLOT_FILE, t);
    st->file_end = i + used;
}

/* Classify toks[i] in tar mode. Returns the number of tokens consumed
 * (>= 1), or 0 if the token should fall through to intent handling. */
static size_t classify_tar(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                           TarState *st) {
    const char *t = toks[i].text;
    const char *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    const char *prev = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;

    /* 1. Quoted text: an exclusion, an archive, or a path */
    if (toks[i].quoted) {
        if (st->exclude) { find_add_exclude(ctx, t); return 1; }
        tar_add_path(ctx, toks, i, st, 1);
        return 1;
    }

    /* 2. Quiet: drop -v */
    if (in_list(lw, TAR_QUIET)) { add_flag(ctx, 'q'); return 1; }
    if (next && strcmp(next, "verbose") == 0 &&
        (strcmp(lw, "no") == 0 || strcmp(lw, "not") == 0 || strcmp(lw, "without") == 0)) {
        add_flag(ctx, 'q');
        return 2;
    }

    /* 3. Strip leading folders: "strip 1", "strip 2 components",
     *    "without the top-level folder" (checked before "without X") */
    if ((strcmp(lw, "strip") == 0 || strcmp(lw, "--strip-components") == 0) && next &&
        all_digits(next, strlen(next)) && strlen(next) < 4) {
        add_slot(ctx, SLOT_NUM, next);
        return i + 2 < count && in_list(toks[i + 2].lower, TAR_STRIP_UNITS) ? 3 : 2;
    }
    static const char *const strip_verbs[] = {
        "strip", "stripping", "without", "drop", "dropping", "remove", "removing", "minus", NULL
    };
    if (in_list(lw, strip_verbs)) {
        size_t k = i + 1;
        if (k < count && (strcmp(toks[k].lower, "the") == 0 || strcmp(toks[k].lower, "a") == 0)) k++;
        if (k < count && !toks[k].quoted && in_list(toks[k].lower, TAR_TOP_WORDS)) {
            k++;
            if (k < count && strcmp(toks[k].lower, "level") == 0) k++;
            if (k < count && in_list(toks[k].lower, TAR_STRIP_UNITS)) {
                add_slot(ctx, SLOT_NUM, "1");
                return k + 1 - i;
            }
        }
    }

    /* 4. Exclusions: "excluding .git and node_modules", "skip *.log" */
    if (in_list(lw, FIND_EXCLUDE_WORDS) || strcmp(lw, "--exclude") == 0) {
        st->exclude = true;
        return 1;
    }
    if (strncmp(lw, "--exclude=", 10) == 0 && t[10]) { find_add_exclude(ctx, t + 10); return 1; }
    if (st->exclude) {
        size_t n = exclude_item(ctx, toks, count, i, tar_word_ok, ".?*");
        if (n) return n;
        st->exclude = false;
    }

    /* 5. Archive names and formats: "backup.tar.gz", "as .tar.xz", "with zstd" */
    if (nl_tar_compression(t, true)) { add_slot(ctx, SLOT_ARCHIVE, t); return 1; }
    const char *fmt = tar_format_word(lw);
    if (fmt) {
        add_slot(ctx, SLOT_EXT, fmt);
        /* "gzip compression", "xz format" */
        static const char *const fmt_nouns[] = { "compression", "compressed", "format", "archive", NULL };
        return next && in_list(next, fmt_nouns) ? 2 : 1;
    }
    if (strcmp(lw, "no") == 0 && next && strcmp(next, "compression") == 0) {
        add_slot(ctx, SLOT_EXT, "tar");
        return 2;
    }

    /* 6. "the archive", "a backup", "a tarball": nouns, not verbs */
    if (prev && (strcmp(lw, "archive") == 0 || strcmp(lw, "backup") == 0) &&
        (strcmp(prev, "the") == 0 || strcmp(prev, "a") == 0 || strcmp(prev, "an") == 0 ||
         strcmp(prev, "this") == 0 || strcmp(prev, "that") == 0 || strcmp(prev, "my") == 0))
        return 1;

    /* 7. The current directory: "here", "this folder", "the current directory" */
    if ((strcmp(lw, "current") == 0 || strcmp(lw, "this") == 0 || strcmp(lw, "working") == 0) &&
        next && in_list(next, FIND_DIR_NOUNS)) {
        if (!st->extracting) { add_slot(ctx, SLOT_FILE, "."); st->file_end = i + 2; }
        return 2;
    }
    if (strcmp(lw, "here") == 0) return 1;

    /* 8. Numbers only mean something in "strip 1" */
    if (is_number(lw, false)) {
        if (!ctx->stray) ctx->stray = arena_dup(ctx, t);
        return 1;
    }

    /* 9. Paths and file names */
    if (strcmp(t, ".") == 0 || strcmp(t, "..") == 0 || strcmp(t, "~") == 0 ||
        strchr(t, '/') || looks_like_file(t, TOOL_GREP)) {
        tar_add_path(ctx, toks, i, st, 1);
        return 1;
    }

    /* 10. "the src folder", "folder dist" */
    if (next && in_list(next, FIND_DIR_NOUNS) && tar_word_ok(lw)) {
        tar_add_path(ctx, toks, i, st, 2);
        return 2;
    }
    if (in_list(lw, FIND_DIR_NOUNS) && next && tar_word_ok(next)) {
        size_t at = 0;
        const char *pw = grep_prev_word(toks, i, &at);
        if (pw && (in_list(pw, TAR_DEST_LEADERS) || in_list(pw, TAR_IN_WORDS) ||
                   in_list(pw, TAR_SOURCE_LEADERS))) {
            tar_add_path(ctx, toks, i + 1, st, 1);
            return 2;
        }
    }

    /* 11. A bare name after a verb or preposition: "compress src into backup",
     *     "extract it to dist", "src and docs" */
    size_t at = 0;
    const char *pw = grep_prev_word(toks, i, &at);
    /* after "into"/"as"/"named" even a verb-like word is a name: "into backup" */
    bool named = pw && in_list(pw, TAR_DEST_LEADERS) && !is_number(lw, false) &&
                 (in_list(lw, TAR_SOURCE_LEADERS) || is_keyword(TOOL_TAR, lw)) &&
                 !in_list(lw, TAR_EXTRACT_VERBS);
    if ((tar_word_ok(lw) || named) && !(next && in_list(next, FIND_FILE_NOUNS))) {
        bool chained = pw && st->file_end && st->file_end == at &&
                       (strcmp(pw, "and") == 0 || strcmp(pw, "or") == 0);
        if (pw && (chained || in_list(pw, TAR_SOURCE_LEADERS) || in_list(pw, TAR_DEST_LEADERS) ||
                   in_list(pw, TAR_IN_WORDS))) {
            tar_add_path(ctx, toks, i, st, 1);
            return 1;
        }
    }

    /* 12. Glue words carry no intent */
    if (in_list(lw, TAR_FILLER) || in_list(lw, FIND_DIR_NOUNS) || strcmp(lw, "hidden") == 0)
        return 1;
    return 0;
}

bool nl_lex(ParseContext *ctx, ToolId tool, const char *input, const JsonInfo *json) {
    memset(ctx, 0, sizeof *ctx);
    ctx->tool = tool;

    size_t in_len = strlen(input);
    if (in_len >= NL_MAX_INPUT) { ctx->overflow = true; return false; }

    char buf[NL_MAX_INPUT];
    char lowbuf[NL_MAX_INPUT + NL_MAX_RAW];
    RawTok toks[NL_MAX_RAW];
    memcpy(buf, input, in_len + 1);

    size_t count = tokenize(buf, toks, NL_MAX_RAW, lowbuf, &ctx->overflow);

    FindState fst = { false, false, 0, 0, 'm' };
    if (tool == TOOL_FIND)
        for (size_t i = 0; i < count; i++)
            if (!toks[i].quoted && in_list(toks[i].lower, FIND_ACCESS_WORDS)) fst.time_prefix = 'a';

    TarState tst = { false, false, 0 };
    if (tool == TOOL_TAR)
        for (size_t i = 0; i < count; i++)
            if (!toks[i].quoted && in_list(toks[i].lower, TAR_EXTRACT_VERBS)) tst.extracting = true;

    bool path_cond[NL_MAX_SLOTS] = { false };
    size_t path_count = 0;
    bool seen_path = false;

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        const char *t = toks[i].text;
        const char *lw = toks[i].lower;
        const char *prev = i > 0 ? toks[i - 1].lower : NULL;
        const char *prev2 = i > 1 ? toks[i - 2].lower : NULL;
        const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;

        bool grep_like = tool == TOOL_GREP;

        if (tool == TOOL_FIND) {
            uint8_t before = ctx->slot_count;
            size_t used = classify_find(ctx, toks, count, i, &fst);
            if (ctx->slot_count != before) {
                fst.floor = i + (used ? used : 1);
                if (ctx->slots[ctx->slot_count - 1].type == SLOT_FILE) fst.root_end = fst.floor;
            }
            if (used) { i += used - 1; continue; }
            if (lw[0] == '\0' || in_list(lw, STOPWORDS)) continue;
            add_intent(ctx, lw);
            continue;
        }

        if (tool == TOOL_TAR) {
            size_t used = classify_tar(ctx, toks, count, i, &tst);
            if (used) { i += used - 1; continue; }
            if (lw[0] == '\0' || in_list(lw, STOPWORDS)) continue;
            add_intent(ctx, lw);
            continue;
        }

        /* 1. Quoted strings */
        if (toks[i].quoted && grep_like) {
            /* the pattern, unless it's a path given after "in" or after the
             * pattern: "find 'TODO' in 'my notes/'" */
            bool after_in = prev && !toks[i - 1].quoted && in_list(prev, GREP_IN_WORDS);
            bool file = (after_in || nl_count_slots(ctx, SLOT_QUOTED_STR) > 0) &&
                        looks_like_file(t, tool);
            add_slot(ctx, file ? SLOT_FILE : SLOT_QUOTED_STR, t);
            continue;
        }
        if (toks[i].quoted) {
            const char *ext = extension_of(t);
            bool file = tool == TOOL_JQ ? (ext && ext_in(ext, JQ_EXTS)) : looks_like_file(t, tool);
            add_slot(ctx, file ? SLOT_FILE : SLOT_QUOTED_STR, t);
            continue;
        }

        if (grep_like) {
            size_t used = classify_grep(ctx, tool, toks, count, i);
            if (used) { i += used - 1; continue; }
        } else if (tool == TOOL_JQ) {
            /* Count intent: "count", "how many/much", "number of" */
            if (strcmp(lw, "count") == 0 ||
                (strcmp(lw, "how") == 0 && next && (strcmp(next, "many") == 0 || strcmp(next, "much") == 0)) ||
                (strcmp(lw, "number") == 0 && next && strcmp(next, "of") == 0))
                ctx->wants_count = true;

            /* 2. JSON paths */
            if (is_json_path(t)) {
                if (path_count < NL_MAX_SLOTS) {
                    bool cond = (next && in_list(next, JQ_COND_FOLLOWERS)) ||
                                (prev && in_list(prev, JQ_KEY_LEADERS));
                    path_cond[path_count++] = cond;
                    if (cond) ctx->cond_paths++;
                }
                add_slot(ctx, SLOT_JSON_PATH, t);
                seen_path = true;
                continue;
            }
            /* 3. Numbers & JSON literals */
            if (is_number(t, true) || strcmp(lw, "true") == 0 ||
                strcmp(lw, "false") == 0 || strcmp(lw, "null") == 0) {
                add_slot(ctx, SLOT_NUM, is_number(t, true) ? t : lw);
                continue;
            }
            /* 4. Files */
            if (looks_like_file(t, tool)) { add_slot(ctx, SLOT_FILE, t); continue; }
            /* 5. Bare value after a comparator: "where .name is bob" */
            if (seen_path && prev && !in_list(lw, JQ_RESERVED) &&
                (in_list(prev, JQ_VALUE_TRIGGERS) ||
                 (strcmp(prev, "to") == 0 && prev2 &&
                  (strcmp(prev2, "equal") == 0 || strcmp(prev2, "equals") == 0)))) {
                add_slot(ctx, SLOT_QUOTED_STR, t);
                continue;
            }
            /* 6. Bare field names from the input file: "emails" -> .email */
            const char *field = field_for_word(json, lw);
            if (field && !in_list(lw, STOPWORDS) && !is_keyword(tool, lw)) {
                char path[40];
                snprintf(path, sizeof path, ".%s", field);
                if (path_count < NL_MAX_SLOTS) {
                    bool cond = (next && in_list(next, JQ_COND_FOLLOWERS)) ||
                                (prev && (in_list(prev, JQ_KEY_LEADERS) || in_list(prev, JQ_COND_LEADERS)));
                    path_cond[path_count++] = cond;
                    if (cond) ctx->cond_paths++;
                }
                add_slot(ctx, SLOT_JSON_PATH, path);
                seen_path = true;
                continue;
            }
        } else { /* TOOL_FFMPEG */
            /* 2. Dotted extension: ".mp4" */
            if (lw[0] == '.' && ext_in(lw + 1, FFMPEG_EXTS)) {
                add_slot(ctx, SLOT_EXT, lw + 1);
                add_intent(ctx, lw + 1);
                continue;
            }
            /* 3. Timestamps, resolutions, bitrates, fps, factors */
            if (classify_ffmpeg_literal(ctx, t, lw)) continue;
            /* 4. Plain numbers (with unit lookahead) */
            if (is_number(t, false)) {
                /* "from 1 to 3 seconds": the range's trailing unit also applies
                 * to its first number */
                double mult = next && i + 2 < count
                    ? range_unit(next, toks[i + 2].lower, i + 3 < count ? toks[i + 3].lower : NULL)
                    : 0;
                if (mult > 0) {
                    char buf[64];
                    format_seconds(buf, sizeof buf, atof(t) * mult);
                    add_slot(ctx, SLOT_TIME, buf);
                    continue;
                }
                i += (size_t)classify_ffmpeg_number(ctx, t, prev, prev2, next);
                continue;
            }
            /* 5. Files */
            if (looks_like_file(t, tool)) { add_slot(ctx, SLOT_FILE, t); continue; }
            /* 6. Standalone format words: also count as intent */
            if (ext_in(lw, FFMPEG_EXTS)) {
                add_slot(ctx, SLOT_EXT, strcmp(lw, "jpeg") == 0 ? "jpg" : lw);
                add_intent(ctx, lw);
                continue;
            }
        }

        /* Intent tokens */
        if (lw[0] == '\0' || in_list(lw, STOPWORDS)) continue;
        add_intent(ctx, lw);
    }

    if (tool == TOOL_JQ && path_count > 1) reorder_jq_paths(ctx, path_cond);
    return !ctx->overflow;
}

/* ------------------------------------------------------------------ */
/* Queries & names                                                    */
/* ------------------------------------------------------------------ */

const char *nl_find_slot(const ParseContext *ctx, SlotType type, int index) {
    int seen = 0;
    for (size_t i = 0; i < ctx->slot_count; i++) {
        if (ctx->slots[i].type != type) continue;
        if (seen++ == index) return ctx->slots[i].value;
    }
    return NULL;
}

int nl_count_slots(const ParseContext *ctx, SlotType type) {
    int n = 0;
    for (size_t i = 0; i < ctx->slot_count; i++)
        if (ctx->slots[i].type == type) n++;
    return n;
}

ToolId nl_tool_from_name(const char *name) {
    if (strcmp(name, "ffmpeg") == 0 || strcmp(name, "ffprobe") == 0) return TOOL_FFMPEG;
    if (strcmp(name, "jq") == 0) return TOOL_JQ;
    if (strcmp(name, "grep") == 0) return TOOL_GREP;
    if (strcmp(name, "find") == 0) return TOOL_FIND;
    if (strcmp(name, "tar") == 0 || strcmp(name, "bsdtar") == 0 || strcmp(name, "gtar") == 0)
        return TOOL_TAR;
    return TOOL_UNKNOWN;
}

const char *nl_tool_name(ToolId tool) {
    switch (tool) {
    case TOOL_FFMPEG: return "ffmpeg";
    case TOOL_JQ:     return "jq";
    case TOOL_GREP:   return "grep";
    case TOOL_FIND:   return "find";
    case TOOL_TAR:    return "tar";
    default:          return "unknown";
    }
}

static const struct { const char *name; const char *hint; } SLOT_INFO[SLOT_TYPE_COUNT] = {
    [SLOT_NONE]       = { "NONE",       "" },
    [SLOT_FILE]       = { "FILE",       "a file path (e.g. input.mp4, data.json)" },
    [SLOT_EXT]        = { "EXT",        "a format or file type (e.g. mp4, gif, py)" },
    [SLOT_TIME]       = { "TIME",       "a timestamp (e.g. 00:10, 1:30, 15s)" },
    [SLOT_RES]        = { "RES",        "a resolution (e.g. 720p, 1920x1080)" },
    [SLOT_BITRATE]    = { "BITRATE",    "a bitrate (e.g. 128k, 2M)" },
    [SLOT_FPS]        = { "FPS",        "a frame rate (e.g. 30fps)" },
    [SLOT_FACTOR]     = { "FACTOR",     "a multiplier (e.g. 2x, 1.5x)" },
    [SLOT_NUM]        = { "NUM",        "a number (e.g. 10)" },
    [SLOT_QUOTED_STR] = { "QUOTED_STR", "a quoted string or search pattern (e.g. 'active', 'TODO')" },
    [SLOT_JSON_PATH]  = { "JSON_PATH",  "a JSON path (e.g. .users[].email)" },
    [SLOT_SIZE]       = { "SIZE",       "a size (e.g. larger than 100MB, under 4k)" },
    [SLOT_EXCLUDE]    = { "EXCLUDE",    "a directory to skip (e.g. ignoring node_modules)" },
    [SLOT_LINES]      = { "LINES",      "a line count (e.g. less than 100 lines)" },
    [SLOT_ARCHIVE]    = { "ARCHIVE",    "a tar archive (e.g. backup.tar.gz, logs.tgz, dist.tar.xz)" },
    [SLOT_DEST]       = { "DEST",       "a destination (e.g. into dist/)" },
};

const char *nl_slot_name(SlotType type) {
    return type < SLOT_TYPE_COUNT ? SLOT_INFO[type].name : "?";
}

const char *nl_slot_hint(SlotType type) {
    return type < SLOT_TYPE_COUNT ? SLOT_INFO[type].hint : "";
}
