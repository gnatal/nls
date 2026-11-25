/*
 * lexer_jq.c — jq: JSON queries: paths, filters, counts, sorting, conversion.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    "after", "before", "where", "whose", "with", "between", "within", "outside",
    "beyond", NULL
};

/* jq: a JSON path followed by one of these is a *condition* path */
static const char *const JQ_COND_FOLLOWERS[] = {
    "is", "equals", "equal", "==", "=", "!=", ">", "<", ">=", "<=",
    "contains", "containing", "contain", "includes", "include", "has",
    "matches", "match", "matching", "like", "not", "isnt", "isn't",
    "greater", "less", "more", "fewer", "above", "below", "over", "under",
    "bigger", "smaller", "larger", "higher", "lower", "older", "younger",
    "exceeds", "after", "before", "at", "exists", "exist", "present",
    "defined", "between", "within", "outside", "beyond", NULL
};

/* jq: a path right after one of these is the condition, whatever follows */
static const char *const JQ_WHERE_WORDS[] = { "where", "whose", "if", "when", NULL };

/* jq: words that may follow a "where" path without being a comparator */
static const char *const JQ_NOT_OPS[] = {
    "and", "or", "but", "then", "from", "in", "of", NULL
};

/* "from 20 to 40" at toks[i]: an inclusive range */
static bool from_to_range(const RawTok *toks, size_t count, size_t i) {
    return i + 3 < count && !toks[i].quoted && strcmp(toks[i].lower, "from") == 0 &&
           is_number(toks[i + 1].text, true) && strcmp(toks[i + 2].lower, "to") == 0 &&
           is_number(toks[i + 3].text, true);
}

/* Is the path at toks[i] the subject of "where"/"whose"/"if"? Remembers an
 * unknown comparator right after it ("where .age around 30"), so the
 * caller can refuse rather than guess what it means. */
static bool where_path(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                       const char *path) {
    size_t at = i;
    const char *pw = prev_word(toks, i, &at);
    /* "where either .a is 1 or ...", "where both .a ... and ..." */
    if (pw && (strcmp(pw, "either") == 0 || strcmp(pw, "both") == 0))
        pw = prev_word(toks, at, &at);
    if (!pw || !in_list(pw, JQ_WHERE_WORDS)) return false;
    if (i + 1 < count && !toks[i + 1].quoted) {
        const char *nx = toks[i + 1].lower;
        bool word = isalpha((unsigned char)nx[0]);
        if (word && !in_list(nx, JQ_COND_FOLLOWERS) && !in_list(nx, JQ_NOT_OPS) &&
            !looks_like_file(toks[i + 1].text, TOOL_JQ) && !ctx->jq_bad_op) {
            ctx->jq_bad_op = arena_dup(ctx, nx);
            ctx->jq_bad_path = arena_dup(ctx, path);
        }
    }
    return true;
}

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

void nl_lex_jq(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
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
            add_slot(ctx, ext && ext_in(ext, JQ_EXTS) ? SLOT_FILE : SLOT_QUOTED_STR, t);
            continue;
        }
        /* Count intent: "count", "how many/much", "number of" */
        if (strcmp(lw, "count") == 0 ||
            (strcmp(lw, "how") == 0 && next && (strcmp(next, "many") == 0 || strcmp(next, "much") == 0)) ||
            (strcmp(lw, "number") == 0 && next && strcmp(next, "of") == 0))
            ctx->wants_count = true;

        /* 2. JSON paths */
        if (is_json_path(t)) {
            if (path_count < NL_MAX_SLOTS) {
                bool cond = where_path(ctx, toks, count, i, t) ||
                            (next && in_list(next, JQ_COND_FOLLOWERS)) ||
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
        if (looks_like_file(t, TOOL_JQ)) { add_slot(ctx, SLOT_FILE, t); continue; }
        /* 5. Bare value after a comparator: "where .name is bob" */
        if (seen_path && prev && !in_list(lw, JQ_RESERVED) &&
            (in_list(prev, JQ_VALUE_TRIGGERS) ||
             (strcmp(prev, "to") == 0 && prev2 &&
              (strcmp(prev2, "equal") == 0 || strcmp(prev2, "equals") == 0)))) {
            add_slot(ctx, SLOT_QUOTED_STR, t);
            continue;
        }
        /* 6. Ranges: "from 20 to 40" means between; "not between" means outside */
        if (seen_path && from_to_range(toks, count, i)) { add_intent(ctx, "between"); continue; }
        if ((strcmp(lw, "between") == 0 || strcmp(lw, "within") == 0) && prev &&
            (strcmp(prev, "not") == 0 || strcmp(prev, "isnt") == 0 || strcmp(prev, "isn't") == 0) &&
            ctx->token_count > 0 && strcmp(ctx->intent_tokens[ctx->token_count - 1], prev) == 0) {
            ctx->intent_tokens[ctx->token_count - 1] = "outside";
            continue;
        }
        /* 7. Bare field names from the input file: "emails" -> .email */
        const char *field = field_for_word(json, lw);
        if (field && !in_list(lw, STOPWORDS) && !is_keyword(TOOL_JQ, lw)) {
            char path[40];
            snprintf(path, sizeof path, ".%s", field);
            if (path_count < NL_MAX_SLOTS) {
                bool cond = where_path(ctx, toks, count, i, path) ||
                            (next && in_list(next, JQ_COND_FOLLOWERS)) ||
                            (prev && (in_list(prev, JQ_KEY_LEADERS) || in_list(prev, JQ_COND_LEADERS)));
                path_cond[path_count++] = cond;
                if (cond) ctx->cond_paths++;
            }
            add_slot(ctx, SLOT_JSON_PATH, path);
            seen_path = true;
            continue;
        }
        lex_intent(ctx, lw);
    }

    if (path_count > 1) reorder_jq_paths(ctx, path_cond);

    /* "between 40 and 20": put the bounds in order so the range isn't empty */
    bool range = false;
    for (uint8_t i = 0; i < ctx->token_count; i++)
        if (strcmp(ctx->intent_tokens[i], "between") == 0 ||
            strcmp(ctx->intent_tokens[i], "within") == 0 ||
            strcmp(ctx->intent_tokens[i], "outside") == 0 ||
            strcmp(ctx->intent_tokens[i], "beyond") == 0) range = true;
    int a = -1, b = -1;
    for (uint8_t i = 0; range && i < ctx->slot_count && b < 0; i++) {
        if (ctx->slots[i].type != SLOT_NUM) continue;
        if (a < 0) a = i; else b = i;
    }
    if (b >= 0 && is_number(ctx->slots[a].value, true) && is_number(ctx->slots[b].value, true) &&
        strtod(ctx->slots[a].value, NULL) > strtod(ctx->slots[b].value, NULL)) {
        const char *t = ctx->slots[a].value;
        ctx->slots[a].value = ctx->slots[b].value;
        ctx->slots[b].value = t;
    }
}
