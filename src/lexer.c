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

    bool path_cond[NL_MAX_SLOTS] = { false };
    size_t path_count = 0;
    bool seen_path = false;

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        const char *t = toks[i].text;
        const char *lw = toks[i].lower;
        const char *prev = i > 0 ? toks[i - 1].lower : NULL;
        const char *prev2 = i > 1 ? toks[i - 2].lower : NULL;
        const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;

        /* 1. Quoted strings */
        if (toks[i].quoted) {
            const char *ext = extension_of(t);
            bool file = tool == TOOL_JQ ? (ext && ext_in(ext, JQ_EXTS)) : looks_like_file(t, tool);
            add_slot(ctx, file ? SLOT_FILE : SLOT_QUOTED_STR, t);
            continue;
        }

        if (tool == TOOL_JQ) {
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
    return TOOL_UNKNOWN;
}

const char *nl_tool_name(ToolId tool) {
    switch (tool) {
    case TOOL_FFMPEG: return "ffmpeg";
    case TOOL_JQ:     return "jq";
    default:          return "unknown";
    }
}

static const struct { const char *name; const char *hint; } SLOT_INFO[SLOT_TYPE_COUNT] = {
    [SLOT_NONE]       = { "NONE",       "" },
    [SLOT_FILE]       = { "FILE",       "a file path (e.g. input.mp4, data.json)" },
    [SLOT_EXT]        = { "EXT",        "a target format (e.g. mp4, mp3, gif)" },
    [SLOT_TIME]       = { "TIME",       "a timestamp (e.g. 00:10, 1:30, 15s)" },
    [SLOT_RES]        = { "RES",        "a resolution (e.g. 720p, 1920x1080)" },
    [SLOT_BITRATE]    = { "BITRATE",    "a bitrate (e.g. 128k, 2M)" },
    [SLOT_FPS]        = { "FPS",        "a frame rate (e.g. 30fps)" },
    [SLOT_FACTOR]     = { "FACTOR",     "a multiplier (e.g. 2x, 1.5x)" },
    [SLOT_NUM]        = { "NUM",        "a number (e.g. 10)" },
    [SLOT_QUOTED_STR] = { "QUOTED_STR", "a quoted string (e.g. 'active')" },
    [SLOT_JSON_PATH]  = { "JSON_PATH",  "a JSON path (e.g. .users[].email)" },
};

const char *nl_slot_name(SlotType type) {
    return type < SLOT_TYPE_COUNT ? SLOT_INFO[type].name : "?";
}

const char *nl_slot_hint(SlotType type) {
    return type < SLOT_TYPE_COUNT ? SLOT_INFO[type].hint : "";
}
