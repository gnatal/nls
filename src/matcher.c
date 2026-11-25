/*
 * matcher.c — slot-gated, weighted keyword-overlap template selection.
 *
 * Score = sum over the template's keywords of the best match against any
 * intent token: exact +10, inflection (plural, or the same stem after
 * light suffix stripping: -s, -ing, -ed, comparative -er) +8, built-in
 * synonym +7, shared 4-char prefix +6.
 * Templates whose required slots are not all present are "blocked"; the
 * best blocked candidate is kept for diagnostics. Templates the tool's
 * expander would refuse (e.g. git_diff with a date) are "rejected" and only
 * chosen when nothing else can run, so the refusal still gets reported.
 * Ties go to the template that uses more of the user's slots, then to the
 * one with more required slots (more specific), then to the one that
 * matched more real words rather than filler ("make", "what", "everything"),
 * then to the one listed first. Close runners-up are reported so the caller
 * can warn.
 */
#include "nl.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Word normalization: stems and synonyms                             */
/* ------------------------------------------------------------------ */

#define STEM_MAX 32

/* -er is only stripped from these: as a general rule it would turn
 * "container" into "contain" and "header" into "head". */
static const char *const COMPARATIVES[] = {
    "smaller", "bigger", "larger", "greater", "faster", "slower", "newer", "older",
    "lower", "higher", "shorter", "longer", "louder", "quieter", "softer", "brighter",
    "darker", "sharper", "lighter", "heavier", "wider", "narrower", "cheaper",
    "thinner", "thicker", "smoother", NULL
};

/* Words that mean the same thing to every tool. A user word matches a
 * template keyword from the same group at synonym strength (+7), below an
 * exact or inflected match, so a template that lists the word itself still
 * wins. Members are compared by stem ("shrinking" is in the first group). */
#define SYN_WORDS 8
static const char *const SYNONYMS[][SYN_WORDS] = {
    { "compress", "shrink", "reduce", "smaller", "downsize", "squeeze", NULL },
    { "undo", "revert", "rollback", NULL },
    { "show", "get", "list", "print", "display", NULL },
    { "delete", "remove", "erase", NULL },
    { "search", "find", "look", "locate", "lookup", NULL },
    { "combine", "merge", "join", "concatenate", "concat", NULL },
    { "extract", "unpack", "decompress", "unzip", "untar", NULL },
    { "image", "picture", "photo", "img", NULL },
    { "mute", "silence", "silent", NULL },
    { "modified", "changed", "edited", "updated", "altered", NULL },
    { "big", "large", "huge", NULL },
};
#define SYN_GROUPS (sizeof SYNONYMS / sizeof SYNONYMS[0])

/* Filler that templates list as keywords but that says little about which
 * command is meant: on equal score, a template matched by other words wins
 * ("make video.mp4 smaller" is a compression, not a GIF). */
static const char *const WEAK_WORDS[] = {
    "make", "create", "do", "turn", "convert", "take", "what", "which", "everything", "all", "every",
    "want", "need", "give", "some", "something", "stuff", "thing", "things", NULL
};

/* Generic verbs and nouns that don't by themselves point at a different
 * command: ignoring them is no reason to doubt a match. Only used for the
 * confidence checks (as a tie-breaker "get" must still count: "get URL and
 * save to x" is a GET, not a download). With a count intent, the count
 * words are spoken for as well. */
static const char *const GENERIC_WORDS[] = {
    "show", "list", "get", "print", "display", "view", "see", "about", "value",
    "values", "item", "items", NULL
};
/* Markers the git lexer puts in for "this commit" / "the last commit": they
 * say which commit, not what to do with it. */
static const char *const NOUN_MARKERS[] = { "@commit", "@last", NULL };
static const char *const COUNT_WORDS[] = { "count", "how", "many", "much", "number", NULL };

static bool ends_with(const char *s, size_t n, const char *suffix) {
    size_t k = strlen(suffix);
    return n >= k && memcmp(s + n - k, suffix, k) == 0;
}

static bool in_words(const char *s, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(s, list[i]) == 0) return true;
    return false;
}

/* Light, conservative suffix stripping so inflections share a stem:
 * changes/changed/changing -> "chang", files/file -> "fil", cutting -> "cut",
 * copies/copied -> "copy", bigger -> "big". Only compared for equality. */
static void stem_word(const char *w, char out[STEM_MAX]) {
    size_t n = strlen(w);
    if (n >= STEM_MAX) n = STEM_MAX - 1;
    memcpy(out, w, n);
    out[n] = '\0';
    if (n <= 3) return;

    if (n > 4 && (ends_with(out, n, "ies") || ends_with(out, n, "ied"))) {
        out[n - 3] = 'y';
        out[n - 2] = '\0';
        return;
    }
    /* plural / 3rd person -s, but not "class", "status", "analysis", "alias" */
    if (out[n - 1] == 's' && !ends_with(out, n, "ss") && !ends_with(out, n, "us") &&
        !ends_with(out, n, "is") && !ends_with(out, n, "as"))
        out[--n] = '\0';

    bool stripped = false;
    if (ends_with(out, n, "ing") && n - 3 >= 3) { n -= 3; stripped = true; }
    else if (ends_with(out, n, "ed") && n - 2 >= 3) { n -= 2; stripped = true; }
    else if (in_words(w, COMPARATIVES)) {
        if (ends_with(out, n, "ier")) { out[n - 3] = 'y'; n -= 2; }
        else n -= 2;
        stripped = true;
    }
    out[n] = '\0';
    /* cutting -> cutt -> cut, bigger -> bigg -> big; keep "small", "stuff" */
    if (stripped && n >= 4 && out[n - 1] == out[n - 2] && !strchr("lsfz", out[n - 1]))
        out[--n] = '\0';
    /* make/making -> "mak", size/sized -> "siz" */
    if (n > 3 && out[n - 1] == 'e') out[--n] = '\0';
}

static char syn_stems[SYN_GROUPS][SYN_WORDS][STEM_MAX];
static bool syn_ready = false;

static unsigned syn_mask(const char *stem) {
    if (!syn_ready) {
        for (size_t g = 0; g < SYN_GROUPS; g++)
            for (size_t k = 0; k < SYN_WORDS && SYNONYMS[g][k]; k++)
                stem_word(SYNONYMS[g][k], syn_stems[g][k]);
        syn_ready = true;
    }
    unsigned mask = 0;
    for (size_t g = 0; g < SYN_GROUPS; g++)
        for (size_t k = 0; k < SYN_WORDS && SYNONYMS[g][k]; k++)
            if (strcmp(stem, syn_stems[g][k]) == 0) { mask |= 1u << g; break; }
    return mask;
}

typedef struct {
    const char *w;
    char stem[STEM_MAX];
    unsigned syn;
} Word;

static void word_init(Word *x, const char *w) {
    x->w = w;
    stem_word(w, x->stem);
    x->syn = syn_mask(x->stem);
}

static int similarity(const Word *tok, const Word *kw) {
    const char *t = tok->w, *k = kw->w;
    if (strcmp(t, k) == 0) return 10;

    size_t lt = strlen(t), lk = strlen(k);
    if (lt == lk + 1 && t[lt - 1] == 's' && strncmp(t, k, lk) == 0) return 8;
    if (lt == lk + 2 && strcmp(t + lk, "es") == 0 && strncmp(t, k, lk) == 0) return 8;
    if (lk == lt + 1 && k[lk - 1] == 's' && strncmp(t, k, lt) == 0) return 8;
    if (lt > 3 && lk > 3 && strcmp(tok->stem, kw->stem) == 0) return 8;

    if (tok->syn & kw->syn) return 7;

    if (lt >= 4 && lk >= 4 && strncmp(t, k, 4) == 0) return 6;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Keyword scoring                                                    */
/* ------------------------------------------------------------------ */

static void tokens_init(const ParseContext *ctx, Word toks[NL_MAX_TOKENS]) {
    for (size_t t = 0; t < ctx->token_count; t++) word_init(&toks[t], ctx->intent_tokens[t]);
}

/* One-to-one matching: each intent token can satisfy at most one keyword
 * (so "frames" can't score for both "frame" and "framerate"). Exact
 * matches are claimed first, then inflections, synonyms and prefixes.
 * *used_mask gets bit t set for each intent token t that scored. */
static int score_words(const ParseContext *ctx, const Word *toks, const CommandTemplate *tpl,
                       uint32_t *used_mask) {
    static const int levels[] = { 10, 8, 7, 6 };
    Word kws[NL_MAX_KEYWORDS];
    size_t nkw = 0;
    while (nkw < NL_MAX_KEYWORDS && tpl->keywords[nkw]) { word_init(&kws[nkw], tpl->keywords[nkw]); nkw++; }

    bool tok_used[NL_MAX_TOKENS] = { false };
    bool kw_done[NL_MAX_KEYWORDS] = { false };
    int total = 0;
    uint32_t mask = 0;

    for (size_t l = 0; l < sizeof levels / sizeof levels[0]; l++) {
        for (size_t k = 0; k < nkw; k++) {
            if (kw_done[k]) continue;
            for (size_t t = 0; t < ctx->token_count; t++) {
                if (tok_used[t]) continue;
                if (similarity(&toks[t], &kws[k]) == levels[l]) {
                    tok_used[t] = kw_done[k] = true;
                    mask |= 1u << t;
                    total += levels[l];
                    break;
                }
            }
        }
    }
    if (used_mask) *used_mask = mask;
    return total;
}

int nl_keyword_score(const ParseContext *ctx, const CommandTemplate *tpl) {
    Word toks[NL_MAX_TOKENS];
    tokens_init(ctx, toks);
    return score_words(ctx, toks, tpl, NULL);
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
        if (ctx->tool == TOOL_MAGICK ? nl_find_slot(ctx, SLOT_DEST, 0) != NULL
                                     : nl_find_slot(ctx, SLOT_FILE, 1) != NULL) {
            used++;
            memcpy(seen[nseen++], ctx->tool == TOOL_MAGICK ? "DEST:0" : "FILE:1", 7);
        }
        /* an open extension takes the requested format; a forced one
         * ({OUT_FILE|!gif}) only counts when it is the format asked for */
        const char *ext = nl_find_slot(ctx, SLOT_EXT, 0);
        bool forced = strncmp(out, "{OUT_FILE|!", 11) == 0;
        size_t flen = forced ? strcspn(out + 11, "|}") : 0;
        if (ext && (!forced || (strlen(ext) == flen && strncmp(out + 11, ext, flen) == 0))) {
            used++;
            memcpy(seen[nseen++], "EXT:0", 6);
        }
    }
    /* git builders consume slots without a {TYPE:i} placeholder */
    if (ctx->tool == TOOL_GIT) {
        const char *t = tpl->flag_template;
        if (strstr(t, "{PATHS") && nl_find_slot(ctx, SLOT_FILE, 0)) used++;
        if (strstr(t, "{AUTHOR}") && nl_find_slot(ctx, SLOT_AUTHOR, 0)) used++;
        if (strstr(t, "{SPAN}") || strstr(t, "{BASE}") || strstr(t, "{COMMITS}") ||
            strstr(t, "{TARGET}")) {
            if (nl_find_slot(ctx, SLOT_NUM, 0)) { used++; memcpy(seen[nseen++], "NUM:0", 6); }
            if (nl_find_slot(ctx, SLOT_REV, 0)) { used++; memcpy(seen[nseen++], "REV:0", 6); }
        }
    }
    /* curl builders consume slots without a {TYPE:i} placeholder */
    if (ctx->tool == TOOL_CURL) {
        const char *t = tpl->flag_template;
        bool body = strstr(t, "{BODY}") || strstr(t, "{UPLOAD}");
        if (nl_find_slot(ctx, SLOT_DEST, 0) && strstr(t, "{OUTPUT")) used++;
        if (nl_find_slot(ctx, SLOT_FILE, 0) && (body || strstr(t, "{OUTPUT|O}"))) used++;
        if (nl_find_slot(ctx, SLOT_QUOTED_STR, 0) && strstr(t, "{BODY}")) used++;
    }
    /* magick builders consume slots without a {TYPE:i} placeholder */
    if (ctx->tool == TOOL_MAGICK) {
        const char *t = tpl->flag_template;
        if ((strstr(t, "{RESIZE}") || strstr(t, "{CROP}")) && nl_find_slot(ctx, SLOT_RES, 0)) used++;
        if (strstr(t, "{QUALITY}") && nl_find_slot(ctx, SLOT_NUM, 0)) used++;
        if (strstr(t, "{DELAY}") && nl_find_slot(ctx, SLOT_FPS, 0)) used++;
        if (strstr(t, "{INPUTS}")) used += nl_count_slots(ctx, SLOT_FILE);
        if (strstr(t, "{GLOB}")) {
            if (nl_find_slot(ctx, SLOT_FILE, 0)) used++;
            if (nl_find_slot(ctx, SLOT_EXT, 0)) { used++; memcpy(seen[nseen++], "EXT:0", 6); }
        }
        if (strstr(t, "{OUT_NAME")) {
            if (nl_find_slot(ctx, SLOT_DEST, 0)) used++;
            if (nl_find_slot(ctx, SLOT_EXT, 0)) used++;
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

/* ------------------------------------------------------------------ */
/* Ranking                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const CommandTemplate *tpl;
    int score;
    int used;
    size_t req;
    int strong;        /* matched intent tokens that aren't WEAK_WORDS */
    uint32_t tokens;
    size_t index;      /* position in NL_TEMPLATES */
} Candidate;

/* Is `a` a better pick than `b`? On full ties the template listed first wins. */
static bool better(const Candidate *a, const Candidate *b) {
    if (a->score != b->score) return a->score > b->score;
    if (a->used != b->used) return a->used > b->used;
    if (a->req != b->req) return a->req > b->req;
    if (a->strong != b->strong) return a->strong > b->strong;
    return a->index < b->index;
}

/* Keep the NL_MAX_ALTS + 1 best runnable candidates, best first. */
#define TOP_N (NL_MAX_ALTS + 1)

/* Does one of the tokens in `mask` match a keyword of `tpl` exactly or as an
 * inflection (not just a synonym or a shared prefix)? */
static bool strongly_matches(const Word *toks, uint32_t mask, const CommandTemplate *tpl) {
    for (size_t t = 0; mask >> t; t++) {
        if (!((mask >> t) & 1u)) continue;
        for (size_t k = 0; k < NL_MAX_KEYWORDS && tpl->keywords[k]; k++) {
            Word kw;
            word_init(&kw, tpl->keywords[k]);
            if (similarity(&toks[t], &kw) >= 8) return true;
        }
    }
    return false;
}
static void top_insert(Candidate top[TOP_N], size_t *n, const Candidate *c) {
    size_t pos = *n;
    while (pos > 0 && better(c, &top[pos - 1])) pos--;
    if (pos >= TOP_N) return;
    size_t last = *n < TOP_N ? *n : TOP_N - 1;
    for (size_t k = last; k > pos; k--) top[k] = top[k - 1];
    top[pos] = *c;
    if (*n < TOP_N) (*n)++;
}

void nl_match_only(const ParseContext *ctx, MatchResult *out, bool verbose, const char *only) {
    memset(out, 0, sizeof *out);
    size_t only_len = only ? strlen(only) : 0;
    const CommandTemplate *best_filter = NULL;  /* for "how many ... where ..." */
    int best_filter_score = 0;
    Candidate top[TOP_N];
    size_t ntop = 0;
    Candidate rejected = { 0 };                 /* best one the expander refuses */
    /* best blocked template matched by a real word, not just "this commit" */
    const CommandTemplate *meant = NULL;
    int meant_score = 0;
    uint8_t meant_nmissing = 0;
    SlotType meant_missing[NL_MAX_REQUIRED];

    Word toks[NL_MAX_TOKENS];
    tokens_init(ctx, toks);
    uint32_t weak = 0;   /* bit t: intent token t is filler */
    uint32_t vague = 0;  /* ... or too generic to cast doubt on a match */
    for (size_t t = 0; t < ctx->token_count; t++) {
        const char *w = ctx->intent_tokens[t];
        if (in_words(w, WEAK_WORDS)) weak |= 1u << t;
        if (in_words(w, GENERIC_WORDS) || (ctx->wants_count && in_words(w, COUNT_WORDS)))
            vague |= 1u << t;
    }
    vague |= weak;
    uint32_t nouns = vague;
    for (size_t t = 0; t < ctx->token_count; t++)
        if (in_words(ctx->intent_tokens[t], NOUN_MARKERS)) nouns |= 1u << t;
    /* every runnable candidate's matched tokens, for the confidence check */
    enum { MAX_RUNNABLE = 128 };
    const CommandTemplate *runnable[MAX_RUNNABLE];
    uint32_t runnable_tokens[MAX_RUNNABLE];
    size_t nrunnable = 0;

    if (verbose) fprintf(stderr, "candidates:\n");

    for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
        const CommandTemplate *tpl = &NL_TEMPLATES[i];
        if (tpl->tool != ctx->tool) continue;
        if (only && strncmp(tpl->id, only, only_len) != 0) continue;

        Candidate c = { tpl, 0, 0, 0, 0, 0, i };
        c.score = score_words(ctx, toks, tpl, &c.tokens);
        if (c.score <= 0) continue;

        if (!shape_ok(ctx, tpl)) {
            if (verbose)
                fprintf(stderr, "  %-24s score=%-3d skipped: input is an %s\n", tpl->id, c.score,
                        ctx->input_shape == '[' ? "array" : "object");
            if (!out->wrong_shape || c.score > out->wrong_shape_score) {
                out->wrong_shape = tpl;
                out->wrong_shape_score = c.score;
            }
            continue;
        }

        SlotType missing[NL_MAX_REQUIRED];
        uint8_t nmissing = check_slots(ctx, tpl, missing);
        c.used = slots_used(ctx, tpl);
        c.req = required_count(tpl);
        for (uint32_t v = c.tokens & ~weak; v; v &= v - 1) c.strong++;
        bool refused = nmissing == 0 && !nl_check(ctx, tpl);

        if (verbose) {
            fprintf(stderr, "  %-24s score=%-3d used=%d strong=%d %s", tpl->id, c.score, c.used,
                    c.strong, nmissing ? "blocked, missing:" : refused ? "refused" : "ok");
            for (uint8_t m = 0; m < nmissing; m++)
                fprintf(stderr, " %s", nl_slot_name(missing[m]));
            fputc('\n', stderr);
        }

        if (nmissing == 0) {
            if (refused) {
                if (!rejected.tpl || better(&c, &rejected)) rejected = c;
                continue;
            }
            top_insert(top, &ntop, &c);
            if (nrunnable < MAX_RUNNABLE) {
                runnable[nrunnable] = tpl;
                runnable_tokens[nrunnable++] = c.tokens;
            }
            if (strncmp(tpl->id, "jq_filter_", 10) == 0 && c.score > best_filter_score) {
                best_filter = tpl;
                best_filter_score = c.score;
            }
        } else {
            bool better_blocked = !out->blocked || c.score > out->blocked_score ||
                                  (c.score == out->blocked_score && nmissing < out->missing_count);
            if (c.tokens & ~nouns && (!meant || c.score > meant_score)) {
                meant = tpl;
                meant_score = c.score;
                meant_nmissing = nmissing;
                memcpy(meant_missing, missing, sizeof missing);
            }
            if (better_blocked) {
                out->blocked = tpl;
                out->blocked_score = c.score;
                out->missing_count = nmissing;
                memcpy(out->missing, missing, sizeof missing);
            }
        }
    }

    /* A refused template still wins when it ranks first, so its refusal is
     * reported. Exception: git refuses a template that can't take one of the
     * slots given ("what changed yesterday": git diff has no date filter);
     * then a runnable one that uses more of them is the better reading.
     * Other tools refuse for safety (a too-broad find -delete), and falling
     * through there could pick a different destructive command. */
    bool use_rejected = rejected.tpl && (ntop == 0 || better(&rejected, &top[0]));
    if (use_rejected && ntop > 0 && ctx->tool == TOOL_GIT && top[0].used > rejected.used) {
        if (verbose) fprintf(stderr, "%s can't take every detail given: using %s\n",
                             rejected.tpl->id, top[0].tpl->id);
        use_rejected = false;
    }
    /* magick: the same, but the template that takes every detail may share
     * no keyword with the prompt ("combine a.png b.png into anim.gif at
     * 10fps" is refused by magick_append, which has no frame rate, and
     * magick_gif matched no word). Every magick template writes a new file,
     * so falling through can't destroy anything. */
    if (use_rejected && !only && ctx->tool == TOOL_MAGICK) {
        Candidate fall = { 0 };
        for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
            const CommandTemplate *tpl = &NL_TEMPLATES[i];
            SlotType missing[NL_MAX_REQUIRED];
            if (tpl->tool != ctx->tool || tpl == rejected.tpl ||
                check_slots(ctx, tpl, missing) != 0 || !nl_check(ctx, tpl))
                continue;
            Candidate c = { tpl, 0, slots_used(ctx, tpl), required_count(tpl), 0, 0, i };
            if (c.used <= rejected.used) continue;
            c.score = score_words(ctx, toks, tpl, &c.tokens);
            if (!fall.tpl || c.used > fall.used || (c.used == fall.used && better(&c, &fall)))
                fall = c;
        }
        if (fall.tpl) {
            if (verbose) fprintf(stderr, "%s can't take every detail given: using %s\n",
                                 rejected.tpl->id, fall.tpl->id);
            use_rejected = false;
            top[0] = fall;
            ntop = 1;
        }
    }
    if (use_rejected) {
        out->best = rejected.tpl;
        out->best_score = rejected.score;
        out->best_tokens = rejected.tokens;
        ntop = 0;
    } else if (ntop > 0) {
        out->best = top[0].tpl;
        out->best_score = top[0].score;
        out->best_tokens = top[0].tokens;
    }

    /* grep: a pattern with no recognizable verb ("'TODO' in src/")
     * means a plain search; find: "c files in src" means a plain listing.
     * Either way: the tool's first template that can run. */
    /* tar: "src/ into backup.tar.gz" creates, a bare archive name lists;
     * but never fall back when a verb was recognized ("extract src/"). */
    /* curl: a bare URL means a plain GET.
     * magick: "hero.png 800x600" resizes, "a.png to webp" converts. */
    if (!only && !out->best &&
        (ctx->tool == TOOL_GREP || ctx->tool == TOOL_FIND ||
         ((ctx->tool == TOOL_TAR || ctx->tool == TOOL_CURL || ctx->tool == TOOL_MAGICK) &&
          !out->blocked))) {
        for (size_t i = 0; i < NL_TEMPLATE_COUNT; i++) {
            const CommandTemplate *tpl = &NL_TEMPLATES[i];
            SlotType missing[NL_MAX_REQUIRED];
            if (tpl->tool != ctx->tool || check_slots(ctx, tpl, missing) != 0) continue;
            if (ctx->tool == TOOL_MAGICK && strcmp(tpl->id, "magick_resize") != 0 &&
                strncmp(tpl->id, "magick_convert", 14) != 0)
                break;   /* only resize / convert are safe guesses */
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
        if (best_filter != out->best) {
            for (size_t k = 0; k < ntop; k++)
                if (top[k].tpl == best_filter) out->best_tokens = top[k].tokens;
            ntop = 0;   /* the count rule decided it, not the scores */
        }
        out->best = best_filter;
        out->best_score = best_filter_score;
        if (out->blocked && out->blocked_score > best_filter_score &&
            strncmp(out->blocked->id, "jq_filter_", 10) != 0)
            out->blocked = NULL;
    }

    /* "tag this commit", "label this commit": the winner (git_revert) only
     * matched "this commit" or generic words, while a template that lacks a
     * detail (git_tag, no name given) matched what the user asked for, and
     * scored as well. Ask for the detail rather than run the weak guess. */
    if (!only && out->best && meant && meant_score >= out->best_score &&
        !(out->best_tokens & ~nouns)) {
        if (verbose)
            fprintf(stderr, "%s only matched generic words: %s fits better but lacks a detail\n",
                    out->best->id, meant->id);
        out->blocked = meant;
        out->blocked_score = meant_score;
        out->missing_count = meant_nmissing;
        memcpy(out->missing, meant_missing, sizeof meant_missing);
        out->best = NULL;
        out->best_score = 0;
        out->best_tokens = 0;
        ntop = 0;
        nrunnable = 0;
    }

    /* Runners-up within NL_CLOSE_MARGIN points that use at least as many
     * of the user's slots and were clearly matched by a specific word the
     * winner ignored: the prompt points two ways and the pick was a guess.
     * Two templates matched by the same words, like "get" for curl_get and
     * curl_get_json, are a designed default: the first listed is the plain one. */
    uint32_t ignored = ~out->best_tokens & ~vague;
    for (size_t k = 1; k < ntop && out->alt_count < NL_MAX_ALTS; k++) {
        if (top[k].score + NL_CLOSE_MARGIN < top[0].score) break;
        if (top[k].used < top[0].used || !strongly_matches(toks, top[k].tokens & ignored, top[k].tpl))
            continue;
        out->alts[out->alt_count] = top[k].tpl;
        out->alt_scores[out->alt_count++] = top[k].score;
    }

    /* Low confidence: a single keyword won, and another runnable template
     * clearly matched a specific word the winner ignored. */
    if (out->best && out->best_score < NL_CONFIDENT_SCORE) {
        for (size_t k = 0; k < nrunnable && !out->low_confidence; k++)
            if (runnable[k] != out->best &&
                strongly_matches(toks, runnable_tokens[k] & ignored, runnable[k]))
                out->low_confidence = true;
    }

    if (verbose && (out->alt_count || out->low_confidence))
        fprintf(stderr, "confidence: %s%s\n", out->low_confidence ? "low" : "ok",
                out->alt_count ? ", close runner-up" : "");
}

void nl_match(const ParseContext *ctx, MatchResult *out, bool verbose) {
    nl_match_only(ctx, out, verbose, NULL);
}
