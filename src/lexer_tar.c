/*
 * lexer_tar.c — tar: create, extract and list archives.
 */
#include "lexer_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *pw = prev_word(toks, i, &at);
    /* skip a dir noun: "into the folder dist" */
    if (pw && in_list(pw, FIND_DIR_NOUNS)) pw = prev_word(toks, at, &at);
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
        const char *pw = prev_word(toks, i, &at);
        if (pw && (in_list(pw, TAR_DEST_LEADERS) || in_list(pw, TAR_IN_WORDS) ||
                   in_list(pw, TAR_SOURCE_LEADERS))) {
            tar_add_path(ctx, toks, i + 1, st, 1);
            return 2;
        }
    }

    /* 11. A bare name after a verb or preposition: "compress src into backup",
     *     "extract it to dist", "src and docs" */
    size_t at = 0;
    const char *pw = prev_word(toks, i, &at);
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

void nl_lex_tar(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json) {
    (void)json;
    TarState tst = { false, false, 0 };
    for (size_t i = 0; i < count; i++)
        if (!toks[i].quoted && in_list(toks[i].lower, TAR_EXTRACT_VERBS)) tst.extracting = true;

    for (size_t i = 0; i < count && !ctx->overflow; i++) {
        size_t used = classify_tar(ctx, toks, count, i, &tst);
        if (used) { i += used - 1; continue; }
        lex_intent(ctx, toks[i].lower);
    }
}
