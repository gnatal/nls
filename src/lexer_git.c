/*
 * lexer_git.c — git: history, branches, undo, diffs and cleanup.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t branch_end;  /* token index just past the last BRANCH ("delete a and b")  */
    bool to_rev;        /* "to X" names a revision (compare / restore), not a branch */
    bool renaming;      /* "rename X to Y": Y may be any valid name                 */
    bool ref_noun;      /* "branch" / "tag" appears: "called X" names a ref         */
    bool deleting;      /* a delete verb appears: "merged" means "delete merged"    */
    bool tagging;       /* "tag" / "tags" appears: a version like v1.0 is its name   */
    bool committing;    /* starts with "commit": "staged" means only what's staged  */
} GitState;

static const char *const GIT_REMOTES[] = {
    "origin", "upstream", "remote", "remotes", "github", "gitlab", "bitbucket", "server", NULL
};

static const char *const GIT_COMMIT_NOUNS[] = { "commit", "commits", NULL };

/* "last commit", "most recent commit": the commit at HEAD */
static const char *const GIT_LAST_WORDS[] = {
    "last", "latest", "previous", "recent", "newest", "prior", NULL
};

/* a bare number after one of these is a count: "last 3", "top 5" */
static const char *const GIT_COUNT_WORDS[] = {
    "last", "latest", "previous", "recent", "past", "first", "top", "limit", "max",
    "maximum", "-n", NULL
};

static const char *const GIT_UNTIL_WORDS[] = {
    "before", "until", "till", "older", "earlier", NULL
};

static const char *const GIT_SINCE_WORDS[] = {
    "since", "after", "within", "last", "past", "newer", "in", "from", NULL
};

/* "branch X", "tag X": X is always a ref name */
static const char *const GIT_REF_LEADERS[] = { "branch", "branches", "tag", "tags", NULL };

/* "switch to X", "merge X": X is a branch if it isn't a request word */
static const char *const GIT_BRANCH_LEADERS[] = {
    "to", "switch", "checkout", "merge", "rename", "push", "publish", NULL
};

/* "from X", "onto X", "against X": X is the revision to work from */
static const char *const GIT_REV_LEADERS[] = {
    "from", "onto", "on", "into", "against", "vs", "versus", "at", "diff", "compare",
    "comparing", NULL
};

static const char *const GIT_FILE_LEADERS[] = {
    "in", "inside", "under", "within", "folder", "directory", "dir", NULL
};

/* verbs after which "to X" is a revision: "compare to main", "restore x to v1.0" */
static const char *const GIT_TO_REV_VERBS[] = {
    "compare", "comparing", "compared", "diff", "difference", "differences", "vs", "versus",
    "restore", "revert", "reset", "rollback", "roll", NULL
};

static const char *const GIT_FILLER[] = {
    "git", "repo", "repository", "currently", "now", "ago", "since", "after", "before",
    "until", "till", "than", "older", "newer", "i", "i've", "ive", "we", "our", "were",
    "was", "is", "are", "be", "by", "my", NULL
};

/* Words that are never branch, author or file names */
static const char *const GIT_RESERVED[] = {
    "last", "latest", "previous", "recent", "newest", "prior", "current", "all",
    "everything", "changes", "change", "staged", "unstaged", "untracked", "ignored",
    "yesterday", "today", "me", "myself", "it", "them", "here", "there", "head", "date",
    "time", "name", "most", "not", "and", "or", "one", "each", "every", "work", "stuff",
    "files", "file", "everyone", "anyone", "someone", "who", "what", "which", "when",
    "where", "why", "how", "that", "this", "these", "those", "local", "new", "old", "merged",
    "unmerged", "progress", "exist", "exists", "existing", "available", NULL
};

/* Characters git accepts in a branch / tag name, minus anything a shell
 * would treat specially. Also rejects "..", "//", "x.lock", "-x", ... */
static bool git_ref_word(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || n > 100 || strchr("-./", t[0]) || strchr("/.", t[n - 1])) return false;
    if (strstr(t, "..") || strstr(t, "//")) return false;
    if (n >= 5 && strcmp(t + n - 5, ".lock") == 0) return false;
    for (const char *p = t; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("._/-", *p))) return false;
    return true;
}

/* HEAD, HEAD~3, HEAD^, ORIG_HEAD, or an abbreviated / full commit hash. */
static bool git_rev_literal(const char *t) {
    size_t n = strlen(t);
    const char *rest = NULL;
    if (strncmp(t, "HEAD", 4) == 0) rest = t + 4;
    else if (strncmp(t, "ORIG_HEAD", 9) == 0) rest = t + 9;
    if (rest) {
        if (*rest == '\0') return true;
        if (*rest == '~') return rest[1] == '\0' || all_digits(rest + 1, strlen(rest + 1));
        while (*rest == '^') rest++;
        return *rest == '\0';
    }
    if (n < 7 || n > 40) return false;
    bool digit = false, letter = false;
    for (const char *p = t; *p; p++) {
        if (isdigit((unsigned char)*p)) digit = true;
        else if (*p >= 'a' && *p <= 'f') letter = true;
        else return false;
    }
    return digit && letter;
}

/* A release version: v1, v1.0, 2.3.1, v2.0.0-rc.1 (a leading "v" or a dot). */
static bool git_version_word(const char *t) {
    const char *p = t;
    bool v = *p == 'v' || *p == 'V';
    if (v) p++;
    if (!isdigit((unsigned char)*p)) return false;
    bool dot = false;
    while (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        if (*p == '.') dot = true;
        p++;
    }
    if (*p == '-' && isalnum((unsigned char)p[1]))
        for (p++; isalnum((unsigned char)*p) || *p == '.' || *p == '-'; p++) {}
    return *p == '\0' && (v || dot) && git_ref_word(t);
}

static bool git_name_basic(const char *t, const char *lw) {
    return git_ref_word(t) && !is_number(lw, false) && !in_list(lw, STOPWORDS) &&
           !in_list(lw, GIT_FILLER) && !in_list(lw, GIT_REMOTES) &&
           !in_list(lw, GIT_COMMIT_NOUNS) && !in_list(lw, GIT_RESERVED) &&
           !in_list(lw, GIT_REF_LEADERS) && !in_list(lw, GIT_BRANCH_LEADERS) &&
           !in_list(lw, GIT_REV_LEADERS) && !in_list(lw, GIT_FILE_LEADERS) &&
           strcmp(lw, "named") != 0 && strcmp(lw, "called") != 0 &&
           find_time_unit(lw, true) == 0;
}

/* A bare name that isn't also a word of some git request */
static bool git_name_ok(const char *t, const char *lw) {
    return git_name_basic(t, lw) && !is_keyword(TOOL_GIT, lw);
}

/* Unmistakably a path: a known extension, ./x, ~/x, /x or x/ */
static bool git_file_like(const char *t) {
    size_t n = strlen(t);
    const char *ext = extension_of(t);
    return (ext && in_list(ext, GREP_EXTS)) || strncmp(t, "./", 2) == 0 ||
           strncmp(t, "~/", 2) == 0 || t[0] == '/' || (n > 1 && t[n - 1] == '/');
}

/* Author names and e-mails: letters, digits and ._@+- */
static bool git_author_word(const char *t) {
    if (!t[0] || t[0] == '-' || strlen(t) > 100) return false;
    for (const char *p = t; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("._@+-", *p))) return false;
    return true;
}

static bool is_iso_date(const char *t) {
    return strlen(t) == 10 && all_digits(t, 4) && t[4] == '-' && all_digits(t + 5, 2) &&
           t[7] == '-' && all_digits(t + 8, 2);
}

/* "--since='7 days ago'" / "--until='2024-01-31'" */
static void git_add_time(ParseContext *ctx, bool until, const char *when) {
    char buf[64];
    snprintf(buf, sizeof buf, "--%s='%s'", until ? "until" : "since", when);
    add_slot(ctx, SLOT_TIME, buf);
}

static void git_add_age(ParseContext *ctx, bool until, long long n, double minutes) {
    const char *unit = minutes == 1 ? "minute" : minutes == 60 ? "hour" :
                       minutes == 1440 ? "day" : minutes == 7 * 1440 ? "week" :
                       minutes == 30 * 1440 ? "month" : "year";
    char when[48];
    snprintf(when, sizeof when, "%lld %s%s ago", n, unit, n == 1 ? "" : "s");
    git_add_time(ctx, until, when);
}

/* Is the date before toks[i] an upper bound ("before", "older than")? The
 * nearest direction word wins; default is a lower bound ("last 7 days"). */
static bool git_until(const RawTok *toks, size_t i) {
    size_t lo = i > 4 ? i - 4 : 0;
    for (size_t k = i; k > lo; k--) {
        const RawTok *w = &toks[k - 1];
        if (w->quoted) break;
        if (in_list(w->lower, GIT_UNTIL_WORDS)) return true;
        if (in_list(w->lower, GIT_SINCE_WORDS)) return false;
    }
    return false;
}

static void git_add_branch(ParseContext *ctx, GitState *st, const char *t, size_t end) {
    add_slot(ctx, SLOT_BRANCH, t);
    st->branch_end = end;
}

/* The word before toks[i], skipping articles and "my", "most", "very". */
static const char *git_prev_word(const RawTok *toks, size_t i, size_t *at) {
    static const char *const skip[] = { "the", "a", "an", "my", "most", "very", "our", NULL };
    while (i > 0) {
        i--;
        if (toks[i].quoted) return NULL;
        if (!in_list(toks[i].lower, skip)) { if (at) *at = i; return toks[i].lower; }
    }
    return NULL;
}

/* Numbers: commit counts, "3 commits ago", ages, otherwise refused. */
static size_t git_number(ParseContext *ctx, const RawTok *toks, size_t count, size_t i) {
    const char *lw = toks[i].lower;
    size_t n = number_prefix(lw, false);
    if (n == 0 || n >= 24) return 0;
    char num[24];
    memcpy(num, lw, n);
    num[n] = '\0';
    const char *suffix = lw + n;
    bool whole = all_digits(num, n) && n <= 5;

    const char *unit = suffix;
    size_t used = 1;
    if (*suffix == '\0') {
        if (i + 1 < count && !toks[i + 1].quoted) { unit = toks[i + 1].lower; used = 2; }
        else unit = NULL;
    }
    const char *after = i + used < count && !toks[i + used].quoted ? toks[i + used].lower : NULL;

    if (unit) {
        double minutes = find_time_unit(unit, true);
        if (minutes > 0) {
            if (!whole) { if (!ctx->stray) ctx->stray = arena_dup(ctx, toks[i].text); return used; }
            git_add_age(ctx, git_until(toks, i), atoll(num), minutes);
            return used + (after && strcmp(after, "ago") == 0 ? 1 : 0);
        }
        if (in_list(unit, GIT_COMMIT_NOUNS) && whole) {
            if (after && (strcmp(after, "ago") == 0 || strcmp(after, "back") == 0)) {
                char rev[32];
                snprintf(rev, sizeof rev, "HEAD~%s", num);
                add_slot(ctx, SLOT_REV, rev);
                return used + 1;
            }
            add_slot(ctx, SLOT_NUM, num);
            return used;
        }
    }
    if (*suffix) return 0;

    const char *pw = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;
    const char *nw = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    static const char *const after_w[] = { "most", "latest", "recent", "newest", "last", NULL };
    if (whole && ((pw && in_list(pw, GIT_COUNT_WORDS)) || (nw && in_list(nw, after_w)))) {
        add_slot(ctx, SLOT_NUM, num);
        return 1;
    }
    if (!ctx->stray) ctx->stray = arena_dup(ctx, toks[i].text);
    return 1;
}

/* "lines 10 to 20", "line 42", "lines 10-20" -> LINES "10,20". Returns
 * tokens consumed, or 0. */
static size_t git_lines(ParseContext *ctx, const RawTok *toks, size_t count, size_t i) {
    const char *lw = toks[i].lower;
    if (!(strcmp(lw, "line") == 0 || strcmp(lw, "lines") == 0 || strcmp(lw, "-l") == 0) ||
        i + 1 >= count || toks[i + 1].quoted)
        return 0;
    const char *a = toks[i + 1].lower;
    char buf[32];
    const char *dash = strchr(a, '-');
    if (!dash) dash = strchr(a, ',');
    if (dash && all_digits(a, (size_t)(dash - a)) && dash - a <= 6 &&
        all_digits(dash + 1, strlen(dash + 1)) && strlen(dash + 1) <= 6) {
        snprintf(buf, sizeof buf, "%.*s,%s", (int)(dash - a), a, dash + 1);
        add_slot(ctx, SLOT_LINES, buf);
        return 2;
    }
    if (!all_digits(a, strlen(a)) || strlen(a) > 6) return 0;
    static const char *const seps[] = { "to", "through", "thru", "-", "and", "until", NULL };
    if (i + 3 < count && in_list(toks[i + 2].lower, seps) &&
        all_digits(toks[i + 3].lower, strlen(toks[i + 3].lower)) && strlen(toks[i + 3].lower) <= 6) {
        snprintf(buf, sizeof buf, "%s,%s", a, toks[i + 3].lower);
        add_slot(ctx, SLOT_LINES, buf);
        return 4;
    }
    snprintf(buf, sizeof buf, "%s,%s", a, a);
    add_slot(ctx, SLOT_LINES, buf);
    return 2;
}

/* Classify toks[i] in git mode. Returns the number of tokens consumed
 * (>= 1), or 0 if the token should fall through to intent handling. */
static size_t classify_git(ParseContext *ctx, const RawTok *toks, size_t count, size_t i,
                           GitState *st) {
    const char *t = toks[i].text;
    const char *lw = toks[i].lower;
    const char *next = i + 1 < count && !toks[i + 1].quoted ? toks[i + 1].lower : NULL;
    const char *prev = i > 0 && !toks[i - 1].quoted ? toks[i - 1].lower : NULL;

    /* 1. Quoted text: a ref after "branch"/"tag", a path after "file",
     *    otherwise a message or search string */
    if (toks[i].quoted) {
        if (prev && (in_list(prev, GIT_REF_LEADERS) ||
                     (st->ref_noun && (strcmp(prev, "named") == 0 || strcmp(prev, "called") == 0)))
            && git_ref_word(t)) {
            git_add_branch(ctx, st, t, i + 1);
            return 1;
        }
        static const char *const text_words[] = {
            "added", "add", "adding", "removed", "remove", "removing", "introduced", "introduce",
            "deleted", "delete", "containing", "contains", "contain", "mentioning", "mention",
            "mentions", "message", "msg", "string", "text", "code", "saying", "says", "titled",
            "subject", "grep", "search", "for", "find", "about", "-m", "-s", "named", "called",
            "commit", "as", NULL
        };
        bool pathy = strcmp(t, ".") == 0 || strchr(t, '/') || git_file_like(t) ||
                     looks_like_file(t, TOOL_GREP);
        if ((prev && (strcmp(prev, "file") == 0 || strcmp(prev, "path") == 0)) ||
            (pathy && !(prev && in_list(prev, text_words)))) {
            add_slot(ctx, SLOT_FILE, t);
            return 1;
        }
        add_slot(ctx, SLOT_QUOTED_STR, t);
        return 1;
    }

    /* 2. Unquoted shell syntax ("$(whoami)", "a;b") is never a name: refuse */
    if (strpbrk(t, "$`;|&<>(){}\\\"")) {
        if (!ctx->stray) ctx->stray = arena_dup(ctx, t);
        return 1;
    }

    /* 2b. Fixed phrases: "undo the reset", "undo git add" */
    if (strcmp(lw, "undo") == 0) {
        static const char *const skip[] = {
            "the", "my", "that", "this", "last", "previous", "a", "an", "git", "accidental",
            "bad", "wrong", "latest", "recent", NULL
        };
        size_t k = i + 1;
        while (k < count && !toks[k].quoted && in_list(toks[k].lower, skip)) k++;
        if (k < count && !toks[k].quoted) {
            const char *w = toks[k].lower;
            const char *tok = strcmp(w, "reset") == 0 ? "@undoreset" :
                              (strcmp(w, "rebase") == 0 || strcmp(w, "merge") == 0) ? "@undorebase" :
                              strcmp(w, "amend") == 0 ? "@undoamend" :
                              (strcmp(w, "add") == 0 || strcmp(w, "stage") == 0) ? "unstage" : NULL;
            if (tok) { add_intent(ctx, tok); return k + 1 - i; }
        }
    }

    /* 3. "not in main", "missing from main", "since v1.0", "between v1 and v2" */
    static const char *const neg[] = {
        "not", "isn't", "isnt", "aren't", "arent", "missing", "absent", "excluding", NULL
    };
    static const char *const neg_in[] = { "in", "on", "from", "into", NULL };
    if (in_list(lw, neg) && next && in_list(next, neg_in) && i + 2 < count && !toks[i + 2].quoted &&
        (git_name_basic(toks[i + 2].text, toks[i + 2].lower) || git_rev_literal(toks[i + 2].text))) {
        add_slot(ctx, SLOT_EXCLUDE, toks[i + 2].text);
        add_intent(ctx, "@notin");
        return 3;
    }
    /* "haven't I pushed", "not pulled yet": the negation goes with the verb */
    static const char *const neg_verb[] = {
        "not", "haven't", "havent", "hasn't", "hasnt", "didn't", "didnt", "never", NULL
    };
    if (in_list(lw, neg_verb)) {
        static const char *const pushed[] = { "push", "pushed", "pushing", NULL };
        static const char *const pulled[] = {
            "pull", "pulled", "pulling", "fetch", "fetched", "fetching", NULL
        };
        for (size_t k = i + 1; k < count && k <= i + 3 && !toks[k].quoted; k++) {
            if (in_list(toks[k].lower, pushed)) { add_intent(ctx, "unpushed"); return 1; }
            if (in_list(toks[k].lower, pulled)) { add_intent(ctx, "unpulled"); return 1; }
        }
    }
    if (strcmp(lw, "since") == 0 && next &&
        (git_name_ok(toks[i + 1].text, next) || git_rev_literal(toks[i + 1].text)) &&
        !is_iso_date(next)) {
        add_slot(ctx, SLOT_EXCLUDE, toks[i + 1].text);
        add_intent(ctx, "@notin");
        return 2;
    }
    if (strcmp(lw, "between") == 0 && i + 3 < count && !toks[i + 1].quoted && !toks[i + 3].quoted &&
        strcmp(toks[i + 2].lower, "and") == 0) {
        const char *a = toks[i + 1].text, *b = toks[i + 3].text;
        bool a_ok = git_name_basic(a, toks[i + 1].lower) || git_rev_literal(a);
        bool b_ok = git_name_basic(b, toks[i + 3].lower) || git_rev_literal(b);
        if (a_ok && b_ok) {
            add_slot(ctx, SLOT_EXCLUDE, a);
            add_slot(ctx, SLOT_REV, b);
            add_intent(ctx, "between");
            return 4;
        }
    }

    /* 4. Authors: "by alice", "authored by 'Jane Doe'", "by me", "my commits" */
    if (strcmp(lw, "by") == 0 || strcmp(lw, "author") == 0 || strcmp(lw, "authored") == 0) {
        size_t k = i + 1;
        if (strcmp(lw, "authored") == 0 && k < count && strcmp(toks[k].lower, "by") == 0) k++;
        if (k < count) {
            const char *a = toks[k].text, *alw = toks[k].lower;
            if (toks[k].quoted && a[0]) { add_slot(ctx, SLOT_AUTHOR, a); return k + 1 - i; }
            if (!toks[k].quoted && (strcmp(alw, "me") == 0 || strcmp(alw, "myself") == 0)) {
                add_slot(ctx, SLOT_AUTHOR, "me");
                return k + 1 - i;
            }
            static const char *const not_names[] = {
                "date", "time", "age", "name", "size", "recency", "most", "last", "latest",
                "newest", "oldest", "recent", "commit", "commits", NULL
            };
            if (!toks[k].quoted && git_author_word(a) && !in_list(alw, not_names) &&
                !in_list(alw, STOPWORDS) && !in_list(alw, GIT_RESERVED) && !is_number(alw, false) &&
                !is_keyword(TOOL_GIT, alw) && (strchr(a, '@') || !looks_like_file(a, TOOL_GREP))) {
                add_slot(ctx, SLOT_AUTHOR, a);
                return k + 1 - i;
            }
        }
        if (strcmp(lw, "by") == 0) return 1;
    }
    if ((strcmp(lw, "my") == 0 && next && in_list(next, GIT_COMMIT_NOUNS)) || strcmp(lw, "mine") == 0) {
        add_slot(ctx, SLOT_AUTHOR, "me");
        return 1;
    }

    /* 5. Revisions written out: HEAD~2, a1b2c3d */
    if (git_rev_literal(t)) { add_slot(ctx, SLOT_REV, t); return 1; }

    /* 5b. "tag this commit v1.0": with a tag around, a version names it
     *     (unless it's where to work from: "diff against v1.0") */
    if (st->tagging && git_version_word(t)) {
        const char *pw = git_prev_word(toks, i, NULL);
        if (!(pw && in_list(pw, GIT_REV_LEADERS))) {
            git_add_branch(ctx, st, t, i + 1);
            return 1;
        }
    }

    /* 6. Line ranges, dates, numbers */
    size_t n = git_lines(ctx, toks, count, i);
    if (n) return n;
    if (is_iso_date(t)) { git_add_time(ctx, git_until(toks, i), t); return 1; }
    n = git_number(ctx, toks, count, i);
    if (n) return n;

    /* 7. Ages without a number: "yesterday", "today", "last week" */
    if (strcmp(lw, "yesterday") == 0) { git_add_time(ctx, git_until(toks, i), "yesterday"); return 1; }
    if (strcmp(lw, "today") == 0) { git_add_time(ctx, git_until(toks, i), "midnight"); return 1; }
    static const char *const one_unit[] = { "last", "past", "this", "a", "an", "one", NULL };
    double minutes = find_time_unit(lw, false);
    if (minutes > 0 && i > 0 && !toks[i - 1].quoted && in_list(toks[i - 1].lower, one_unit)) {
        git_add_age(ctx, git_until(toks, i), 1, minutes);
        return next && strcmp(next, "ago") == 0 ? 2 : 1;
    }

    /* 8. Names after a leading word: "branch X", "switch to X", "from X",
     *    "in X" */
    size_t at = 0;
    const char *pw = git_prev_word(toks, i, &at);
    bool basic = git_name_basic(t, lw);
    bool ok = basic && !is_keyword(TOOL_GIT, lw);
    bool filey = git_file_like(t);
    if (pw && basic) {
        bool plural = strcmp(pw, "branches") == 0 || strcmp(pw, "tags") == 0;
        if (!filey && ((in_list(pw, GIT_REF_LEADERS) && (!plural || ok)) ||
                       (st->ref_noun && (strcmp(pw, "named") == 0 || strcmp(pw, "called") == 0 ||
                                         strcmp(pw, "as") == 0)) ||
                       (strcmp(pw, "to") == 0 && st->renaming) ||
                       ((strcmp(pw, "and") == 0 || strcmp(pw, "or") == 0) && st->branch_end == at && ok))) {
            git_add_branch(ctx, st, t, i + 1);
            return 1;
        }
        if (strcmp(pw, "file") == 0 || strcmp(pw, "path") == 0) {
            add_slot(ctx, SLOT_FILE, t);
            return 1;
        }
        /* "stash called wip": with no branch/tag around, a name is a message */
        if (!st->ref_noun && (strcmp(pw, "named") == 0 || strcmp(pw, "called") == 0)) {
            add_slot(ctx, SLOT_QUOTED_STR, t);
            return 1;
        }
    }
    if (pw && ok && !filey) {
        bool as_of = strcmp(pw, "of") == 0 && at > 0 && strcmp(toks[at - 1].lower, "as") == 0;
        if (in_list(pw, GIT_REV_LEADERS) || as_of || (strcmp(pw, "to") == 0 && st->to_rev)) {
            add_slot(ctx, SLOT_REV, t);
            return 1;
        }
        if (in_list(pw, GIT_BRANCH_LEADERS)) {
            git_add_branch(ctx, st, t, i + 1);
            return 1;
        }
    }
    if (pw && ok && in_list(pw, GIT_FILE_LEADERS)) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }
    /* "the feature-x branch" */
    if (ok && !filey && next && (strcmp(next, "branch") == 0 || strcmp(next, "tag") == 0)) {
        git_add_branch(ctx, st, t, i + 1);
        return 1;
    }

    /* 9. Paths */
    if (strcmp(t, ".") == 0 || strcmp(t, "..") == 0 || strchr(t, '/') ||
        looks_like_file(t, TOOL_GREP) || filey ||
        (lw[0] == '.' && isalnum((unsigned char)lw[1]))) {
        add_slot(ctx, SLOT_FILE, t);
        return 1;
    }

    /* 10. Nouns. "commit" is a verb only at the start or after "and"/"then";
     *     "last commit" means HEAD; "commits" means history. */
    if (in_list(lw, GIT_COMMIT_NOUNS)) {
        static const char *const verb_after[] = { "and", "then", "&&", "please", "to", "just", "now", NULL };
        bool singular = strcmp(lw, "commit") == 0;
        if (singular && (i == 0 || (prev && in_list(prev, verb_after) &&
                                    !(i > 1 && in_list(toks[i - 2].lower, GIT_LAST_WORDS))))) {
            add_intent(ctx, "commit");
            return 1;
        }
        size_t lat = 0;
        const char *lp = git_prev_word(toks, i, &lat);
        bool after_last = lp && in_list(lp, GIT_LAST_WORDS);
        if (singular && after_last) {
            ctx->git_last = true;
            add_intent(ctx, "@last");
            size_t tat = 0;
            const char *tp = git_prev_word(toks, lat, &tat);
            if (tp && (strcmp(tp, "to") == 0 || strcmp(tp, "into") == 0)) add_intent(ctx, "@tolast");
        } else if (singular) {
            add_intent(ctx, "@commit");
        } else if (!(lp && (after_last || is_number(lp, false)))) {
            add_intent(ctx, "history");
        }
        return 1;
    }
    if (strcmp(lw, "staged") == 0 && st->committing) {
        /* "commit staged changes": just the index, not git add -A first.
         * Two tokens, so it outscores commit_all's "stage" + "changes". */
        add_intent(ctx, "@staged");
        add_intent(ctx, "@staged+");
        return 1;
    }
    if (strcmp(lw, "merged") == 0 && st->deleting) {
        /* "delete merged branches": two tokens, so it outscores "delete branch X" */
        add_intent(ctx, "@delmerged");
        add_intent(ctx, "@delmerged+");
        return 1;
    }
    if (strcmp(lw, "branches") == 0) { add_intent(ctx, "branch"); return 1; }
    if (strcmp(lw, "tags") == 0)     { add_intent(ctx, "tag"); return 1; }
    if (strcmp(lw, "stashes") == 0)  { add_intent(ctx, "stash"); return 1; }

    /* 11. Glue words carry no intent */
    if (in_list(lw, GIT_FILLER)) return 1;
    return 0;
}

void nl_lex_git(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    GitState gst = { 0, false, false, false, false, false, false };
    for (size_t i = 0; i < count; i++) {
        /* the request starts with "commit" ("please commit", "git commit") */
        static const char *const lead[] = { "please", "git", "now", "just", NULL };
        if (toks[i].quoted || !in_list(toks[i].lower, lead)) {
            gst.committing = !toks[i].quoted && strcmp(toks[i].lower, "commit") == 0;
            break;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (toks[i].quoted) continue;
        const char *w = toks[i].lower;
        if (strcmp(w, "tag") == 0 || strcmp(w, "tags") == 0) gst.tagging = true;
        if (in_list(w, GIT_TO_REV_VERBS)) gst.to_rev = true;
        if (strcmp(w, "rename") == 0 || strcmp(w, "renaming") == 0) gst.renaming = true;
        if (in_list(w, GIT_REF_LEADERS)) gst.ref_noun = true;
        static const char *const del[] = {
            "delete", "remove", "drop", "clean", "cleanup", "prune", "erase", NULL
        };
        if (in_list(w, del)) gst.deleting = true;
    }

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        size_t used = classify_git(ctx, toks, count, i, &gst);
        if (used) { i += used - 1; continue; }
        lex_intent(ctx, toks[i].lower);
    }
}
