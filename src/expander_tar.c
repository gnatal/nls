/*
 * expander_tar.c — tar: refusals, archive naming and the {TAR_FLAGS} {ARCHIVE} {EXCLUDES} {MEMBERS} builders.
 */
#include "expander_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *tar_comp_name(char c) {
    switch (c) {
    case 'z': return "gzip";
    case 'j': return "bzip2";
    case 'J': return "xz";
    case 'Z': return "zstd";
    default:  return "no compression";
    }
}

static const char *tar_comp_suffix(char c) {
    switch (c) {
    case 'z': return ".tar.gz";
    case 'j': return ".tar.bz2";
    case 'J': return ".tar.xz";
    case 'Z': return ".tar.zst";
    default:  return ".tar";
    }
}

/* The operation a tar template performs: 'c', 'x' or 't'. */
static char tar_mode(const CommandTemplate *tpl) {
    const char *m = strstr(tpl->flag_template, "{TAR_FLAGS|");
    return m ? m[11] : 0;
}

/* Compression to use: from the archive name, else the requested format,
 * else gzip when creating. 0 (with err set) if they disagree. */
static char tar_compression(const ParseContext *ctx, char *err, size_t err_size) {
    const char *archive = nl_find_slot(ctx, SLOT_ARCHIVE, 0);
    const char *ext = nl_find_slot(ctx, SLOT_EXT, 0);
    char a = archive ? nl_tar_compression(archive, true) : 0;
    char e = 0;
    if (ext) {
        char dotted[16];
        snprintf(dotted, sizeof dotted, ".%s", ext);
        e = nl_tar_compression(dotted, false);
    }
    if (a && e && a != e) {
        if (err && err_size)
            snprintf(err, err_size, "'%s' is a %s archive, but %s was asked for",
                     archive, tar_comp_name(a), tar_comp_name(e));
        return 0;
    }
    return a ? a : e ? e : 'z';
}

/* Does `name` end in a non-tar extension ("backup.zip")? */
static bool has_foreign_ext(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return false;
    bool alpha = false;
    for (const char *p = dot + 1; *p; p++) {
        if (!isalnum((unsigned char)*p)) return false;
        if (isalpha((unsigned char)*p)) alpha = true;
    }
    return alpha && strlen(dot + 1) <= 5;
}

/* Archive name for "compress src/" with no name given: src.tar.gz; with
 * "into backup" or "into out/": backup.tar.gz, out/src.tar.gz. */
static bool tar_derive_archive(const ParseContext *ctx, char comp, char *dst, size_t size,
                               char *err, size_t err_size) {
    const char *dest = nl_find_slot(ctx, SLOT_DEST, 0);
    const char *dir = "";
    char stem[NL_MAX_INPUT];
    size_t dl = dest ? strlen(dest) : 0;
    if (dest && dl > 0 && dest[dl - 1] != '/') {
        if (has_foreign_ext(dest)) {
            set_err(err, err_size, "'%s' isn't a tar archive name; use .tar, .tar.gz, .tgz, "
                    ".tar.bz2, .tar.xz or .tar.zst", dest);
            return false;
        }
        snprintf(stem, sizeof stem, "%s", dest);
    } else {
        if (dest) dir = dest;
        stem[0] = '\0';
        if (nl_count_slots(ctx, SLOT_FILE) == 1) {
            snprintf(stem, sizeof stem, "%s", nl_find_slot(ctx, SLOT_FILE, 0));
            size_t n = strlen(stem);
            while (n > 1 && stem[n - 1] == '/') stem[--n] = '\0';
            const char *slash = strrchr(stem, '/');
            if (slash) memmove(stem, slash + 1, strlen(slash + 1) + 1);
        }
        if (!stem[0] || strcmp(stem, ".") == 0 || strcmp(stem, "..") == 0 ||
            strcmp(stem, "~") == 0 || strcmp(stem, "/") == 0)
            snprintf(stem, sizeof stem, "archive");
    }
    int n = snprintf(dst, size, "%s%s%s", dir, stem, tar_comp_suffix(comp));
    return n >= 0 && (size_t)n < size;
}

/* Refuse requests the chosen template can't fully express. */
static bool tar_check(const ParseContext *ctx, const CommandTemplate *tpl,
                      char *err, size_t err_size) {
    char mode = tar_mode(tpl);
    if (ctx->stray) {
        set_err(err, err_size, "not sure what '%s' refers to; numbers are only used in "
                "e.g. 'strip 1 folder' when extracting", ctx->stray);
        return false;
    }
    if (nl_count_slots(ctx, SLOT_ARCHIVE) > 1) {
        set_err(err, err_size, "one archive at a time, please (got '%s' and another)",
                nl_find_slot(ctx, SLOT_ARCHIVE, 0));
        return false;
    }
    const char *dest = nl_find_slot(ctx, SLOT_DEST, 0);
    if (dest && !dest[0]) {
        set_err(err, err_size, "the name after \"into\" is empty%s", "");
        return false;
    }
    if (nl_count_slots(ctx, SLOT_DEST) > 1) {
        set_err(err, err_size, "more than one destination given ('%s' and another)",
                nl_find_slot(ctx, SLOT_DEST, 0));
        return false;
    }
    if (nl_count_slots(ctx, SLOT_EXT) > 1) {
        set_err(err, err_size, "pick one compression format%s", "");
        return false;
    }
    if (mode != 'x' && nl_count_slots(ctx, SLOT_NUM)) {
        set_err(err, err_size, "stripping leading folders only applies when extracting%s", "");
        return false;
    }
    if (mode == 'c' && nl_find_slot(ctx, SLOT_ARCHIVE, 0) && nl_find_slot(ctx, SLOT_DEST, 0)) {
        set_err(err, err_size, "got both an archive name and a destination ('%s'); "
                "give the full path instead, e.g. into out/backup.tar.gz",
                nl_find_slot(ctx, SLOT_DEST, 0));
        return false;
    }
    if (mode == 't' && nl_find_slot(ctx, SLOT_DEST, 0)) {
        set_err(err, err_size, "a destination ('%s') only applies when extracting",
                nl_find_slot(ctx, SLOT_DEST, 0));
        return false;
    }
    return tar_compression(ctx, err, err_size) != 0;
}

/* {EXCLUDES} {MEMBERS} {ARCHIVE} {TAR_FLAGS|mode}. Returns -1 if `name` is
 * not one of them, 0 on error, 1 when expanded. */
static int tar_placeholder(const ParseContext *ctx, const char *name, size_t len,
                           const char *rest, Out *o, char *err, size_t err_size) {
    char buf[NL_MAX_CMD_LEN];
    char quoted[NL_MAX_CMD_LEN];
#define IS(lit) (len == sizeof lit - 1 && strncmp(name, lit, len) == 0)
    if (IS("EXCLUDES") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_EXCLUDE); k++) {
            out_puts(o, " --exclude=");
            put_single_quoted(o, nl_find_slot(ctx, SLOT_EXCLUDE, k));
        }
        return 1;
    }
    if (IS("MEMBERS") && *rest == '}') {
        for (int k = 0; k < nl_count_slots(ctx, SLOT_FILE); k++) {
            const char *v = nl_find_slot(ctx, SLOT_FILE, k);
            /* a name starting with '-' must not be read as an option */
            if (v[0] == '-') { snprintf(buf, sizeof buf, "./%s", v); v = buf; }
            if (!nl_shell_quote(v, quoted, sizeof quoted)) { o->overflow = true; return 1; }
            out_puts(o, " ");
            out_puts(o, quoted);
        }
        return 1;
    }
    if (IS("ARCHIVE") && *rest == '}') {
        const char *a = nl_find_slot(ctx, SLOT_ARCHIVE, 0);
        if (!a) {
            if (!tar_derive_archive(ctx, tar_compression(ctx, err, err_size), buf, sizeof buf,
                                    err, err_size))
                return 0;
            a = buf;
        }
        if (a[0] == '-') {   /* keep "-rf.tar.gz" from looking like options */
            int n = snprintf(quoted, sizeof quoted, "./%s", a);
            if (n < 0 || (size_t)n >= sizeof buf) { o->overflow = true; return 1; }
            memcpy(buf, quoted, (size_t)n + 1);
            a = buf;
        }
        if (!nl_shell_quote(a, quoted, sizeof quoted)) { o->overflow = true; return 1; }
        out_puts(o, quoted);
        return 1;
    }
    if (IS("TAR_FLAGS") && rest[0] == '|' && strchr("cxt", rest[1]) && rest[2] == '}') {
        char comp = tar_compression(ctx, err, err_size);
        if (!comp) return 0;
        if (comp == 'Z') out_puts(o, "--zstd ");
        out_putn(o, "-", 1);
        out_putn(o, rest + 1, 1);
        if (strchr("zjJ", comp)) out_putn(o, &comp, 1);
        if (!strchr(ctx->flags, 'q')) out_putn(o, "v", 1);
        out_putn(o, "f", 1);
        return 1;
    }
#undef IS
    return -1;
}

static int tar_ph(const ParseContext *ctx, const Placeholder *ph, Out *o,
                  char *err, size_t err_size) {
    return tar_placeholder(ctx, ph->name, ph->len, ph->rest, o, err, err_size);
}

const ToolExpander nl_expander_tar = { tar_check, NULL, tar_ph };
