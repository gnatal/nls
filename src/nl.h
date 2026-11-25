#ifndef NL_H
#define NL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* The release version. Don't bump it by hand: `make release V=X.Y.Z` does it
 * (with CHANGELOG.md, the man page and the docs), commits and tags vX.Y.Z.
 * `make version` prints it; the release workflow checks the tag. */
#define NL_VERSION        "0.1.0"

/* What `nls --version` prints. The Makefile appends the commit to builds
 * that aren't of the release tag itself: 0.1.0+16.g2827e3e (16 commits since
 * v0.1.0, or in total if it isn't tagged; ".dirty" for uncommitted changes).
 * Builds without git (a source tarball) print NL_VERSION alone. */
#ifndef NL_VERSION_SUFFIX
#define NL_VERSION_SUFFIX ""
#endif
#define NL_VERSION_FULL   NL_VERSION NL_VERSION_SUFFIX

#define NL_MAX_SLOTS      16
#define NL_MAX_TOKENS     32
#define NL_MAX_RAW        64
#define NL_MAX_KEYWORDS   28
#define NL_MAX_REQUIRED   6
#define NL_MAX_INPUT      2048
#define NL_ARENA_SIZE     4096
#define NL_MAX_CMD_LEN    4096
#define NL_MAX_FIELDS     64

/* Matching confidence (matcher.c). A single keyword scores at most 10, so
 * below NL_CONFIDENT_SCORE only one word matched; if the user said more
 * than that, the match is flagged low-confidence. A runner-up within
 * NL_CLOSE_MARGIN points is reported as an alternative. */
#define NL_CONFIDENT_SCORE 12
#define NL_CLOSE_MARGIN    2
#define NL_MAX_ALTS        2

typedef enum {
    TOOL_UNKNOWN = 0,
    TOOL_FFMPEG,
    TOOL_JQ,
    TOOL_GREP,
    TOOL_FIND,
    TOOL_TAR,
    TOOL_GIT,
    TOOL_CURL,
    TOOL_MAGICK,
    TOOL_COUNT
} ToolId;

typedef enum {
    SLOT_NONE = 0,
    SLOT_FILE,       /* input.mp4, users.json, ./clips/             (shell-quoted) */
    SLOT_EXT,        /* ffmpeg: mp3, mp4, gif; grep: file type (py) for --include */
    SLOT_TIME,       /* ffmpeg: 00:10, 15s -> "15", 2min -> "120";
                        find: a whole age test, "last 7 days" -> "-mtime -7"       */
    SLOT_RES,        /* 720p -> "-2:720", 1920x1080 -> "1920:1080"   (ffmpeg only)  */
    SLOT_BITRATE,    /* 128k, 320kbps -> "320k", 2M                  (ffmpeg only)  */
    SLOT_FPS,        /* 30fps, "30 fps", "fps 30"                    (ffmpeg only)  */
    SLOT_FACTOR,     /* 2x, 1.5x                                     (ffmpeg only)  */
    SLOT_NUM,        /* 10, -3, 2.5; in jq mode also true/false/null; grep: counts */
    SLOT_QUOTED_STR, /* 'ERROR', "bob", a bare word after a jq comparator, or a
                        grep pattern after "for"/"containing"/...              */
    SLOT_JSON_PATH,  /* .users[].email, .status                      (jq only)      */
    SLOT_SIZE,       /* find: "larger than 100MB" -> "+100M", "under 1k" -> "-1024c" */
    SLOT_EXCLUDE,    /* find: a directory / pattern to prune ("ignoring build");
                        tar: an --exclude pattern; git: "not on main" (log main..)  */
    SLOT_LINES,      /* find: "less than 100 lines" -> "< 100" (awk NR comparison);
                        git: "lines 10 to 20" -> "10,20" (blame -L)               */
    SLOT_ARCHIVE,    /* tar: a path ending in .tar, .tar.gz, .tgz, .tar.xz, ...       */
    SLOT_DEST,       /* tar: "into dist/" -> extraction folder, or the archive name
                        when creating ("compress src into backup")                 */
    SLOT_REV,        /* git: a revision to work from: "from main", "3 commits ago"
                        -> HEAD~3, a1b2c3d, HEAD~2                                 */
    SLOT_BRANCH,     /* git: a branch / tag acted on: "branch feature-x", "to main" */
    SLOT_AUTHOR,     /* git: "by alice", "by me" -> "me"; curl: "as user alice" (-u) */
    SLOT_URL,        /* curl: http(s)://... from a strict allowlist (single-quoted)  */
    SLOT_HEADER,     /* curl: 'X-Api-Key: abc', "token $TOKEN" -> Authorization     */
    SLOT_COLOR,      /* magick: white, red, '#ff8800' (shell-quoted on expansion)    */
    /* git TIME is a whole option: "--since='7 days ago'", "--until='2024-01-31'";
       curl TIME is a timeout in whole seconds, curl NUM a retry count;
       curl FILE is a request body / upload, DEST where to save the response;
       magick RES is a geometry ("800x600", "800x", "x600", "50%"), NUM the
       -quality (1-100), FACTOR the operation's amount (fuzz %, degrees, blur
       sigma, border px), FPS an animation rate, DEST the output file/folder */
    SLOT_TYPE_COUNT
} SlotType;

typedef struct {
    SlotType type;
    const char *value; /* NUL-terminated, stored in ParseContext.arena */
} ExtractedSlot;

/* jq: what top-level JSON shape a template needs. Enforced only when the
 * input file can be read; with stdin or an unreadable file anything goes. */
typedef enum {
    SHAPE_ANY = 0,
    SHAPE_OBJECT,   /* applies {JSON_PATH:0} to the root: invalid on arrays */
    SHAPE_ARRAY     /* iterates the root: invalid on objects               */
} InputShape;

typedef struct {
    ToolId tool;
    const char *id;                          /* e.g. "ffmpeg_cut"                    */
    const char *description;                 /* one-line human description          */
    const char *example;                     /* example prompt                      */
    const char *keywords[NL_MAX_KEYWORDS];   /* normalized intent tokens, NULL-terminated */
    SlotType required_slots[NL_MAX_REQUIRED];/* SLOT_NONE-terminated; repeats mean "need N" */
    const char *flag_template;
    InputShape shape;
} CommandTemplate;

/* jq: what inspect.c learned about the input file */
typedef struct {
    char shape;                              /* '[' or '{', 0 if unknown */
    char fields[NL_MAX_FIELDS][32];          /* identifier-like keys one level in */
    size_t field_count;
} JsonInfo;

typedef struct {
    ToolId tool;
    ExtractedSlot slots[NL_MAX_SLOTS];
    uint8_t slot_count;
    const char *intent_tokens[NL_MAX_TOKENS];
    uint8_t token_count;
    char arena[NL_ARENA_SIZE];
    size_t arena_used;
    uint8_t cond_paths;                      /* jq: paths used as conditions/keys */
    char input_shape;                        /* jq: '[' or '{' if FILE:0 was peeked, else 0 */
    bool wants_count;                        /* jq: "how many", "count", "number of" */
    char flags[8];                           /* grep: i w v F; find: i; tar: q (quiet);
                                                curl: L (follow) k (insecure) c (compressed);
                                                magick: ! (exact size) > (only shrink) */
    char find_kind;                          /* find: 'f', 'd', 'l', or 'a' (any type) */
    bool find_kind_set;                      /* find: the user named a file type     */
    char find_depth[8];                      /* find: -maxdepth value, "" if none    */
    const char *stray;                       /* find/tar/git/curl/magick: a number nls couldn't place */
    bool git_last;                           /* git: "the last commit" was mentioned  */
    const char *jq_bad_op;                   /* jq: unknown comparator after "where .x" */
    const char *jq_bad_path;                 /* jq: the path it followed               */
    const char *missing_path;                /* grep: "in src" but no such file/folder  */
    bool overflow;                           /* too many tokens/slots or arena full */
} ParseContext;

typedef struct {
    const CommandTemplate *best;             /* winning template, or NULL          */
    int best_score;
    const CommandTemplate *blocked;          /* best-scoring template that lacked slots */
    int blocked_score;
    SlotType missing[NL_MAX_REQUIRED];       /* what `blocked` was missing          */
    uint8_t missing_count;
    const CommandTemplate *wrong_shape;      /* best template skipped for input shape */
    int wrong_shape_score;
    uint32_t best_tokens;                    /* bit i: intent token i scored for `best` */
    const CommandTemplate *alts[NL_MAX_ALTS];/* close runners-up that could also run */
    int alt_scores[NL_MAX_ALTS];
    uint8_t alt_count;
    bool low_confidence;                     /* weak evidence: see NL_CONFIDENT_SCORE */
} MatchResult;

/* A destructive template: its command deletes data, overwrites it or
 * rewrites shared state. With -x, nls says what it destroys, runs the dry
 * run first and runs the command only on a typed "y"; -y skips that only
 * together with --force. */
typedef struct {
    const char *id;                          /* template id                         */
    const char *what;                        /* "deletes the matching files for good" */
    const char *dry_run;                     /* a harmless preview in the template
                                                grammar, taking the same slots;
                                                NULL if the tool has none           */
} Danger;

/* templates.c */
extern const CommandTemplate NL_TEMPLATES[];
extern const size_t NL_TEMPLATE_COUNT;
/* The template's Danger entry, or NULL if its command destroys nothing. */
const Danger *nl_danger(const CommandTemplate *tpl);

/* lexer.c */
ToolId nl_tool_from_name(const char *name);
const char *nl_tool_name(ToolId tool);
const char *nl_slot_name(SlotType type);
const char *nl_slot_hint(SlotType type);
/* tar: 'z' gzip, 'j' bzip2, 'J' xz, 'Z' zstd, '-' plain .tar, 0 if not a tar name */
char nl_tar_compression(const char *name, bool need_base);
/* `json` (may be NULL) lets bare words like "emails" resolve to fields (.email) */
bool nl_lex(ParseContext *ctx, ToolId tool, const char *input, const JsonInfo *json);
const char *nl_find_slot(const ParseContext *ctx, SlotType type, int index);
int nl_count_slots(const ParseContext *ctx, SlotType type);

/* inspect.c */
bool nl_inspect_json(const char *path, JsonInfo *info);

/* matcher.c */
int nl_keyword_score(const ParseContext *ctx, const CommandTemplate *tpl);
void nl_match(const ParseContext *ctx, MatchResult *out, bool verbose);
/* Same, considering only templates whose id starts with `only` (NULL: all)
 * and without the no-keyword fallbacks. */
void nl_match_only(const ParseContext *ctx, MatchResult *out, bool verbose, const char *only);

/* jq_compound.c: "where .a is x and .b > 3". Writes the command to `out`.
 * Returns false with `err` set on a refusal, or with `err` empty when the
 * prompt has no conjunction to split at. `quiet` drops the note on stderr
 * saying how "and"/"or" were grouped. */
bool nl_jq_compound(const char *prompt, const JsonInfo *json, bool verbose, bool quiet,
                    char *out, size_t out_size, char *err, size_t err_size);

/* expander.c */
/* Would the tool's expander accept this template for this request? (It
 * refuses e.g. a git command that can't honour a date filter.) */
bool nl_check(const ParseContext *ctx, const CommandTemplate *tpl);
bool nl_expand(const ParseContext *ctx, const CommandTemplate *tpl,
               char *out, size_t out_size, char *err, size_t err_size);
bool nl_shell_quote(const char *in, char *out, size_t out_size);
/* curl: does `s` start with http:// or https:// and stay inside the URL allowlist? */
bool nl_url_ok(const char *s);

/* explain.c: --explain. Print a part-by-part breakdown of `cmd` to `out`.
 * Parts it can't explain are marked NL_NO_EXPLANATION. */
#define NL_NO_EXPLANATION "(no explanation)"
void nl_explain(const char *cmd, FILE *out);

/* voice.c: --listen. Record a spoken request and transcribe it offline
 * (whisper.cpp, or $NLS_RECORD / $NLS_STT), normalized into `out`. Returns
 * 0, or the exit status after saying what went wrong (130: Ctrl+C). */
int nl_listen(bool verbose, char *out, size_t size);
/* Rewrite a transcript into words the lexer knows: "two" -> 2, "users dot
 * json" -> users.json, "restore slash" -> restore/; fillers and "[BLANK_AUDIO]"
 * dropped. False if it doesn't fit in `size`. */
bool nl_voice_normalize(const char *in, char *out, size_t size);

#endif /* NL_H */
