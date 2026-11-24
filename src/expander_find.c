/*
 * expander_find.c — find: refusals and the {KIND} {NAMES} {DEPTH} {PRUNE} {PRINT} builders.
 */
#include "expander_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool find_has_filter(const ParseContext *ctx) {
    return nl_count_slots(ctx, SLOT_EXT) || nl_count_slots(ctx, SLOT_QUOTED_STR) ||
           nl_count_slots(ctx, SLOT_SIZE) || nl_count_slots(ctx, SLOT_TIME) ||
           nl_count_slots(ctx, SLOT_LINES);
}

/* Refuse templates that would silently drop part of the request. */
static bool find_check(const ParseContext *ctx, const CommandTemplate *tpl,
                       char *err, size_t err_size) {
    const char *t = tpl->flag_template;
    if (ctx->stray) {
        set_err(err, err_size, "not sure what '%s' refers to; try e.g. 'first 5', "
                "'larger than 5MB', 'older than 5 days' or 'less than 5 lines'", ctx->stray);
        return false;
    }
    int seen[SLOT_TYPE_COUNT] = { 0 };
    for (size_t i = 0; i < ctx->slot_count; i++) {
        SlotType type = ctx->slots[i].type;
        if (template_uses(t, type, seen[type]++)) continue;
        if (type == SLOT_EXCLUDE && strstr(t, "-delete")) {
            set_err(err, err_size, "skipping directories can't be combined with deleting "
                    "(find's -delete turns off -prune); narrow the search path instead%s", "");
            return false;
        }
        static const char *const what[SLOT_TYPE_COUNT] = {
            [SLOT_FILE] = "searching that many directories", [SLOT_EXT] = "a name filter",
            [SLOT_QUOTED_STR] = "a name filter", [SLOT_TIME] = "an age filter",
            [SLOT_NUM] = "a count", [SLOT_SIZE] = "a size filter",
            [SLOT_EXCLUDE] = "excluding directories", [SLOT_LINES] = "a line-count filter",
        };
        set_err(err, err_size, "%s isn't supported for this command",
                what[type] ? what[type] : "that");
        return false;
    }
    if (ctx->find_kind_set && !strstr(t, "{KIND}")) {
        const char *ty = strstr(t, "-type ");
        char want = ty ? ty[6] : 0;
        if (ctx->find_kind != want) {
            set_err(err, err_size, "this command only works on %s",
                    want == 'd' ? "directories" : "regular files");
            return false;
        }
    }
    if (nl_count_slots(ctx, SLOT_LINES)) {
        const char *ty = strstr(t, "-type ");
        char type = strstr(t, "{KIND}") ? (ctx->find_kind ? ctx->find_kind : 'f') : ty ? ty[6] : 0;
        if (type != 'f') {
            set_err(err, err_size, "line counts only apply to regular files%s", "");
            return false;
        }
    }
    if (strchr(ctx->flags, 'i') &&
        (!strstr(t, "{NAMES}") || !(nl_count_slots(ctx, SLOT_EXT) || nl_count_slots(ctx, SLOT_QUOTED_STR)))) {
        set_err(err, err_size, "ignoring case only applies to a name pattern (e.g. named 'readme*')%s", "");
        return false;
    }
    if (ctx->find_depth[0] && !strstr(t, "{DEPTH}")) {
        set_err(err, err_size, "a depth limit isn't supported for this command%s", "");
        return false;
    }
    if ((strstr(t, "-delete") || strstr(t, "rm -")) && !strstr(t, "-empty") && !find_has_filter(ctx)) {
        set_err(err, err_size, "refusing to delete everything; say which files "
                "(e.g. '*.log', named 'core', older than 30 days)%s", "");
        return false;
    }
    return true;
}

/* {KIND} {NAMES} {DEPTH} {PRUNE} {PRINT}. Returns -1 if `name` is not one of
 * them, 0 on error, 1 when expanded. */
static int find_placeholder(const ParseContext *ctx, const char *name, size_t len, Out *o,
                            char *err, size_t err_size) {
    char quoted[NL_MAX_CMD_LEN];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
    if (IS("KIND")) {
        char k = ctx->find_kind ? ctx->find_kind : 'f';
        if (k != 'a') { out_puts(o, " -type "); out_putn(o, &k, 1); }
        return 1;
    }
    if (IS("DEPTH")) {
        if (ctx->find_depth[0]) { out_puts(o, " -maxdepth "); out_puts(o, ctx->find_depth); }
        return 1;
    }
    if (IS("PRINT")) {
        /* pruning, or the awk line-count test (-exec is an action), turn
         * off find's implicit -print */
        if (nl_count_slots(ctx, SLOT_EXCLUDE) || nl_count_slots(ctx, SLOT_LINES))
            out_puts(o, " -print");
        return 1;
    }
    if (IS("PRUNE")) {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_EXCLUDE); k++) {
            const char *v = nl_find_slot(ctx, SLOT_EXCLUDE, k);
            char pat[NL_MAX_INPUT + 4];
            snprintf(pat, sizeof pat, "*/%s", v);
            if (!nl_shell_quote(pat, quoted, sizeof quoted)) { o->overflow = true; return 1; }
            out_puts(o, " -path ");
            out_puts(o, quoted);
            out_puts(o, " -prune -o");
        }
        return 1;
    }
    if (IS("NAMES")) {
        const char *test = strchr(ctx->flags, 'i') ? "-iname " : "-name ";
        int next = nl_count_slots(ctx, SLOT_EXT), nstr = nl_count_slots(ctx, SLOT_QUOTED_STR);
        if (next + nstr == 0) return 1;
        bool group = next + nstr > 1;
        if (group) out_puts(o, " \\(");
        for (int k = 0; k < next + nstr; k++) {
            out_puts(o, k ? " -o " : " ");
            out_puts(o, test);
            if (k < next) {
                const char *e = nl_find_slot(ctx, SLOT_EXT, k);
                for (const char *p = e; *p; p++)
                    if (!isalnum((unsigned char)*p)) {
                        set_err(err, err_size, "refusing unsafe value '%s'", e);
                        return 0;
                    }
                out_puts(o, "'*.");
                out_puts(o, e);
                out_puts(o, "'");
            } else {
                if (!nl_shell_quote(nl_find_slot(ctx, SLOT_QUOTED_STR, k - next), quoted, sizeof quoted)) {
                    o->overflow = true;
                    return 1;
                }
                out_puts(o, quoted);
            }
        }
        if (group) out_puts(o, " \\)");
        return 1;
    }
#undef IS
    return -1;
}

static int find_ph(const ParseContext *ctx, const Placeholder *ph, Out *o,
                   char *err, size_t err_size) {
    if (*ph->rest != '}') return -1;
    return find_placeholder(ctx, ph->name, ph->len, o, err, err_size);
}

const ToolExpander nl_expander_find = { find_check, NULL, find_ph };
