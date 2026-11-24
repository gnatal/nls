/*
 * expander_git.c — git: refusals and the {PATHS} {AUTHOR} {SPAN} {BASE} {COMMITS} {TARGET} builders.
 */
#include "expander_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool git_uses_span(const char *t) {
    return strstr(t, "{SPAN}") || strstr(t, "{BASE}") || strstr(t, "{COMMITS}") ||
           strstr(t, "{TARGET}");
}

/* Does a git template consume slot TYPE #idx, directly or via a builder? */
static bool git_template_uses(const char *t, SlotType type, int idx) {
    if (type == SLOT_FILE && strstr(t, "{PATHS")) return true;
    if (type == SLOT_AUTHOR && strstr(t, "{AUTHOR}")) return true;
    if (idx == 0 && (type == SLOT_NUM || type == SLOT_REV) && git_uses_span(t)) return true;
    if (idx == 0 && type == SLOT_EXCLUDE && strstr(t, "{SPAN}")) return true;
    return template_uses(t, type, idx);
}

/* Refuse templates that would silently drop or misread part of the request. */
static bool git_check(const ParseContext *ctx, const CommandTemplate *tpl,
                      char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    if (ctx->stray) {
        if (strpbrk(ctx->stray, "$`;|&<>(){}*\\\"'!"))
            set_err(err, err_size, "not sure what '%s' is; quote names and messages, "
                    "e.g. branch 'feature-x' or message 'fix bug'", ctx->stray);
        else
            set_err(err, err_size, "not sure what '%s' refers to; numbers are used in e.g. "
                    "'last 3 commits', '3 commits ago', 'last 7 days' or 'lines 10 to 20'", ctx->stray);
        return false;
    }
    int seen[SLOT_TYPE_COUNT] = { 0 };
    for (size_t i = 0; i < ctx->slot_count; i++) {
        SlotType type = ctx->slots[i].type;
        if (git_template_uses(t, type, seen[type]++)) continue;
        static const char *const what[SLOT_TYPE_COUNT] = {
            [SLOT_FILE] = "a file path", [SLOT_TIME] = "a date filter",
            [SLOT_NUM] = "a number of commits", [SLOT_QUOTED_STR] = "a message or search text",
            [SLOT_EXCLUDE] = "a branch to leave out", [SLOT_LINES] = "a line range",
            [SLOT_REV] = "a revision", [SLOT_BRANCH] = "a branch name",
            [SLOT_AUTHOR] = "an author filter",
        };
        const char *v = ctx->slots[i].value;
        if (err && err_size)
            snprintf(err, err_size, "%s ('%s') isn't supported for this command%s",
                     what[type] ? what[type] : "that", v,
                     seen[type] > 1 ? " (or not that many of them)" : "");
        return false;
    }
    bool has_rev = nl_count_slots(ctx, SLOT_REV) || nl_count_slots(ctx, SLOT_EXCLUDE);
    if (git_uses_span(t) && nl_count_slots(ctx, SLOT_NUM) && has_rev) {
        set_err(err, err_size, "give either a number of commits or a revision ('%s'), not both",
                nl_find_slot(ctx, SLOT_REV, 0) ? nl_find_slot(ctx, SLOT_REV, 0)
                                               : nl_find_slot(ctx, SLOT_EXCLUDE, 0));
        return false;
    }
    const char *num = nl_find_slot(ctx, SLOT_NUM, 0);
    if (num) {
        long v = 0;
        for (const char *p = num; *p && v < 100000; p++) v = v * 10 + (*p - '0');
        if (v < 1 || v > 9999) {
            set_err(err, err_size, "'%s' isn't a usable number of commits", num);
            return false;
        }
        if (strcmp(tpl->id, "git_squash") == 0 && v < 2) {
            set_err(err, err_size, "squashing needs at least 2 commits (got %s)", num);
            return false;
        }
    }
    if (strstr(t, "--follow") && nl_count_slots(ctx, SLOT_FILE) > 1) {
        set_err(err, err_size, "following renames works on one file at a time%s", "");
        return false;
    }
    if (!strstr(t, "{PATHS")) {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_FILE); k++)
            if (nl_find_slot(ctx, SLOT_FILE, k)[0] == '-') {
                set_err(err, err_size, "refusing file name '%s' (it would be read as an option); "
                        "write it as ./name", nl_find_slot(ctx, SLOT_FILE, k));
                return false;
            }
    }
    return true;
}

static bool git_put_ref(Out *o, const char *v, char *err, size_t err_size) {
    if (!git_ref_ok(v)) { set_err(err, err_size, "refusing unsafe value '%s'", v); return false; }
    out_puts(o, v);
    return true;
}

/* {PATHS} {PATHS|default} {AUTHOR} {SPAN} {BASE} {COMMITS}. Returns -1 if
 * `name` is not one of them, 0 on error, 1 when expanded. Like a {TYPE:i}
 * placeholder, a builder that emitted user input counts toward its << >>
 * group, and one that emitted nothing makes the group drop. */
static int git_placeholder(const ParseContext *ctx, const char *name, size_t len,
                           const char *rest, const char *close, Out *o,
                           int *placeholders, bool *all_user, char *err, size_t err_size) {
    char quoted[NL_MAX_CMD_LEN];
    char buf[64];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
    if (IS("PATHS") && (*rest == '}' || *rest == '|')) {
        int n = nl_count_slots(ctx, SLOT_FILE);
        if (n == 0) {
            *all_user = false;
            if (*rest == '|') { out_puts(o, " -- "); out_putn(o, rest + 1, (size_t)(close - rest - 1)); }
            return 1;
        }
        out_puts(o, " --");
        for (int k = 0; k < n; k++) {
            if (!nl_shell_quote(nl_find_slot(ctx, SLOT_FILE, k), quoted, sizeof quoted)) {
                o->overflow = true;
                return 1;
            }
            out_puts(o, " ");
            out_puts(o, quoted);
        }
        (*placeholders)++;
        return 1;
    }
    if (*rest != '}') return -1;
    if (IS("AUTHOR")) {
        int n = nl_count_slots(ctx, SLOT_AUTHOR);
        if (n == 0) *all_user = false;
        for (int k = 0; k < n; k++) {
            const char *a = nl_find_slot(ctx, SLOT_AUTHOR, k);
            if (strcmp(a, "me") == 0) out_puts(o, " --author=\"$(git config user.name)\"");
            else { out_puts(o, " --author="); put_single_quoted(o, a); }
            (*placeholders)++;
        }
        return 1;
    }
    const char *num = nl_find_slot(ctx, SLOT_NUM, 0);
    const char *rev = nl_find_slot(ctx, SLOT_REV, 0);
    if (num && !all_digits_str(num)) { set_err(err, err_size, "refusing unsafe value '%s'", num); return 0; }
    if (IS("SPAN")) {
        /* git diff: " HEAD~3 HEAD", " main feature", " main", or nothing */
        const char *excl = nl_find_slot(ctx, SLOT_EXCLUDE, 0);
        if (num || ctx->git_last) {
            snprintf(buf, sizeof buf, " HEAD~%s HEAD", num ? num : "1");
            out_puts(o, buf);
        } else if (excl) {
            out_puts(o, " ");
            if (!git_put_ref(o, excl, err, err_size)) return 0;
            out_puts(o, " ");
            if (!git_put_ref(o, rev ? rev : "HEAD", err, err_size)) return 0;
        } else if (rev) {
            out_puts(o, " ");
            if (!git_put_ref(o, rev, err, err_size)) return 0;
        } else {
            *all_user = false;
            return 1;
        }
        (*placeholders)++;
        return 1;
    }
    if (IS("BASE")) {
        /* git rebase -i: a base revision, or HEAD~N */
        if (rev) { if (!git_put_ref(o, rev, err, err_size)) return 0; }
        else if (num) { snprintf(buf, sizeof buf, "HEAD~%s", num); out_puts(o, buf); }
        else {
            set_err(err, err_size, "say how many commits (e.g. 'last 3 commits') or a base "
                    "(e.g. 'onto main')%s", "");
            return 0;
        }
        (*placeholders)++;
        return 1;
    }
    if (IS("TARGET")) {
        /* git reset: a revision, else HEAD~N (default HEAD~1) */
        if (rev) { if (!git_put_ref(o, rev, err, err_size)) return 0; }
        else { snprintf(buf, sizeof buf, "HEAD~%s", num ? num : "1"); out_puts(o, buf); }
        (*placeholders)++;
        return 1;
    }
    if (IS("COMMITS")) {
        /* git revert: a revision, "HEAD~N..HEAD" for the last N, else HEAD */
        if (rev) { if (!git_put_ref(o, rev, err, err_size)) return 0; }
        else if (num && strcmp(num, "1") != 0) { snprintf(buf, sizeof buf, "HEAD~%s..HEAD", num); out_puts(o, buf); }
        else out_puts(o, "HEAD");
        (*placeholders)++;
        return 1;
    }
#undef IS
    return -1;
}

static int git_ph(const ParseContext *ctx, const Placeholder *ph, Out *o,
                  char *err, size_t err_size) {
    return git_placeholder(ctx, ph->name, ph->len, ph->rest, ph->close, o,
                           ph->placeholders, ph->all_user, err, err_size);
}

const ToolExpander nl_expander_git = { git_check, NULL, git_ph };
