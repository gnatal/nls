/*
 * lexer.c — tokenizer, shared helpers and dispatch to the per-tool lexers.
 *
 * The input prompt is split into raw tokens (whitespace-delimited, with
 * '...' / "..." grouping). The tool's lexer (lexer_<tool>.c) then classifies
 * each raw token into a typed slot (FILE, TIME, RES, ...) or normalizes it
 * into an intent token used for template scoring. Classification may peek
 * ahead (units such as "10 seconds") or look behind ("width 640", "is bob").
 *
 * All extracted strings live in ParseContext.arena; nothing is heap-allocated.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small string helpers                                               */
/* ------------------------------------------------------------------ */

bool in_list(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(s, list[i]) == 0) return true;
    return false;
}

const char *const STOPWORDS[] = {
    "to", "the", "a", "an", "and", "with", "in", "of", "for", "at", "on",
    "into", "as", "please", "me", "my", "this", "that", "it", "i", "can",
    "you", "using", "then", "so", "its", "file", NULL
};

const char *const FFMPEG_EXTS[] = {
    "mp4", "mkv", "mov", "avi", "webm", "flv", "wmv", "m4v", "mpg", "mpeg",
    "ts", "3gp", "mp3", "wav", "aac", "m4a", "flac", "ogg", "opus", "wma",
    "gif", "png", "jpg", "jpeg", "webp", "bmp", NULL
};

const char *const JQ_EXTS[] = {
    "json", "jsonl", "ndjson", "geojson", "har", NULL
};

/* Length of a leading -?\d+(\.\d+)? match, or 0. */
size_t number_prefix(const char *s, bool allow_sign) {
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

bool is_number(const char *s, bool allow_sign) {
    size_t n = number_prefix(s, allow_sign);
    return n > 0 && s[n] == '\0';
}

bool all_digits(const char *s, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

const char *extension_of(const char *s) {
    const char *slash = strrchr(s, '/');
    const char *base = slash ? slash + 1 : s;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return NULL;
    return dot + 1;
}

bool ext_in(const char *ext, const char *const *list) {
    char low[16];
    size_t n = strlen(ext);
    if (n == 0 || n >= sizeof low) return false;
    for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)ext[i]);
    return in_list(low, list);
}

/* Does `s` look like a filesystem path for the given tool? */
bool looks_like_file(const char *s, ToolId tool) {
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

/* ------------------------------------------------------------------ */
/* Arena / context helpers                                            */
/* ------------------------------------------------------------------ */

const char *arena_dup(ParseContext *ctx, const char *s) {
    size_t n = strlen(s) + 1;
    if (ctx->arena_used + n > sizeof ctx->arena) { ctx->overflow = true; return NULL; }
    char *dst = ctx->arena + ctx->arena_used;
    memcpy(dst, s, n);
    ctx->arena_used += n;
    return dst;
}

void add_slot(ParseContext *ctx, SlotType type, const char *value) {
    if (ctx->slot_count >= NL_MAX_SLOTS) { ctx->overflow = true; return; }
    const char *v = arena_dup(ctx, value);
    if (!v) return;
    ctx->slots[ctx->slot_count].type = type;
    ctx->slots[ctx->slot_count].value = v;
    ctx->slot_count++;
}

void add_intent(ParseContext *ctx, const char *word) {
    if (ctx->token_count >= NL_MAX_TOKENS) { ctx->overflow = true; return; }
    const char *v = arena_dup(ctx, word);
    if (!v) return;
    ctx->intent_tokens[ctx->token_count++] = v;
}

void format_seconds(char *buf, size_t size, double secs) {
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

void lex_intent(ParseContext *ctx, const char *lw) {
    if (lw[0] == '\0' || in_list(lw, STOPWORDS)) return;
    add_intent(ctx, lw);
}

/* Is `w` a keyword of any template for `tool`? Such words keep their
 * meaning as intent even if the JSON happens to have a same-named field. */
bool is_keyword(ToolId tool, const char *w) {
    for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
        if (NL_TEMPLATES[i].tool != tool) continue;
        for (size_t k = 0; k < NL_MAX_KEYWORDS && NL_TEMPLATES[i].keywords[k]; k++)
            if (strcmp(NL_TEMPLATES[i].keywords[k], w) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Phrases shared by several tools                                    */
/* ------------------------------------------------------------------ */

/* grep-style modifier phrases (grep, and "ignoring case" in find).
 * Matched on consecutive raw tokens, consumed (they are
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

/* Extensions that name a file type on their own (".py", "c files"); also
 * the 1-char ones the generic name.ext rule in looks_like_file misses. */
const char *const GREP_EXTS[] = {
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

void add_flag(ParseContext *ctx, char f) {
    size_t n = strlen(ctx->flags);
    if (strchr(ctx->flags, f) || n + 1 >= sizeof ctx->flags) return;
    ctx->flags[n] = f;
    ctx->flags[n + 1] = '\0';
}

/* Length of the modifier phrase starting at toks[i], or 0. */
size_t grep_modifier_at(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
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
const char *grep_type_ext(const char *w) {
    for (size_t i = 0; GREP_LANGS[i].name; i++)
        if (strcmp(w, GREP_LANGS[i].name) == 0) return GREP_LANGS[i].ext;
    return in_list(w, GREP_EXTS) ? w : NULL;
}

bool is_alnum_word(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n > max) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i])) return false;
    return true;
}

/* The word before toks[i], skipping articles/quantifiers ("for all TODO"). */
const char *prev_word(const RawTok *toks, size_t i, size_t *at) {
    static const char *const skip[] = { "the", "a", "an", "all", "any", "every", NULL };
    while (i > 0) {
        i--;
        if (toks[i].quoted) return NULL;
        if (!in_list(toks[i].lower, skip)) { if (at) *at = i; return toks[i].lower; }
    }
    return NULL;
}

/* "ignoring build", "excluding .git and node_modules" */
const char *const FIND_EXCLUDE_WORDS[] = {
    "ignoring", "ignore", "excluding", "exclude", "except", "skip", "skipping",
    "omit", "omitting", "outside", "without", NULL
};

const char *const FIND_DIR_NOUNS[] = {
    "directories", "directory", "dirs", "dir", "folders", "folder",
    "subdirectories", "subdirs", "subfolders", NULL
};

const char *const FIND_FILE_NOUNS[] = { "files", "file", NULL };

double find_time_unit(const char *u, bool m_is_minutes) {
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

void find_add_exclude(ParseContext *ctx, const char *t) {
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
size_t exclude_item(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
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

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

static const NlLexFn LEXERS[TOOL_COUNT] = {
    [TOOL_FFMPEG] = nl_lex_ffmpeg,
    [TOOL_JQ]     = nl_lex_jq,
    [TOOL_GREP]   = nl_lex_grep,
    [TOOL_FIND]   = nl_lex_find,
    [TOOL_TAR]    = nl_lex_tar,
    [TOOL_GIT]    = nl_lex_git,
    [TOOL_CURL]   = nl_lex_curl,
    [TOOL_MAGICK] = nl_lex_magick,
};

bool nl_lex(ParseContext *ctx, ToolId tool, const char *input, const JsonInfo *json) {
    memset(ctx, 0, sizeof *ctx);
    ctx->tool = tool;

    size_t in_len = strlen(input);
    if (in_len >= NL_MAX_INPUT) { ctx->overflow = true; return false; }
    if (tool <= TOOL_UNKNOWN || tool >= TOOL_COUNT || !LEXERS[tool]) return false;

    char buf[NL_MAX_INPUT];
    char lowbuf[NL_MAX_INPUT + NL_MAX_RAW];
    RawTok toks[NL_MAX_RAW];
    memcpy(buf, input, in_len + 1);
    /* Control characters (but tab) separate words like spaces, quoted or
     * not, so none reaches a slot: an escape sequence in a file name would
     * let the printed command redraw the terminal before -x asks. */
    for (char *p = buf; *p; p++)
        if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f) *p = ' ';

    size_t count = tokenize(buf, toks, NL_MAX_RAW, lowbuf, &ctx->overflow);
    LEXERS[tool](ctx, toks, count, json);
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
    if (strcmp(name, "git") == 0) return TOOL_GIT;
    if (strcmp(name, "curl") == 0) return TOOL_CURL;
    if (strcmp(name, "magick") == 0 || strcmp(name, "convert") == 0 ||
        strcmp(name, "imagemagick") == 0)
        return TOOL_MAGICK;
    return TOOL_UNKNOWN;
}

const char *nl_tool_name(ToolId tool) {
    switch (tool) {
    case TOOL_FFMPEG: return "ffmpeg";
    case TOOL_JQ:     return "jq";
    case TOOL_GREP:   return "grep";
    case TOOL_FIND:   return "find";
    case TOOL_TAR:    return "tar";
    case TOOL_GIT:    return "git";
    case TOOL_CURL:   return "curl";
    case TOOL_MAGICK: return "magick";
    default:         return "unknown";
    }
}

static const struct { const char *name; const char *hint; } SLOT_INFO[SLOT_TYPE_COUNT] = {
    [SLOT_NONE]       = { "NONE",       "" },
    [SLOT_FILE]       = { "FILE",       "a file path (e.g. input.mp4, data.json)" },
    [SLOT_EXT]        = { "EXT",        "a format or file type (e.g. mp4, gif, py)" },
    [SLOT_TIME]       = { "TIME",       "a timestamp (e.g. 00:10, 1:30, 15s)" },
    [SLOT_RES]        = { "RES",        "a resolution or size (e.g. 720p, 1920x1080, width 800, 50%)" },
    [SLOT_BITRATE]    = { "BITRATE",    "a bitrate (e.g. 128k, 2M)" },
    [SLOT_FPS]        = { "FPS",        "a frame rate (e.g. 30fps)" },
    [SLOT_FACTOR]     = { "FACTOR",     "a multiplier (e.g. 2x, 1.5x), or for magick an amount "
                                        "(e.g. fuzz 10%, 90 degrees, 10px border)" },
    [SLOT_NUM]        = { "NUM",        "a number (e.g. 10)" },
    [SLOT_QUOTED_STR] = { "QUOTED_STR", "a quoted string or search pattern (e.g. 'active', 'TODO')" },
    [SLOT_JSON_PATH]  = { "JSON_PATH",  "a JSON path (e.g. .users[].email)" },
    [SLOT_SIZE]       = { "SIZE",       "a size (e.g. larger than 100MB, under 4k)" },
    [SLOT_EXCLUDE]    = { "EXCLUDE",    "a directory to skip (e.g. ignoring node_modules), or for git "
                                        "a branch to leave out (e.g. not on main)" },
    [SLOT_LINES]      = { "LINES",      "a line count (e.g. less than 100 lines) or range (lines 10 to 20)" },
    [SLOT_ARCHIVE]    = { "ARCHIVE",    "a tar archive (e.g. backup.tar.gz, logs.tgz, dist.tar.xz)" },
    [SLOT_DEST]       = { "DEST",       "a destination (e.g. into dist/)" },
    [SLOT_REV]        = { "REV",        "a revision (e.g. from main, 3 commits ago, a1b2c3d, HEAD~2)" },
    [SLOT_BRANCH]     = { "BRANCH",     "a branch or tag name (e.g. branch feature-x)" },
    [SLOT_AUTHOR]     = { "AUTHOR",     "an author (e.g. by alice), or for curl a user (e.g. as user alice)" },
    [SLOT_URL]        = { "URL",        "a URL (e.g. https://api.example.com/users)" },
    [SLOT_HEADER]     = { "HEADER",     "a header (e.g. header 'X-Api-Key: abc', token $TOKEN)" },
    [SLOT_COLOR]      = { "COLOR",      "a colour (e.g. white, red, '#ff8800')" },
};

const char *nl_slot_name(SlotType type) {
    return type < SLOT_TYPE_COUNT ? SLOT_INFO[type].name : "?";
}

const char *nl_slot_hint(SlotType type) {
    return type < SLOT_TYPE_COUNT ? SLOT_INFO[type].hint : "";
}
