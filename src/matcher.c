/*
 * matcher.c — slot-gated, weighted keyword-overlap template selection.
 *
 * Score = sum over the template's keywords of the best match against any
 * intent token: exact +10, plural (+s/+es) +8, shared 4-char stem +6.
 * Templates whose required slots are not all present are "blocked"; the
 * best blocked candidate is kept for diagnostics.
 * Ties go to the template that uses more of the user's slots, then to the
 * one with more required slots (more specific), then to the one listed first.
 */
#include "nl.h"

#include <stdio.h>
#include <string.h>

static int token_similarity(const char *tok, const char *kw) {
    if (strcmp(tok, kw) == 0) return 10;

    size_t lt = strlen(tok), lk = strlen(kw);
    if (lt == lk + 1 && tok[lt - 1] == 's' && strncmp(tok, kw, lk) == 0) return 8;
    if (lt == lk + 2 && strcmp(tok + lk, "es") == 0 && strncmp(tok, kw, lk) == 0) return 8;
    if (lk == lt + 1 && kw[lk - 1] == 's' && strncmp(tok, kw, lt) == 0) return 8;

    if (lt >= 4 && lk >= 4 && strncmp(tok, kw, 4) == 0) return 6;
    return 0;
}

/* One-to-one matching: each intent token can satisfy at most one keyword
 * (so "frames" can't score for both "frame" and "framerate"). Exact
 * matches are claimed first, then plurals, then stems. */
int nl_keyword_score(const ParseContext *ctx, const CommandTemplate *tpl) {
    static const int levels[] = { 10, 8, 6 };
    bool tok_used[NL_MAX_TOKENS] = { false };
    bool kw_done[NL_MAX_KEYWORDS] = { false };
    int total = 0;

    for (size_t l = 0; l < sizeof levels / sizeof levels[0]; l++) {
        for (size_t k = 0; k < NL_MAX_KEYWORDS && tpl->keywords[k]; k++) {
            if (kw_done[k]) continue;
            for (size_t t = 0; t < ctx->token_count; t++) {
                if (tok_used[t]) continue;
                if (token_similarity(ctx->intent_tokens[t], tpl->keywords[k]) == levels[l]) {
                    tok_used[t] = kw_done[k] = true;
                    total += levels[l];
                    break;
                }
            }
        }
    }
    return total;
}

static size_t required_count(const CommandTemplate *tpl) {
    size_t n = 0;
    while (n < NL_MAX_REQUIRED && tpl->required_slots[n] != SLOT_NONE) n++;
    return n;
}

/* Number of distinct {TYPE:i} placeholders the parsed input can fill.
 * Used as the tie-breaker: on equal keyword score, prefer the template that
 * makes use of more of what the user actually said. */
static int slots_used(const ParseContext *ctx, const CommandTemplate *tpl) {
    char seen[NL_MAX_SLOTS * 4][24];
    int nseen = 0, used = 0;

    /* {OUT_FILE} implicitly consumes an explicit output file (FILE:1) and the
     * target format (EXT:0) unless its extension is forced with '!'. */
    const char *out = strstr(tpl->flag_template, "{OUT_FILE");
    if (out) {
        if (nl_find_slot(ctx, SLOT_FILE, 1)) {
            used++;
            memcpy(seen[nseen++], "FILE:1", 7);
        }
        if (strncmp(out, "{OUT_FILE|!", 11) != 0 && nl_find_slot(ctx, SLOT_EXT, 0)) {
            used++;
            memcpy(seen[nseen++], "EXT:0", 6);
        }
    }
    for (const char *p = tpl->flag_template; (p = strchr(p, '{')) != NULL; p++) {
        const char *colon = p + 1;
        while ((*colon >= 'A' && *colon <= 'Z') || *colon == '_') colon++;
        if (*colon != ':' || colon == p + 1) continue;
        const char *end = colon + 1;
        int idx = 0;
        while (*end >= '0' && *end <= '9') idx = idx * 10 + (*end++ - '0');

        char key[24];
        size_t name_len = (size_t)(colon - (p + 1));
        if (name_len >= 16) continue;
        snprintf(key, sizeof key, "%.*s:%d", (int)name_len, p + 1, idx);
        bool dup = false;
        for (int s = 0; s < nseen; s++) if (strcmp(seen[s], key) == 0) { dup = true; break; }
        if (dup || nseen >= (int)(sizeof seen / sizeof seen[0])) continue;
        memcpy(seen[nseen++], key, sizeof key);

        for (int type = 1; type < SLOT_TYPE_COUNT; type++) {
            const char *n = nl_slot_name((SlotType)type);
            if (strlen(n) == name_len && strncmp(n, p + 1, name_len) == 0) {
                if (nl_find_slot(ctx, (SlotType)type, idx)) used++;
                break;
            }
        }
    }
    return used;
}

/* Does the template fit the peeked top-level JSON shape (if known)? */
static bool shape_ok(const ParseContext *ctx, const CommandTemplate *tpl) {
    if (ctx->input_shape == 0 || tpl->shape == SHAPE_ANY) return true;
    if (tpl->shape == SHAPE_ARRAY) return ctx->input_shape == '[';
    /* SHAPE_OBJECT: a path like .[0] or .[] is fine on an array root */
    const char *path = nl_find_slot(ctx, SLOT_JSON_PATH, 0);
    if (ctx->input_shape == '[') return path && strncmp(path, ".[", 2) == 0;
    return true;
}

/* Fills `missing` with one entry per unsatisfied required slot. */
static uint8_t check_slots(const ParseContext *ctx, const CommandTemplate *tpl,
                           SlotType missing[NL_MAX_REQUIRED]) {
    int need[SLOT_TYPE_COUNT] = { 0 };
    size_t req = required_count(tpl);
    for (size_t i = 0; i < req; i++) need[tpl->required_slots[i]]++;

    uint8_t n = 0;
    for (int type = 1; type < SLOT_TYPE_COUNT; type++) {
        int have = nl_count_slots(ctx, (SlotType)type);
        for (int k = have; k < need[type] && n < NL_MAX_REQUIRED; k++)
            missing[n++] = (SlotType)type;
    }
    return n;
}

void nl_match(const ParseContext *ctx, MatchResult *out, bool verbose) {
    memset(out, 0, sizeof *out);
    int best_used = 0;
    size_t best_req = 0;
    const CommandTemplate *best_filter = NULL;  /* for "how many ... where ..." */
    int best_filter_score = 0;

    if (verbose) fprintf(stderr, "candidates:\n");

    for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
        const CommandTemplate *tpl = &NL_TEMPLATES[i];
        if (tpl->tool != ctx->tool) continue;

        int score = nl_keyword_score(ctx, tpl);
        if (score <= 0) continue;

        if (!shape_ok(ctx, tpl)) {
            if (verbose)
                fprintf(stderr, "  %-24s score=%-3d skipped: input is an %s\n", tpl->id, score,
                        ctx->input_shape == '[' ? "array" : "object");
            if (!out->wrong_shape || score > out->wrong_shape_score) {
                out->wrong_shape = tpl;
                out->wrong_shape_score = score;
            }
            continue;
        }

        SlotType missing[NL_MAX_REQUIRED];
        uint8_t nmissing = check_slots(ctx, tpl, missing);

        int used = slots_used(ctx, tpl);

        if (verbose) {
            fprintf(stderr, "  %-24s score=%-3d used=%d %s", tpl->id, score, used,
                    nmissing ? "blocked, missing:" : "ok");
            for (uint8_t m = 0; m < nmissing; m++)
                fprintf(stderr, " %s", nl_slot_name(missing[m]));
            fputc('\n', stderr);
        }

        if (nmissing == 0) {
            size_t req = required_count(tpl);
            bool better = !out->best || score > out->best_score ||
                          (score == out->best_score &&
                           (used > best_used || (used == best_used && req > best_req)));
            if (better) {
                out->best = tpl;
                out->best_score = score;
                best_used = used;
                best_req = req;
            }
            if (strncmp(tpl->id, "jq_filter_", 10) == 0 && score > best_filter_score) {
                best_filter = tpl;
                best_filter_score = score;
            }
        } else {
            bool better = !out->blocked || score > out->blocked_score ||
                          (score == out->blocked_score && nmissing < out->missing_count);
            if (better) {
                out->blocked = tpl;
                out->blocked_score = score;
                out->missing_count = nmissing;
                memcpy(out->missing, missing, sizeof missing);
            }
        }
    }

    /* grep: a pattern with no recognizable verb ("'TODO' in src/")
     * means a plain search; find: "c files in src" means a plain listing.
     * Either way: the tool's first template that can run. */
    if (!out->best && (ctx->tool == TOOL_GREP || ctx->tool == TOOL_FIND)) {
        for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
            const CommandTemplate *tpl = &NL_TEMPLATES[i];
            SlotType missing[NL_MAX_REQUIRED];
            if (tpl->tool != ctx->tool || check_slots(ctx, tpl, missing) != 0) continue;
            if (verbose) fprintf(stderr, "no keyword matched: defaulting to %s\n", tpl->id);
            out->best = tpl;
            out->best_score = 0;
            break;
        }
    }

    /* "how many users have emails": counting wraps a filter ({COUNT} appends
     * "| length"), so a matching filter beats the plain count templates. */
    if (ctx->wants_count && best_filter) {
        if (verbose) fprintf(stderr, "count intent: using filter %s\n", best_filter->id);
        out->best = best_filter;
        out->best_score = best_filter_score;
        if (out->blocked && out->blocked_score > best_filter_score &&
            strncmp(out->blocked->id, "jq_filter_", 10) != 0)
            out->blocked = NULL;
    }
}
