#ifndef NL_H
#define NL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NL_VERSION        "0.1.0"

#define NL_MAX_SLOTS      16
#define NL_MAX_TOKENS     32
#define NL_MAX_RAW        64
#define NL_MAX_KEYWORDS   16
#define NL_MAX_REQUIRED   6
#define NL_MAX_INPUT      2048
#define NL_ARENA_SIZE     4096
#define NL_MAX_CMD_LEN    4096
#define NL_MAX_FIELDS     64

typedef enum {
    TOOL_UNKNOWN = 0,
    TOOL_FFMPEG,
    TOOL_JQ,
    TOOL_GREP,
    TOOL_FIND,
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
    SLOT_EXCLUDE,    /* find: a directory / pattern to prune ("ignoring build")      */
    SLOT_LINES,      /* find: "less than 100 lines" -> "< 100" (awk NR comparison)   */
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
    char flags[8];                           /* grep: modifier letters i w v F; find: i */
    char find_kind;                          /* find: 'f', 'd', 'l', or 'a' (any type) */
    bool find_kind_set;                      /* find: the user named a file type     */
    char find_depth[8];                      /* find: -maxdepth value, "" if none    */
    const char *find_stray;                  /* find: a number nls couldn't place    */
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
} MatchResult;

/* templates.c */
extern const CommandTemplate NL_TEMPLATES[];
extern const size_t NL_TEMPLATE_COUNT;

/* lexer.c */
ToolId nl_tool_from_name(const char *name);
const char *nl_tool_name(ToolId tool);
const char *nl_slot_name(SlotType type);
const char *nl_slot_hint(SlotType type);
/* `json` (may be NULL) lets bare words like "emails" resolve to fields (.email) */
bool nl_lex(ParseContext *ctx, ToolId tool, const char *input, const JsonInfo *json);
const char *nl_find_slot(const ParseContext *ctx, SlotType type, int index);
int nl_count_slots(const ParseContext *ctx, SlotType type);

/* inspect.c */
bool nl_inspect_json(const char *path, JsonInfo *info);

/* matcher.c */
int nl_keyword_score(const ParseContext *ctx, const CommandTemplate *tpl);
void nl_match(const ParseContext *ctx, MatchResult *out, bool verbose);

/* expander.c */
bool nl_expand(const ParseContext *ctx, const CommandTemplate *tpl,
               char *out, size_t out_size, char *err, size_t err_size);
bool nl_shell_quote(const char *in, char *out, size_t out_size);

#endif /* NL_H */
