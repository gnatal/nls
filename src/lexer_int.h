/*
 * lexer_int.h — internal interface between lexer.c and the per-tool lexers.
 *
 * lexer.c tokenizes the prompt into RawToks and hands them to the tool's
 * lexer (lexer_<tool>.c), which fills ParseContext with typed slots and
 * intent tokens. The helpers below are shared by several tools.
 *
 * Adding a tool: write lexer_<tool>.c defining nl_lex_<tool>(), declare it
 * here and add it to LEXERS[] in lexer.c.
 */
#ifndef NL_LEXER_INT_H
#define NL_LEXER_INT_H

#include "nl.h"

typedef struct {
    char *text;   /* original text (case preserved), punctuation-trimmed */
    char *lower;  /* lowercased copy */
    bool quoted;
} RawTok;

/* A tool's lexer: classify toks[0..count) into ctx (already zeroed, with
 * ctx->tool set). `json` is what inspect.c learned about a jq input file
 * (NULL if none); other tools ignore it. Set ctx->overflow on overflow. */
typedef void (*NlLexFn)(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);

void nl_lex_ffmpeg(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_jq(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_grep(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_find(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_tar(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_git(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_curl(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);
void nl_lex_magick(ParseContext *ctx, const RawTok *toks, size_t count, const JsonInfo *json);

/* ---- strings & numbers ---- */
bool in_list(const char *s, const char *const *list);
size_t number_prefix(const char *s, bool allow_sign);
bool is_number(const char *s, bool allow_sign);
bool all_digits(const char *s, size_t len);
const char *extension_of(const char *s);
bool ext_in(const char *ext, const char *const *list);
bool looks_like_file(const char *s, ToolId tool);
void format_seconds(char *buf, size_t size, double secs);
bool is_alnum_word(const char *s, size_t max);

/* ---- word lists ---- */
extern const char *const STOPWORDS[];
extern const char *const FFMPEG_EXTS[];
extern const char *const JQ_EXTS[];
extern const char *const GREP_EXTS[];
extern const char *const FIND_EXCLUDE_WORDS[];
extern const char *const FIND_DIR_NOUNS[];
extern const char *const FIND_FILE_NOUNS[];

/* ---- building the context ---- */
const char *arena_dup(ParseContext *ctx, const char *s);
void add_slot(ParseContext *ctx, SlotType type, const char *value);
void add_intent(ParseContext *ctx, const char *word);
void add_flag(ParseContext *ctx, char f);
/* An ordinary word: an intent token unless it's empty or a stopword. */
void lex_intent(ParseContext *ctx, const char *lw);
bool is_keyword(ToolId tool, const char *w);

/* ---- phrases shared by several tools ---- */
size_t grep_modifier_at(ParseContext *ctx, const RawTok *toks, size_t count, size_t i, const char *only);
const char *grep_type_ext(const char *w);
/* The word before toks[i], skipping articles/quantifiers ("for all TODO"). */
const char *prev_word(const RawTok *toks, size_t i, size_t *at);
double find_time_unit(const char *u, bool m_is_minutes);
void find_add_exclude(ParseContext *ctx, const char *t);
size_t exclude_item(ParseContext *ctx, const RawTok *toks, size_t count, size_t i, bool (*word_ok)(const char *), const char *hidden_pat);

#endif /* NL_LEXER_INT_H */
