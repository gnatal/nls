/*
 * expander_int.h — internal interface between expander.c and the per-tool
 * expanders.
 *
 * expander.c owns the template grammar ({TYPE:i}, {OUT_FILE}, << >>, ...),
 * shell quoting and the raw-value allowlists. A tool with its own
 * placeholders or refusal rules provides a ToolExpander in
 * expander_<tool>.c (any hook may be NULL) and registers it in
 * EXPANDERS[] in expander.c. Tools without one (ffmpeg, jq, grep) use the
 * generic grammar only.
 */
#ifndef NL_EXPANDER_INT_H
#define NL_EXPANDER_INT_H

#include "nl.h"

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool overflow;
} Out;

/* A {NAME...} placeholder as the generic expander found it. */
typedef struct {
    const char *name;      /* "KIND", "TAR_FLAGS", ... (not NUL-terminated) */
    size_t len;            /* length of name                                 */
    const char *rest;      /* just after the name: ':', '|' or '}'           */
    const char *close;     /* the closing '}'                                */
    int *placeholders;     /* bump when the builder consumed user input      */
    bool *all_user;        /* clear when it fell back to a default: the      */
                           /* enclosing << >> group is then dropped          */
} Placeholder;

typedef struct {
    /* Refuse a template that can't honour the whole request (e.g. a find
     * -delete that would drop "older than 30 days"). Runs before expansion. */
    bool (*check)(const ParseContext *ctx, const CommandTemplate *tpl, char *err, size_t err_size);
    /* Emit text before the command (e.g. "mkdir -p out && "). */
    bool (*prologue)(const ParseContext *ctx, const CommandTemplate *tpl, Out *o,
                     char *err, size_t err_size);
    /* Expand a tool-specific placeholder: -1 if it isn't one, 0 on error
     * (err set), 1 when expanded. */
    int (*placeholder)(const ParseContext *ctx, const Placeholder *ph, Out *o,
                       char *err, size_t err_size);
} ToolExpander;

extern const ToolExpander nl_expander_find;
extern const ToolExpander nl_expander_tar;
extern const ToolExpander nl_expander_git;
extern const ToolExpander nl_expander_curl;
extern const ToolExpander nl_expander_magick;

/* ---- output ---- */
void out_putn(Out *o, const char *s, size_t n);
void out_puts(Out *o, const char *s);
/* Single-quote `v` unconditionally (--exclude='.git' reads better than
 * --exclude=.git and keeps globs away from the shell). */
void put_single_quoted(Out *o, const char *v);
/* snprintf(err, err_size, msg, arg) if err is non-NULL */
void set_err(char *err, size_t err_size, const char *msg, const char *arg);

/* ---- raw-value allowlists (values spliced without quoting) ---- */
bool raw_value_ok(SlotType type, const char *v);
bool git_ref_ok(const char *v);
bool magick_geom_ok(const char *v);

/* ---- small helpers ---- */
/* Does the template reference slot {NAME:idx} (directly, or through one of
 * the find builders)? */
bool template_uses(const char *tpl, SlotType type, int idx);
/* Does a FILE value name a regular file (rather than a directory)? */
bool looks_like_regular_file(const char *f);
bool all_digits_str(const char *s);
/* Value of a short digit string (callers check all_digits_str first). */
int atoi_safe(const char *s);

#endif /* NL_EXPANDER_INT_H */
