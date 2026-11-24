/*
 * templates.c — the compiled-in knowledge base.
 *
 * Rules for authoring templates (see expander.c for the full grammar):
 *  - FILE / QUOTED_STR / OUT_FILE are shell-quoted by the expander, so they
 *    must NEVER appear inside a single-quoted region. Pass strings to jq via
 *    `--arg v {QUOTED_STR:0}` and reference `$v` in the program.
 *  - Other slot types (TIME, RES, NUM, JSON_PATH, ...) are restricted to a
 *    safe alphabet and may appear inside single-quoted program text.
 *  - Order matters for ties: on equal score, the template requiring more
 *    slots wins, then the one listed first.
 *  - jq: the last field is the required input shape (SHAPE_ANY/OBJECT/ARRAY).
 *    When the input file can be peeked, templates for the other shape are
 *    skipped; e.g. on an array, "get .name" falls through to jq_each.
 *  - jq: when several JSON paths are given, paths followed by a comparator
 *    ("is", ">", "contains", ...) come first. So in filter templates
 *    {JSON_PATH:0} is the condition and {JSON_PATH:1} an optional projection.
 */
#include "nl.h"

#define KW(...)  { __VA_ARGS__ }
#define REQ(...) { __VA_ARGS__ }

#define F  SLOT_FILE
#define T  SLOT_TIME
#define R  SLOT_RES
#define E  SLOT_EXT
#define P  SLOT_JSON_PATH
#define S  SLOT_QUOTED_STR
#define N  SLOT_NUM
#define A  SLOT_ARCHIVE

#define GIF_FILTER "'fps={FPS:0|10},scale={RES:0|480:-1}:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse' -loop 0"

#define JQ_FILE " {FILE:0|}"
#define JQ_PROJ "<< | {JSON_PATH:1}>>"

const CommandTemplate NL_TEMPLATES[] = {
    /* ============================== ffmpeg ============================== */
    { TOOL_FFMPEG, "ffmpeg_cut_scale", "Cut a segment and scale it",
      "cut in.mp4 from 00:10 to 00:25 and scale to 720p",
      KW("cut", "trim", "clip", "from", "between", "scale", "resize"), REQ(F, T, T, R),
      "ffmpeg -ss {TIME:0} -to {TIME:1} -i {FILE:0} -vf scale={RES:0} -c:a copy {OUT_FILE||_cut}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_cut", "Cut a segment between two timestamps (no re-encode)",
      "cut in.mp4 from 00:10 to 00:25",
      KW("cut", "trim", "clip", "from", "between", "segment", "part"), REQ(F, T, T),
      "ffmpeg -ss {TIME:0} -to {TIME:1} -i {FILE:0} -c copy {OUT_FILE||_cut}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_cut_first", "Keep only the first N seconds",
      "keep the first 30 seconds of in.mp4",
      KW("cut", "trim", "clip", "first", "keep", "only", "beginning"), REQ(F, T),
      "ffmpeg -i {FILE:0} -t {TIME:0} -c copy {OUT_FILE||_cut}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_cut_last", "Keep only the last N seconds",
      "keep the last 10s of in.mp4",
      KW("cut", "trim", "clip", "last", "final", "keep", "only", "ending"), REQ(F, T),
      "ffmpeg -sseof -{TIME:0} -i {FILE:0} -c copy {OUT_FILE||_cut}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_seek", "Drop everything before a timestamp",
      "trim in.mp4 starting from 1:30",
      KW("cut", "trim", "clip", "from", "skip", "start", "starting", "after", "onwards", "onward"), REQ(F, T),
      "ffmpeg -ss {TIME:0} -i {FILE:0} -c copy {OUT_FILE||_cut}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_h265", "Re-encode with H.265/HEVC",
      "convert in.mp4 to h265",
      KW("h265", "hevc", "x265", "h.265", "libx265"), REQ(F),
      "ffmpeg -i {FILE:0} -c:v libx265 -crf {NUM:0|28} -preset medium -tag:v hvc1 -c:a copy {OUT_FILE|mp4|_hevc}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_gif_cut", "Make a GIF from a segment",
      "make a gif from in.mp4 between 00:05 and 00:08 at 15fps",
      KW("gif", "animated", "animation", "make", "create", "convert", "cut", "clip", "trim", "between", "segment", "part"),
      REQ(F, T, T),
      "ffmpeg -ss {TIME:0} -to {TIME:1} -i {FILE:0} -vf " GIF_FILTER " {OUT_FILE|!gif}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_gif", "Convert a video to an optimized GIF",
      "convert demo.mov to gif at 12fps width 320",
      KW("gif", "animated", "animation", "make", "create", "convert", "turn"), REQ(F),
      "ffmpeg -i {FILE:0} -vf " GIF_FILTER " {OUT_FILE|!gif}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_scale", "Resize a video",
      "scale in.mp4 to 720p",
      KW("scale", "resize", "resolution", "downscale", "upscale", "width", "height"), REQ(F, R),
      "ffmpeg -i {FILE:0} -vf scale={RES:0} -c:a copy {OUT_FILE||_scaled}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_compress", "Compress a video with H.264 (CRF, default 28)",
      "compress in.mp4",
      KW("compress", "shrink", "smaller", "reduce", "size", "optimize", "lighter"), REQ(F),
      "ffmpeg -i {FILE:0}<< -vf scale={RES:0}>> -c:v libx264 -crf {NUM:0|28} -preset medium "
      "-c:a aac -b:a {BITRATE:0|128k} {OUT_FILE|mp4|_compressed}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_thumbnail", "Grab a single frame as an image",
      "take a screenshot of in.mp4 at 00:30",
      KW("thumbnail", "screenshot", "frame", "snapshot", "still", "image", "picture", "capture", "grab", "extract"),
      REQ(F),
      "ffmpeg -ss {TIME:0|00:00:01} -i {FILE:0} -frames:v 1 {OUT_FILE|jpg|_thumb}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_frames", "Export frames as an image sequence",
      "extract frames from in.mp4 at 2 fps",
      KW("frames", "images", "every", "all", "extract", "each", "sequence", "export", "fps"), REQ(F),
      "ffmpeg -i {FILE:0}<< -vf fps={FPS:0}>> {OUT_FILE|png|_%04d}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_audio_mp3", "Extract the audio track as MP3",
      "extract audio from in.mp4 as mp3 at 320k",
      KW("extract", "audio", "mp3", "sound", "rip", "music", "convert", "save", "get"), REQ(F),
      "ffmpeg -i {FILE:0} -vn -c:a libmp3lame -b:a {BITRATE:0|192k} {OUT_FILE|!mp3}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_audio", "Extract the audio track (format from extension, default mp3)",
      "extract audio from in.mp4 as flac",
      KW("extract", "audio", "sound", "rip", "music", "save", "get", "flac", "wav", "ogg", "opus", "m4a", "aac"), REQ(F),
      "ffmpeg -i {FILE:0} -vn<< -b:a {BITRATE:0}>> {OUT_FILE|mp3}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_mute", "Remove the audio track",
      "remove audio from in.mp4",
      KW("remove", "mute", "strip", "without", "silent", "silence", "drop", "delete", "no", "audio", "sound"), REQ(F),
      "ffmpeg -i {FILE:0} -c:v copy -an {OUT_FILE||_muted}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_remux", "Change container without re-encoding",
      "remux in.mkv to mp4 without re-encoding",
      KW("remux", "rewrap", "container", "lossless", "copy", "without", "re-encoding", "reencoding", "re-encode", "losing"),
      REQ(F),
      "ffmpeg -i {FILE:0} -c copy {OUT_FILE|mp4}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_convert", "Convert to another format",
      "convert in.mkv to mp4",
      KW("convert", "change", "transcode", "format", "turn", "export", "save", "encode"), REQ(F, E),
      "ffmpeg -i {FILE:0} {OUT_FILE}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_convert_to", "Convert to an explicit output file",
      "convert in.mkv to out.webm",
      KW("convert", "change", "transcode", "format", "turn", "export", "save", "encode"), REQ(F, F),
      "ffmpeg -i {FILE:0} {OUT_FILE}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_fps", "Change the frame rate",
      "change in.mp4 to 30fps",
      KW("fps", "framerate", "frame", "rate", "change"), REQ(F, SLOT_FPS),
      "ffmpeg -i {FILE:0} -filter:v fps={FPS:0} -c:a copy {OUT_FILE||_fps}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_speed", "Speed up video and audio (default 2x)",
      "speed up in.mp4 1.5x",
      KW("speed", "faster", "fast", "accelerate", "timelapse"), REQ(F),
      "ffmpeg -i {FILE:0} -filter:v setpts=PTS/{FACTOR:0|2} -filter:a atempo={FACTOR:0|2} {OUT_FILE||_fast}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_reverse", "Play backwards",
      "reverse in.mp4",
      KW("reverse", "reversed", "backwards", "backward", "rewind"), REQ(F),
      "ffmpeg -i {FILE:0} -vf reverse -af areverse {OUT_FILE||_reversed}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_rotate_cw", "Rotate 90 degrees clockwise",
      "rotate in.mp4 clockwise",
      KW("rotate", "clockwise", "right"), REQ(F),
      "ffmpeg -i {FILE:0} -vf transpose=1 -c:a copy {OUT_FILE||_rotated}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_rotate_ccw", "Rotate 90 degrees counter-clockwise",
      "rotate in.mp4 counterclockwise",
      KW("rotate", "counterclockwise", "counter-clockwise", "anticlockwise", "counter", "left"), REQ(F),
      "ffmpeg -i {FILE:0} -vf transpose=2 -c:a copy {OUT_FILE||_rotated}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_hflip", "Mirror horizontally",
      "flip in.mp4 horizontally",
      KW("flip", "mirror", "horizontal", "horizontally"), REQ(F),
      "ffmpeg -i {FILE:0} -vf hflip -c:a copy {OUT_FILE||_flipped}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_vflip", "Flip vertically (upside down)",
      "flip in.mp4 upside down",
      KW("flip", "vertical", "vertically", "upside", "down"), REQ(F),
      "ffmpeg -i {FILE:0} -vf vflip -c:a copy {OUT_FILE||_flipped}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_grayscale", "Convert to black and white",
      "make in.mp4 black and white",
      KW("grayscale", "greyscale", "black", "white", "monochrome", "gray", "grey", "bw"), REQ(F),
      "ffmpeg -i {FILE:0} -vf format=gray -c:a copy {OUT_FILE||_gray}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_louder", "Increase volume (default 1.5x)",
      "make in.mp4 louder 2x",
      KW("volume", "louder", "boost", "increase", "amplify", "loud", "raise"), REQ(F),
      "ffmpeg -i {FILE:0} -filter:a volume={FACTOR:0|1.5} -c:v copy {OUT_FILE||_louder}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_quieter", "Decrease volume by half",
      "make in.mp4 quieter",
      KW("volume", "quieter", "quiet", "lower", "decrease", "reduce", "softer"), REQ(F),
      "ffmpeg -i {FILE:0} -filter:a volume=0.5 -c:v copy {OUT_FILE||_quieter}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_normalize", "Normalize loudness (EBU R128)",
      "normalize audio of in.mp4",
      KW("normalize", "normalise", "normalization", "loudness", "loudnorm", "level", "audio", "volume"), REQ(F),
      "ffmpeg -i {FILE:0} -af loudnorm -c:v copy {OUT_FILE||_normalized}", SHAPE_ANY },

    { TOOL_FFMPEG, "ffmpeg_info", "Show streams, codecs and duration (ffprobe)",
      "show info about in.mp4",
      KW("info", "information", "metadata", "details", "duration", "inspect", "probe",
         "codec", "codecs", "streams", "length", "ffprobe"), REQ(F),
      "ffprobe -hide_banner {FILE:0}", SHAPE_ANY },

    /* ================================ jq ================================ */
    { TOOL_JQ, "jq_paths", "List every path in the document",
      "show the structure of data.json",
      KW("paths", "structure", "schema", "tree", "outline", "layout"), REQ(0),
      "jq -r 'paths | map(tostring) | join(\".\")'" JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_pretty", "Pretty-print JSON",
      "pretty print data.json",
      KW("pretty", "prettify", "format", "beautify", "indent", "print", "show", "view", "display", "read", "open"), REQ(0),
      "jq ." JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_compact", "Minify JSON onto one line per value",
      "minify data.json",
      KW("compact", "minify", "minified", "minimize", "one", "line", "single", "compress"), REQ(0),
      "jq -c ." JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_validate", "Check that the input is valid JSON",
      "validate data.json",
      KW("validate", "valid", "check", "verify", "lint", "syntax"), REQ(0),
      "jq empty" JQ_FILE " && echo valid", SHAPE_ANY },

    { TOOL_JQ, "jq_keys", "List the keys of an object",
      "list keys of data.json",
      KW("keys", "fields", "properties", "attributes", "columns", "key"), REQ(0),
      "jq 'keys'" JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_keys_path", "List the keys of a nested object",
      "list keys of .config in data.json",
      KW("keys", "fields", "properties", "attributes", "columns", "key"), REQ(P),
      "jq '{JSON_PATH:0} | keys'" JQ_FILE, SHAPE_OBJECT },

    { TOOL_JQ, "jq_type", "Show the JSON type of the input",
      "what type is data.json",
      KW("type", "kind", "typeof"), REQ(0),
      "jq 'type'" JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_type_path", "Show the JSON type at a path",
      "what type is .id in data.json",
      KW("type", "kind", "typeof"), REQ(P),
      "jq '{JSON_PATH:0} | type'" JQ_FILE, SHAPE_OBJECT },

    { TOOL_JQ, "jq_length", "Count elements",
      "how many items in data.json",
      KW("count", "length", "how", "many", "much", "number", "size", "len"), REQ(0),
      "jq 'length'" JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_length_path", "Count elements of a nested value",
      "count .users in data.json",
      KW("count", "length", "how", "many", "much", "number", "size", "len"), REQ(P),
      "jq '{JSON_PATH:0} | length'" JQ_FILE, SHAPE_OBJECT },

    { TOOL_JQ, "jq_count_by", "Count items grouped by a field",
      "count users.json by .status",
      KW("count", "group", "by", "per", "occurrences", "frequency", "tally", "breakdown"), REQ(P),
      "jq 'group_by({JSON_PATH:0}) | map({key: (.[0]{JSON_PATH:0} | tostring), count: length})'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_first_n", "Take the first N elements",
      "first 5 items of data.json",
      KW("first", "head", "top", "limit", "take"), REQ(N),
      "jq '.[:{NUM:0}]'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_first", "Take the first element",
      "first item of data.json",
      KW("first", "head", "top"), REQ(0),
      "jq '.[0]'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_last_n", "Take the last N elements",
      "last 3 entries of data.json",
      KW("last", "tail", "bottom", "final", "latest"), REQ(N),
      "jq '.[-{NUM:0}:]'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_last", "Take the last element",
      "last item of data.json",
      KW("last", "tail", "bottom", "final", "latest"), REQ(0),
      "jq '.[-1]'" JQ_FILE, SHAPE_ARRAY },

    /* filters: {JSON_PATH:0} = condition, {JSON_PATH:1} = optional projection */
    { TOOL_JQ, "jq_filter_eq_str", "Keep items whose field equals a string",
      "filter users.json where .status is 'active'",
      KW("filter", "where", "whose", "is", "equals", "equal", "==", "=", "select", "named", "called", "only"),
      REQ(P, S),
      "jq --arg v {QUOTED_STR:0} 'map(select({JSON_PATH:0} == $v)" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_eq_num", "Keep items whose field equals a number/true/false/null",
      "users.json where .age is 30",
      KW("filter", "where", "whose", "is", "equals", "equal", "==", "=", "select", "only"), REQ(P, N),
      "jq 'map(select({JSON_PATH:0} == {NUM:0})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_ne_str", "Keep items whose field is not a string",
      "users.json where .role is not 'admin'",
      KW("filter", "where", "whose", "is", "not", "!=", "isnt", "isn't", "except", "excluding", "exclude", "without"),
      REQ(P, S),
      "jq --arg v {QUOTED_STR:0} 'map(select({JSON_PATH:0} != $v)" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_ne_num", "Keep items whose field is not a number/true/false/null",
      "users.json where .deleted is not true",
      KW("filter", "where", "whose", "is", "not", "!=", "isnt", "isn't", "except", "excluding", "exclude", "without"),
      REQ(P, N),
      "jq 'map(select({JSON_PATH:0} != {NUM:0})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_gt", "Keep items whose field is greater than N",
      "get .name from users.json where .age > 30",
      KW("filter", "where", "whose", "is", ">", "greater", "more", "above", "over", "bigger",
         "larger", "higher", "older", "exceeds", "than", "after"), REQ(P, N),
      "jq 'map(select({JSON_PATH:0} > {NUM:0})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_lt", "Keep items whose field is less than N",
      "users.json where .age < 18",
      KW("filter", "where", "whose", "is", "<", "less", "fewer", "below", "under", "smaller",
         "lower", "younger", "than", "before"), REQ(P, N),
      "jq 'map(select({JSON_PATH:0} < {NUM:0})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_gte", "Keep items whose field is at least N",
      "items.json where .price is at least 10",
      KW("filter", "where", "whose", "is", ">=", "least", "minimum"), REQ(P, N),
      "jq 'map(select({JSON_PATH:0} >= {NUM:0})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_lte", "Keep items whose field is at most N",
      "items.json where .price is at most 10",
      KW("filter", "where", "whose", "is", "<=", "most", "maximum"), REQ(P, N),
      "jq 'map(select({JSON_PATH:0} <= {NUM:0})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_between", "Keep items whose field is between two numbers (inclusive)",
      "items.json where .price is between 10 and 20",
      KW("filter", "where", "whose", "is", "between", "within", "range", "inclusive"), REQ(P, N, N),
      "jq 'map(select({JSON_PATH:0} >= {NUM:0} and {JSON_PATH:0} <= {NUM:1})" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_contains", "Keep items whose string field contains a substring",
      "users.json where .email contains 'gmail'",
      KW("filter", "where", "whose", "contains", "containing", "contain", "includes", "include", "has"),
      REQ(P, S),
      "jq --arg v {QUOTED_STR:0} 'map(select({JSON_PATH:0} | strings | contains($v))" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_regex", "Keep items whose field matches a regex",
      "users.json where .name matches '^A'",
      KW("filter", "where", "whose", "matches", "match", "matching", "regex", "pattern", "like", "test"),
      REQ(P, S),
      "jq --arg v {QUOTED_STR:0} 'map(select({JSON_PATH:0} | strings | test($v))" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_exists", "Keep items where a field is present (non-null)",
      "users.json where .email exists",
      KW("filter", "where", "whose", "exists", "exist", "present", "defined", "set", "nonnull",
         "have", "has", "having"), REQ(P),
      "jq 'map(select({JSON_PATH:0} != null)" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_filter_missing", "Keep items where a field is absent (null)",
      "users.json without .email",
      KW("filter", "where", "whose", "without", "missing", "lacking", "lack", "no", "absent", "null"), REQ(P),
      "jq 'map(select({JSON_PATH:0} == null)" JQ_PROJ "){COUNT}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_search", "Find every string containing text, anywhere",
      "search data.json for 'timeout'",
      KW("search", "find", "anywhere", "recursive", "recursively", "grep", "occurrences", "look"), REQ(S),
      "jq --arg v {QUOTED_STR:0} '.. | strings | select(contains($v))'" JQ_FILE, SHAPE_ANY },

    { TOOL_JQ, "jq_get", "Get the value at a path",
      "get .users[].email from data.json",
      KW("get", "extract", "value", "field", "show", "print", "fetch", "read", "select",
         "pluck", "output", "display", "what"), REQ(P),
      "jq -r '{JSON_PATH:0}'" JQ_FILE, SHAPE_OBJECT },

    { TOOL_JQ, "jq_each", "Get a field from every element of an array",
      "list .email of each user in users.json",
      KW("each", "every", "all", "list", "items", "map", "elements", "iterate",
         "get", "extract", "show", "print", "fetch", "pluck", "value", "field"), REQ(P),
      "jq -r '.[] | {JSON_PATH:0}'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_sort_by", "Sort an array by a field",
      "sort users.json by .age",
      KW("sort", "sorted", "order", "ordered", "arrange", "by", "asc", "ascending"), REQ(P),
      "jq 'sort_by({JSON_PATH:0})<< | map({JSON_PATH:1})>>'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_sort_desc", "Sort an array by a field, descending",
      "sort users.json by .age descending",
      KW("sort", "sorted", "order", "ordered", "arrange", "by", "desc", "descending", "reverse",
         "reversed", "decreasing"), REQ(P),
      "jq 'sort_by({JSON_PATH:0}) | reverse<< | map({JSON_PATH:1})>>'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_sort", "Sort an array",
      "sort data.json",
      KW("sort", "sorted", "order", "ordered", "arrange", "asc", "ascending"), REQ(0),
      "jq 'sort'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_group", "Group array elements by a field",
      "group users.json by .team",
      KW("group", "grouped", "grouping", "by", "bucket", "categorize"), REQ(P),
      "jq 'group_by({JSON_PATH:0})'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_unique_by", "Distinct values of a field",
      "unique .country values in users.json",
      KW("unique", "distinct", "dedupe", "deduplicate", "uniq", "different"), REQ(P),
      "jq 'map({JSON_PATH:0}) | unique'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_unique", "Remove duplicate elements",
      "dedupe data.json",
      KW("unique", "distinct", "dedupe", "deduplicate", "uniq", "duplicates"), REQ(0),
      "jq 'unique'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_sum", "Sum a numeric field",
      "sum .price in orders.json",
      KW("sum", "total", "add", "summed", "summing"), REQ(P),
      "jq 'map({JSON_PATH:0}) | add'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_avg", "Average of a numeric field",
      "average .age in users.json",
      KW("average", "avg", "mean"), REQ(P),
      "jq 'map({JSON_PATH:0}) | add / length'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_max", "Element with the largest field value",
      "item with the highest .price in items.json",
      KW("max", "maximum", "highest", "largest", "biggest", "most", "expensive", "oldest"), REQ(P),
      "jq 'max_by({JSON_PATH:0})<< | {JSON_PATH:1}>>'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_min", "Element with the smallest field value",
      "item with the lowest .price in items.json",
      KW("min", "minimum", "lowest", "smallest", "least", "cheapest", "youngest"), REQ(P),
      "jq 'min_by({JSON_PATH:0})<< | {JSON_PATH:1}>>'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_delete", "Delete a field",
      "delete .debug from config.json",
      KW("delete", "remove", "drop", "strip", "without", "unset"), REQ(P),
      "jq 'del({JSON_PATH:0})'" JQ_FILE, SHAPE_OBJECT },

    { TOOL_JQ, "jq_delete_each", "Delete a field from every element",
      "remove .password from each user in users.json",
      KW("delete", "remove", "drop", "strip", "without", "each", "every", "all", "items"), REQ(P),
      "jq 'map(del({JSON_PATH:0}))'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_csv_fields", "Export chosen fields of each element as CSV (up to 4)",
      "convert users.json to csv with .name .email",
      KW("csv", "spreadsheet", "excel", "comma", "export", "convert"), REQ(P),
      "jq -r '.[] | [{JSON_PATH:0}<<, {JSON_PATH:1}>><<, {JSON_PATH:2}>><<, {JSON_PATH:3}>>] | @csv'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_csv", "Export an array of objects as CSV with a header row",
      "convert users.json to csv",
      KW("csv", "spreadsheet", "excel", "comma", "export", "convert", "table"), REQ(0),
      "jq -r '(.[0] | keys_unsorted) as $k | $k, (.[] | [.[$k[]]]) | @csv'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_tsv", "Export an array of objects as TSV with a header row",
      "convert users.json to tsv",
      KW("tsv", "tab", "tabs", "tab-separated", "export", "convert"), REQ(0),
      "jq -r '(.[0] | keys_unsorted) as $k | $k, (.[] | [.[$k[]]]) | @tsv'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_flatten", "Flatten nested arrays",
      "flatten data.json",
      KW("flatten", "flat", "flattened", "unnest"), REQ(0),
      "jq 'flatten'" JQ_FILE, SHAPE_ARRAY },

    { TOOL_JQ, "jq_slurp", "Combine JSON lines / multiple values into one array",
      "combine lines of events.jsonl into an array",
      KW("slurp", "combine", "merge", "array", "lines", "jsonl", "ndjson", "collect"), REQ(0),
      "jq -s ." JQ_FILE, SHAPE_ANY },

    /* =============================== grep ===============================
     * Every template shares the generic verbs (GREP_VERBS), so they tie on
     * "find"/"search" and the mode words (files, count, context, ...) decide;
     * with no mode word the first one, grep_search, wins.
     * Modifiers ("ignore case", "whole word", "not containing", "literal")
     * are lexed into ctx->flags and merged by {FLAGS|base|allowed}. 'r' is
     * dropped when only regular files are searched. Patterns go through -e
     * so one starting with '-' can't be read as an option. */
#define GREP_VERBS "search", "find", "grep", "look", "show", "print", "locate"
#define GREP_INCL  "<< --include='*.{EXT:0}'>><< --include='*.{EXT:1}'>>"
#define GREP_PATS  " -e {QUOTED_STR:0}<< -e {QUOTED_STR:1}>><< -e {QUOTED_STR:2}>>"
#define GREP_PATHS " {FILE:0|.}<< {FILE:1}>><< {FILE:2}>>"
#define GREP_MODS  "iwvF"

    { TOOL_GREP, "grep_search", "Search for a pattern (recursive, with line numbers)",
      "search for 'TODO' in src/",
      KW(GREP_VERBS, "lookup", "occurrence", "occurrences", "usages", "uses"), REQ(S),
      "grep {FLAGS|rnE|" GREP_MODS "}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_files", "List only the names of files that match",
      "which files contain 'TODO' in src/",
      KW(GREP_VERBS, "files", "filenames", "filename", "names", "which", "list", "paths", "only"), REQ(S),
      "grep {FLAGS|rlE|" GREP_MODS "}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_count", "Count matching lines (per file)",
      "count lines matching 'ERROR' in app.log",
      KW(GREP_VERBS, "count", "how", "many", "number", "per", "each"), REQ(S),
      "grep {FLAGS|rcE|" GREP_MODS "}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_count_total", "Count every occurrence of a pattern (total)",
      "how many times does 'TODO' appear in src/",
      KW(GREP_VERBS, "count", "how", "many", "times", "total", "occurrences", "appear",
         "instances"), REQ(S),
      "grep {FLAGS|rhoE|" GREP_MODS "}" GREP_INCL GREP_PATS GREP_PATHS " | wc -l", SHAPE_ANY },

    { TOOL_GREP, "grep_only", "Print only the matched text, not the whole line",
      "extract only the matching part of '[0-9]+ms' from app.log",
      KW(GREP_VERBS, "only", "extract", "matched", "part", "parts", "just", "pull", "output"), REQ(S),
      "grep {FLAGS|roE|" GREP_MODS "}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_after", "Show matches plus N lines after (default 3)",
      "show 'panic' and 5 lines after in app.log",
      KW(GREP_VERBS, "after", "following", "below", "next"), REQ(S),
      "grep {FLAGS|rnE|" GREP_MODS "} -A {NUM:0|3}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_before", "Show matches plus N lines before (default 3)",
      "show 2 lines before 'Traceback' in app.log",
      KW(GREP_VERBS, "before", "preceding", "above", "previous", "prior"), REQ(S),
      "grep {FLAGS|rnE|" GREP_MODS "} -B {NUM:0|3}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_context", "Show matches with N lines of context (default 2)",
      "find 'timeout' in app.log with 3 lines of context",
      KW(GREP_VERBS, "context", "around", "surrounding", "near", "nearby", "before", "after"), REQ(S),
      "grep {FLAGS|rnE|" GREP_MODS "} -C {NUM:0|2}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    { TOOL_GREP, "grep_max", "Stop after the first N matches per file (default 1)",
      "first 5 matches of 'ERROR' in app.log",
      KW(GREP_VERBS, "first", "max", "maximum", "limit", "stop", "top", "most", "up"), REQ(S),
      "grep {FLAGS|rnE|" GREP_MODS "} -m {NUM:0|1}" GREP_INCL GREP_PATS GREP_PATHS, SHAPE_ANY },

    /* =============================== find ===============================
     * All filters are optional: the lexer turns "c files", "named 'x*'",
     * "larger than 100MB", "in the last 7 days", "ignoring build", "depth 2"
     * into slots / ctx fields, and the find builders ({KIND} {NAMES} {DEPTH}
     * {PRUNE} {PRINT}, see expander.c) place them. Every slot the user gave
     * must be used, so a template that can't express one is refused instead
     * of running a broader command.
     * Every template carries the nouns (files, directories, links) so they
     * tie, and find_files is listed first: a destructive template only wins
     * when a delete verb is present. With no keyword at all ("c files in
     * src") the first template, find_files, is used. */
#define FIND_VERBS  "find", "list", "show", "search", "locate", "get", "print"
#define FIND_NOUNS  "files", "directories", "links"
#define FIND_DEL    "delete", "remove", "rm", "erase", "purge", "clean", "wipe"
#define FIND_ROOTS  "find {FILE:0|.}<< {FILE:1}>><< {FILE:2}>>{DEPTH}"
/* line counts: awk exits 0 when the comparison holds, so -exec is a test */
#define FIND_TESTS  "{NAMES}<< -size {SIZE:0}>><< -size {SIZE:1}>><< {TIME:0}>><< {TIME:1}>>" \
                    "<< -exec awk 'END { exit !(NR {LINES:0}) }' {} \\;>>"

    { TOOL_FIND, "find_files", "Find files by name, type, size or age",
      "c files in src ignoring build",
      KW(FIND_VERBS, FIND_NOUNS, "where", "which", "first"), REQ(0),
      FIND_ROOTS "{PRUNE}{KIND}" FIND_TESTS "{PRINT}<< | head -n {NUM:0}>>", SHAPE_ANY },

    { TOOL_FIND, "find_empty", "Find empty files or directories",
      "empty directories in .",
      KW(FIND_VERBS, FIND_NOUNS, "empty", "blank"), REQ(0),
      FIND_ROOTS "{PRUNE}{KIND} -empty" FIND_TESTS "{PRINT}", SHAPE_ANY },

    { TOOL_FIND, "find_count", "Count matching files or directories",
      "how many c files in src",
      KW(FIND_VERBS, FIND_NOUNS, "count", "how", "many", "number", "total"), REQ(0),
      FIND_ROOTS "{PRUNE}{KIND}" FIND_TESTS "{PRINT} | wc -l", SHAPE_ANY },

    { TOOL_FIND, "find_largest", "List the N largest files (default 10)",
      "10 largest files in ~ ignoring .git",
      KW(FIND_VERBS, FIND_NOUNS, "largest", "biggest", "heaviest", "top", "sorted", "sort"), REQ(0),
      FIND_ROOTS "{PRUNE} -type f" FIND_TESTS " -exec du -h {} + | sort -rh | head -n {NUM:0|10}", SHAPE_ANY },

    { TOOL_FIND, "find_delete", "Delete matching files",
      "delete .DS_Store files recursively",
      KW(FIND_DEL, "files", "links"), REQ(0),
      FIND_ROOTS " -type f" FIND_TESTS " -delete", SHAPE_ANY },

    { TOOL_FIND, "find_delete_dirs", "Delete matching directories and their contents",
      "delete all node_modules directories",
      KW(FIND_DEL, "directories"), REQ(0),
      FIND_ROOTS " -mindepth 1 -type d" FIND_TESTS " -prune -exec rm -rf {} +", SHAPE_ANY },

    { TOOL_FIND, "find_delete_empty", "Delete empty files or directories",
      "delete empty directories in build/",
      KW(FIND_DEL, FIND_NOUNS, "empty", "blank"), REQ(0),
      FIND_ROOTS " -mindepth 1{KIND} -empty" FIND_TESTS " -delete", SHAPE_ANY },

    /* =============================== tar ================================
     * The lexer sorts paths into ARCHIVE (anything ending in a tar suffix),
     * DEST ("into dist/"; "in dist" when extracting) and FILE (sources when
     * creating, members when extracting or listing). Compression comes from
     * the archive suffix or a format word ("with xz"), gzip by default; see
     * the tar builders in expander.c. list comes before extract so a bare
     * archive name ("backup.tar.gz") lists rather than unpacks. */
    { TOOL_TAR, "tar_create", "Create a compressed archive from files or folders",
      "compress src/ into backup.tar.gz excluding .git and node_modules",
      KW("compress", "archive", "pack", "bundle", "tar", "backup", "back", "zip", "create",
         "make", "package", "save"), REQ(F),
      "tar{EXCLUDES} {TAR_FLAGS|c} {ARCHIVE}{MEMBERS}", SHAPE_ANY },

    { TOOL_TAR, "tar_list", "List the contents of an archive",
      "list contents of backup.tar.gz",
      KW("list", "contents", "show", "view", "what", "what's", "whats", "inside", "see", "ls",
         "peek", "inspect", "browse", "contains", "check"), REQ(A),
      "tar{EXCLUDES} {TAR_FLAGS|t} {ARCHIVE}{MEMBERS}", SHAPE_ANY },

    { TOOL_TAR, "tar_extract", "Extract an archive, optionally into a folder",
      "extract archive.tar.xz into dist/",
      KW("extract", "unpack", "untar", "unarchive", "decompress", "unzip", "uncompress",
         "expand", "restore", "unbundle", "inflate"), REQ(A),
      "tar{EXCLUDES} {TAR_FLAGS|x} {ARCHIVE}<< -C {DEST:0}>><< --strip-components={NUM:0}>>{MEMBERS}",
      SHAPE_ANY },

    /* =============================== git ================================
     * The lexer sorts names into BRANCH (acted on: "branch x", "switch to
     * x"), REV (worked from: "from main", "3 commits ago", a hash) and
     * EXCLUDE ("not on main", "since v1.0"); "last 3 commits" is NUM, and
     * "commits" / "the last commit" become the intents "history" /
     * "@last". As with find, every slot the user gave must be used,
     * so a template that can't express part of the request is refused.
     * Builders (see expander.c): {PATHS} " -- f..." | {AUTHOR} | {SPAN}
     * (diff range) | {BASE} (rebase base) | {COMMITS} (revert targets).
     * Order matters on ties: undo / revert / amend come before show, the
     * safe variant (soft reset, -d, clean -n) before the destructive one. */
#define B  SLOT_BRANCH
#define V  SLOT_REV
#define X  SLOT_EXCLUDE
#define GIT_UNDO   "undo", "uncommit", "rollback", "roll", "back", "cancel", "reset", "@last"
#define GIT_LOGW   "log", "history", "show", "list", "view", "see", "display", "print"
#define GIT_LOGOPT "{AUTHOR}<< {TIME:0}>><< {TIME:1}>>"
#define GIT_MSG    "<< -m {QUOTED_STR:0}>>"

    { TOOL_GIT, "git_unstage", "Unstage files, keeping their changes",
      "unstage src/main.c",
      KW("unstage", "unstaged", "unadd", "unstaging", "staging", "index"), REQ(0),
      "git restore --staged{PATHS|.}", SHAPE_ANY },

    { TOOL_GIT, "git_undo_soft", "Undo the last N commits, keeping their changes staged",
      "undo last 2 commits keep changes staged",
      KW(GIT_UNDO, "soft", "staged", "keep", "keeping"), REQ(0),
      "git reset --soft {TARGET}", SHAPE_ANY },

    { TOOL_GIT, "git_undo_mixed", "Undo the last N commits, keeping their changes unstaged",
      "undo the last commit and unstage its changes",
      KW(GIT_UNDO, "mixed", "unstaged", "unstage", "unstaging"), REQ(0),
      "git reset --mixed {TARGET}", SHAPE_ANY },

    { TOOL_GIT, "git_undo_hard", "Delete the last N commits AND their changes (destructive)",
      "throw away the last commit and its changes",
      KW(GIT_UNDO, "hard", "discard", "discarding", "throw", "away", "lose", "losing", "destroy",
         "delete", "drop", "erase", "wipe", "permanently", "completely"), REQ(0),
      "git reset --hard {TARGET}", SHAPE_ANY },

    { TOOL_GIT, "git_revert", "Revert commits with new undo commits (safe after pushing)",
      "revert the last commit",
      KW("revert", "reverse", "undo", "pushed", "public", "shared", "safely", "safe", "inverse",
         "already", "@last", "@commit"), REQ(0),
      "git revert --no-edit {COMMITS}", SHAPE_ANY },

    { TOOL_GIT, "git_undo_reset", "Undo an accidental reset (back to where HEAD just was)",
      "undo the last reset",
      KW("@undoreset"), REQ(0),
      "git reset --hard 'HEAD@{1}'", SHAPE_ANY },

    { TOOL_GIT, "git_undo_merge", "Undo the last merge or rebase (before pushing)",
      "undo the rebase",
      KW("@undorebase"), REQ(0),
      "git reset --hard ORIG_HEAD", SHAPE_ANY },

    { TOOL_GIT, "git_undo_amend", "Undo the last amend, keeping its changes staged",
      "undo the amend",
      KW("@undoamend"), REQ(0),
      "git reset --soft 'HEAD@{1}'", SHAPE_ANY },

    { TOOL_GIT, "git_squash", "Squash the last N commits into one",
      "squash last 4 commits",
      KW("squash", "combine", "meld", "fold", "collapse", "join", "merge", "message", "@last"),
      REQ(N),
      "git reset --soft HEAD~{NUM:0} && git commit" GIT_MSG, SHAPE_ANY },

    { TOOL_GIT, "git_rebase_interactive", "Interactively edit, reorder or squash recent commits",
      "interactively rebase the last 5 commits",
      KW("rebase", "interactive", "interactively", "reorder", "reword", "edit", "fixup",
         "rewrite", "clean", "tidy", "cleanup", "onto"), REQ(0),
      "git rebase -i {BASE}", SHAPE_ANY },

    { TOOL_GIT, "git_rebase", "Rebase the current branch onto another",
      "rebase onto main",
      KW("rebase", "onto", "replay", "catch", "update"), REQ(V),
      "git rebase {REV:0}", SHAPE_ANY },

    { TOOL_GIT, "git_pull_rebase", "Pull and replay local commits on top (no merge commit)",
      "pull with rebase",
      KW("pull", "rebase", "sync", "update", "latest", "linear"), REQ(0),
      "git pull --rebase", SHAPE_ANY },

    { TOOL_GIT, "git_amend", "Add staged changes (or files) to the last commit, same message",
      "add src/main.c to the last commit",
      KW("amend", "add", "forgot", "forgotten", "include", "@tolast", "staged", "missing",
         "@last"), REQ(0),
      "<<git add{PATHS} && >>git commit --amend --no-edit", SHAPE_ANY },

    { TOOL_GIT, "git_amend_message", "Change the last commit's message",
      "change the last commit message to 'fix login redirect'",
      KW("amend", "change", "edit", "fix", "reword", "message", "update", "rename", "@last"),
      REQ(S),
      "git commit --amend -m {QUOTED_STR:0}", SHAPE_ANY },

    { TOOL_GIT, "git_commit", "Commit staged changes (or given files) with a message",
      "commit with message 'fix login bug'",
      KW("commit", "save", "record", "message", "checkin"), REQ(S),
      "git commit -m {QUOTED_STR:0}{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_commit_all", "Stage everything, new files included, and commit",
      "commit all changes with message 'wip'",
      KW("commit", "all", "everything", "including", "untracked", "new", "stage", "add",
         "changes", "message"), REQ(S),
      "git add -A && git commit -m {QUOTED_STR:0}", SHAPE_ANY },

    { TOOL_GIT, "git_log", "Show commits (filter by author, date, path or count)",
      "commits by alice in the last 2 weeks",
      KW(GIT_LOGW, "recent", "latest"), REQ(0),
      "git log --oneline" GIT_LOGOPT "<< -n {NUM:0}>>{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_count", "Count commits (optionally by author, date, path or branch)",
      "how many commits by alice since 2024-01-01",
      KW("count", "how", "many", "number", "total"), REQ(0),
      "git rev-list --count" GIT_LOGOPT " {REV:0|HEAD}{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_diff", "Show changes: working tree, last N commits, or against a branch",
      "what changed in the last 3 commits",
      KW("diff", "changes", "changed", "show", "what", "unstaged", "modified", "uncommitted",
         "difference", "differences", "compare", "local", "working", "between"), REQ(0),
      "git diff{SPAN}{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_diff_staged", "Show staged changes (what the next commit will contain)",
      "show staged changes",
      KW("diff", "changes", "show", "what", "staged", "cached", "index", "committed"), REQ(0),
      "git diff --staged<< {REV:0}>>{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_diff_names", "List the files changed (working tree, last N commits, a branch)",
      "files changed in the last 3 commits",
      KW("files", "names", "filenames", "which", "list", "changed", "modified", "touched",
         "only", "what"), REQ(0),
      "git diff --name-only{SPAN}{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_log_patch", "Show commits with their diffs, e.g. everything touching a file",
      "commits touching src/lexer.c in last 7 days",
      KW(GIT_LOGW, "touching", "touched", "touch", "changes", "changed", "modified", "patch",
         "patches", "diffs", "affecting", "affected", "edited", "edits", "changing"), REQ(0),
      "git log" GIT_LOGOPT "<< -n {NUM:0}>> -p{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_log_follow", "Full history of one file, following renames",
      "full history of src/lexer.c including renames",
      KW("follow", "following", "renames", "renamed", "rename", "moves", "moved",
         "full"), REQ(F),
      "git log --follow" GIT_LOGOPT " -p{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_pickaxe", "Find commits that added or removed a string in the code",
      "find commit that added 'inspect.c'",
      KW("history", "find", "which", "when", "added", "removed", "introduced", "deleted", "add",
         "remove", "introduce", "delete", "code", "string", "search"), REQ(S),
      "git log -S {QUOTED_STR:0} --oneline" GIT_LOGOPT "{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_file_added", "Find the commit that added a file",
      "which commit added src/inspect.c",
      KW("find", "which", "when", "added", "add", "introduced", "introduce",
         "created", "create", "first"), REQ(F),
      "git log --diff-filter=A --oneline" GIT_LOGOPT "{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_file_deleted", "Find the commit that deleted a file",
      "which commit deleted src/old.c",
      KW("find", "which", "when", "deleted", "delete", "removed", "remove", "gone"),
      REQ(F),
      "git log --diff-filter=D --oneline" GIT_LOGOPT "{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_log_message", "Find commits whose message mentions something",
      "commits with message containing 'fix'",
      KW("history", "message", "messages", "mentioning", "mention", "mentions", "saying", "says",
         "titled", "subject", "containing", "contains", "grep", "find", "search", "which",
         "about"), REQ(S),
      "git log --oneline --grep={QUOTED_STR:0}" GIT_LOGOPT "{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_log_range", "Commits on one branch that aren't on another",
      "commits on feature-x not on main",
      KW("history", "@notin", "ahead", "only", "unique", "between"), REQ(X),
      "git log --oneline" GIT_LOGOPT " {EXCLUDE:0}..{REV:0|HEAD}{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_unpushed", "Commits not pushed yet",
      "commits I haven't pushed yet",
      KW("history", "unpushed", "pushed", "push", "outgoing", "ahead", "yet"), REQ(0),
      "git log --oneline '@{upstream}..HEAD'", SHAPE_ANY },

    { TOOL_GIT, "git_unpulled", "Commits on the remote you haven't pulled yet",
      "commits on the remote I haven't pulled",
      KW("history", "unpulled", "pulled", "pull", "incoming", "behind", "remote", "upstream",
         "fetch"), REQ(0),
      "git fetch && git log --oneline 'HEAD..@{upstream}'", SHAPE_ANY },

    { TOOL_GIT, "git_blame", "Who last changed each line of a file (or a line range)",
      "who changed lines 10 to 20 of src/main.c",
      KW("blame", "who", "wrote", "written", "author", "authored", "responsible", "annotate",
         "changed", "touched"), REQ(F),
      "git blame<< -L {LINES:0}>> {FILE:0}", SHAPE_ANY },

    { TOOL_GIT, "git_show_commit", "Show a commit's message and changed files (default: last)",
      "show the last commit",
      KW("show", "view", "inspect", "details", "detail", "what", "what's", "whats", "stat",
         "summary", "describe", "info", "changed", "@last", "@commit"), REQ(0),
      "git show --stat {REV:0|HEAD}", SHAPE_ANY },

    { TOOL_GIT, "git_show_file", "Print a file as it was at another commit or branch",
      "show src/main.c from 3 commits ago",
      KW("show", "view", "see", "print", "cat", "display", "version", "old", "contents",
         "content", "looked", "state", "get"), REQ(V, F),
      "git show {REV:0}:{FILE:0}", SHAPE_ANY },

    { TOOL_GIT, "git_restore_from", "Restore files from another commit or branch",
      "restore src/main.c from 2 commits ago",
      KW("restore", "recover", "bring", "back", "get", "checkout", "reset", "revert", "roll",
         "copy", "take", "grab", "rollback"), REQ(V, F),
      "git restore --source={REV:0}{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_restore_deleted", "Bring back a deleted file from the last commit that had it",
      "restore deleted file src/old.c",
      KW("restore", "recover", "bring", "back", "undelete", "resurrect", "deleted", "lost",
         "removed", "get"), REQ(F),
      "git checkout \"$(git rev-list -n 1 HEAD -- {FILE:0})^\" -- {FILE:0}", SHAPE_ANY },

    { TOOL_GIT, "git_discard", "Discard uncommitted changes to files (destructive)",
      "discard changes to src/main.c",
      KW("discard", "revert", "undo", "reset", "throw", "away", "drop", "lose", "changes",
         "local", "uncommitted", "restore", "edits", "modifications"), REQ(F),
      "git restore{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_untrack", "Stop tracking files but keep them on disk",
      "stop tracking .env",
      KW("untrack", "stop", "tracking", "track", "forget", "cached", "keep", "remove", "rm",
         "ignore"), REQ(F),
      "git rm -r --cached{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_clean_dry", "Preview which untracked files a clean would delete",
      "which untracked files would be deleted",
      KW("clean", "untracked", "delete", "remove", "preview", "dry", "run", "would", "what",
         "list", "show", "which"), REQ(0),
      "git clean -nd{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_clean", "Delete untracked files and folders (destructive)",
      "delete untracked files",
      KW("clean", "untracked", "delete", "remove", "wipe", "purge", "files"), REQ(0),
      "git clean -fd{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_clean_ignored", "Delete untracked AND ignored files, e.g. build output",
      "delete untracked and ignored files",
      KW("clean", "untracked", "delete", "remove", "wipe", "purge", "files", "ignored",
         "gitignored", "build", "artifacts"), REQ(0),
      "git clean -fdx{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_branch_create", "Create a branch and switch to it (optionally from a base)",
      "create branch feature-x from main",
      KW("create", "new", "make", "start", "branch", "open"), REQ(B),
      "git switch -c {BRANCH:0}<< {REV:0}>>", SHAPE_ANY },

    { TOOL_GIT, "git_switch_prev", "Switch back to the previous branch",
      "switch back to the previous branch",
      KW("switch", "checkout", "go", "back", "previous", "prior", "other", "return", "branch"),
      REQ(0),
      "git switch -", SHAPE_ANY },

    { TOOL_GIT, "git_switch", "Switch to a branch",
      "switch to main",
      KW("switch", "checkout", "go", "change", "move", "jump", "branch", "back", "return"),
      REQ(B),
      "git switch {BRANCH:0}", SHAPE_ANY },

    { TOOL_GIT, "git_branch_delete", "Delete local branches (only if merged)",
      "delete branch feature-x",
      KW("delete", "remove", "drop", "branch", "erase", "local"), REQ(B),
      "git branch -d {BRANCH:0}<< {BRANCH:1}>><< {BRANCH:2}>>", SHAPE_ANY },

    { TOOL_GIT, "git_branch_delete_force", "Force-delete local branches, even unmerged ones",
      "force delete branch feature-x",
      KW("delete", "remove", "drop", "branch", "erase", "force", "forcefully", "unmerged",
         "anyway"), REQ(B),
      "git branch -D {BRANCH:0}<< {BRANCH:1}>><< {BRANCH:2}>>", SHAPE_ANY },

    { TOOL_GIT, "git_branch_delete_remote", "Delete a branch on the remote (origin)",
      "delete branch feature-x on origin",
      KW("delete", "remove", "drop", "branch", "erase", "remote", "origin", "remotely",
         "github", "gitlab", "server", "upstream"), REQ(B),
      "git push origin --delete {BRANCH:0}<< {BRANCH:1}>><< {BRANCH:2}>>", SHAPE_ANY },

    { TOOL_GIT, "git_branch_rename", "Rename a branch (the current one if only a new name)",
      "rename branch old-name to new-name",
      KW("rename", "branch", "move", "call"), REQ(B),
      "git branch -m {BRANCH:0}<< {BRANCH:1}>>", SHAPE_ANY },

    { TOOL_GIT, "git_branch_list", "List branches, most recently committed first",
      "list branches by most recent commit",
      KW("list", "show", "branch", "recent", "sorted", "sort", "latest", "all", "view", "date",
         "@last"), REQ(0),
      "git branch --sort=-committerdate", SHAPE_ANY },

    { TOOL_GIT, "git_branch_merged", "List branches already merged (into the current or a branch)",
      "branches merged into main",
      KW("merged", "branch", "list", "show", "which", "done", "finished"), REQ(0),
      "git branch --merged<< {REV:0}>>", SHAPE_ANY },

    { TOOL_GIT, "git_branch_delete_merged", "Delete local branches already merged (keeps main/master/develop)",
      "delete merged branches",
      KW("@delmerged", "@delmerged+"), REQ(0),
      "git branch --merged {REV:0|HEAD} | grep -vE '^[*+]|^[[:space:]]*(main|master|develop)$'"
      " | xargs -r git branch -d", SHAPE_ANY },

    { TOOL_GIT, "git_merge", "Merge a branch into the current one (or into a target)",
      "merge feature-x into main",
      KW("merge", "integrate", "combine", "bring"), REQ(B),
      "<<git switch {REV:0} && >>git merge {BRANCH:0}", SHAPE_ANY },

    { TOOL_GIT, "git_merge_squash", "Squash-merge a branch into one staged change",
      "squash merge feature-x",
      KW("merge", "squash", "squashed", "single", "one"), REQ(B),
      "<<git switch {REV:0} && >>git merge --squash {BRANCH:0}", SHAPE_ANY },

    { TOOL_GIT, "git_merge_abort", "Abort an in-progress merge",
      "abort the merge",
      KW("abort", "cancel", "stop", "quit", "merge", "merging", "conflict", "conflicts"), REQ(0),
      "git merge --abort", SHAPE_ANY },

    { TOOL_GIT, "git_rebase_abort", "Abort an in-progress rebase",
      "abort the rebase",
      KW("abort", "cancel", "stop", "quit", "rebase", "rebasing"), REQ(0),
      "git rebase --abort", SHAPE_ANY },

    { TOOL_GIT, "git_cherry_pick", "Apply a commit from elsewhere onto the current branch",
      "cherry-pick a1b2c3d",
      KW("cherry-pick", "cherrypick", "cherry", "pick", "copy", "apply", "grab", "port",
         "backport", "@commit"), REQ(V),
      "git cherry-pick {REV:0}", SHAPE_ANY },

    { TOOL_GIT, "git_push_upstream", "Push the current (or a named) branch and set its upstream",
      "push the new branch and set upstream",
      KW("push", "upstream", "set", "track", "tracking", "publish", "first", "new", "branch",
         "remote", "origin"), REQ(0),
      "git push -u origin {BRANCH:0|HEAD}", SHAPE_ANY },

    { TOOL_GIT, "git_push_force", "Force-push safely (refuses if someone else pushed meanwhile)",
      "force push after rebase",
      KW("push", "force", "forcefully", "overwrite", "rewrite", "rebase", "safely", "lease",
         "amend"), REQ(0),
      "git push --force-with-lease", SHAPE_ANY },

    { TOOL_GIT, "git_tag", "Tag a commit (annotated when a message is given)",
      "tag the current commit as v1.2.0",
      KW("tag", "release", "mark", "version", "create", "label"), REQ(B),
      "git tag<< -a -m {QUOTED_STR:0}>> {BRANCH:0}<< {REV:0}>>", SHAPE_ANY },

    { TOOL_GIT, "git_tag_list", "List tags, newest first",
      "list tags",
      KW("list", "show", "tag", "all", "view", "which", "recent", "latest"), REQ(0),
      "git tag --sort=-creatordate", SHAPE_ANY },

    { TOOL_GIT, "git_tag_delete", "Delete a local tag",
      "delete tag v1.2.0",
      KW("delete", "remove", "drop", "tag", "erase", "untag"), REQ(B),
      "git tag -d {BRANCH:0}", SHAPE_ANY },

    { TOOL_GIT, "git_tag_push", "Push all tags to origin",
      "push tags",
      KW("push", "tag", "publish", "upload", "release"), REQ(0),
      "git push origin --tags", SHAPE_ANY },

    { TOOL_GIT, "git_stash", "Stash uncommitted changes (optionally named, or only some files)",
      "stash my changes with message 'wip login'",
      KW("stash", "shelve", "aside", "temporarily", "park", "hide", "wip", "save", "changes",
         "message"), REQ(0),
      "git stash push" GIT_MSG "{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_stash_untracked", "Stash changes including untracked files",
      "stash everything including untracked files",
      KW("stash", "shelve", "aside", "untracked", "including", "new", "everything", "all"),
      REQ(0),
      "git stash push -u" GIT_MSG "{PATHS}", SHAPE_ANY },

    { TOOL_GIT, "git_stash_pop", "Re-apply the latest stash and drop it",
      "pop the stash",
      KW("pop", "unstash", "restore", "back", "stash", "stashed", "reapply", "get", "bring",
         "apply"), REQ(0),
      "git stash pop", SHAPE_ANY },

    { TOOL_GIT, "git_stash_apply", "Apply the latest stash but keep it in the list",
      "apply the stash but keep it",
      KW("stash", "apply", "keep", "keeping", "copy", "without", "removing", "stashed"), REQ(0),
      "git stash apply", SHAPE_ANY },

    { TOOL_GIT, "git_stash_list", "List stashes",
      "list stashes",
      KW("stash", "list", "show", "view", "all", "what"), REQ(0),
      "git stash list", SHAPE_ANY },

    { TOOL_GIT, "git_reflog", "Show where HEAD has been (find lost commits or branches)",
      "find lost commits",
      KW("reflog", "show", "view", "lost", "recover", "find", "rescue", "dangling", "missing", "where", "been",
         "orphaned", "deleted", "branch"), REQ(0),
      "git reflog -n {NUM:0|30}", SHAPE_ANY },

    /* =============================== curl ===============================
     * The lexer finds the URL (http(s)://..., or a bare host such as
     * localhost:3000/x or api.example.com/users), headers and bearer tokens
     * (HEADER), a body (FILE, or inline JSON / name=value as QUOTED_STR),
     * where to save the response (DEST), and modifiers: follow redirects /
     * insecure / compressed (flags), "retry 3" (NUM), "timeout 10s" (TIME),
     * "as user alice" (AUTHOR). Every slot given must be used, so e.g. a
     * body on a GET is refused rather than dropped. Builders (expander.c):
     * {URL} {OPTS} {HEADERS} {BODY} {UPLOAD} {OUTPUT}. The plain GET comes
     * first: it is the default when no verb is recognized. */
#define U  SLOT_URL
#define CURL_SEND "{OPTS}{HEADERS}{BODY} {URL}{OUTPUT}"

    { TOOL_CURL, "curl_get", "Send a GET request and print the response",
      "get https://api.example.com/users",
      KW("get", "fetch", "call", "request", "hit", "query", "read", "curl", "load", "open",
         "show", "print", "see", "view", "body", "contents", "content"), REQ(U),
      "curl -sS{OPTS}{HEADERS} {URL}{OUTPUT}", SHAPE_ANY },

    { TOOL_CURL, "curl_get_json", "GET a JSON API and pretty-print the response with jq",
      "get json from https://api.example.com/users",
      KW("json", "pretty", "prettify", "format", "formatted", "jq", "parse", "beautify",
         "readable", "indent", "get", "fetch"), REQ(U),
      "curl -sS{OPTS} -H 'Accept: application/json'{HEADERS} {URL} | jq .", SHAPE_ANY },

    { TOOL_CURL, "curl_post", "POST a file, inline JSON or form fields",
      "post data.json to https://api.example.com/users",
      KW("post", "send", "submit", "create", "posting", "sending"), REQ(U),
      "curl -sS{OPTS} -X POST" CURL_SEND, SHAPE_ANY },

    { TOOL_CURL, "curl_put", "PUT a file, inline JSON or form fields (replace a resource)",
      "put config.json to https://api.example.com/settings",
      KW("put", "update", "replace", "overwrite", "upsert"), REQ(U),
      "curl -sS{OPTS} -X PUT" CURL_SEND, SHAPE_ANY },

    { TOOL_CURL, "curl_patch", "PATCH a resource with a file, inline JSON or form fields",
      "patch '{\"active\":false}' to https://api.example.com/users/42",
      KW("patch", "modify", "partial", "partially", "change", "edit"), REQ(U),
      "curl -sS{OPTS} -X PATCH" CURL_SEND, SHAPE_ANY },

    { TOOL_CURL, "curl_delete", "Send a DELETE request",
      "delete https://api.example.com/users/42 with token $TOKEN",
      KW("delete", "remove", "destroy", "del", "deleting", "erase"), REQ(U),
      "curl -sS{OPTS} -X DELETE{HEADERS} {URL}{OUTPUT}", SHAPE_ANY },

    { TOOL_CURL, "curl_status", "Print just the HTTP status code (after redirects)",
      "check response status of https://example.com",
      KW("status", "code", "check", "response", "http", "up", "alive", "online", "ping",
         "health", "healthy", "returns", "return", "reachable", "down", "working"), REQ(U),
      "curl -sSIL{OPTS|L}{HEADERS} -o /dev/null -w '%{http_code}\\n' {URL}", SHAPE_ANY },

    { TOOL_CURL, "curl_headers", "Show the response headers of a GET request",
      "show response headers of https://example.com",
      KW("headers", "header", "response", "show", "print", "see", "view", "list", "inspect"),
      REQ(U),
      "curl -sS{OPTS}{HEADERS} -D - -o /dev/null {URL}", SHAPE_ANY },

    { TOOL_CURL, "curl_download", "Download a file (follows redirects, fails on HTTP errors)",
      "download https://example.com/file.zip as out.zip",
      KW("download", "save", "grab", "downloading", "store", "pull", "wget", "write"), REQ(U),
      "curl -fL{OPTS|L}{HEADERS} {URL}{OUTPUT|O}", SHAPE_ANY },

    { TOOL_CURL, "curl_resume", "Resume an interrupted download",
      "resume download of https://example.com/big.iso",
      KW("resume", "continue", "resuming", "partial", "interrupted", "unfinished", "restart",
         "download", "finish"), REQ(U),
      "curl -fL{OPTS|L} -C -{HEADERS} {URL}{OUTPUT|O}", SHAPE_ANY },

    { TOOL_CURL, "curl_upload", "Upload files as a multipart form (-F file=@...)",
      "upload photo.png to https://example.com/upload",
      KW("upload", "uploading", "attach", "multipart", "form", "attachment"), REQ(U, F),
      "curl -sS{OPTS}{HEADERS}{UPLOAD} {URL}{OUTPUT}", SHAPE_ANY },

    { TOOL_CURL, "curl_timing", "Break down where a request spends its time (DNS, TLS, first byte)",
      "how long does https://example.com take",
      KW("time", "timing", "timings", "latency", "slow", "fast", "measure", "long", "speed",
         "benchmark", "take", "takes", "duration", "ttfb", "breakdown", "performance", "quick"),
      REQ(U),
      "curl -sS{OPTS}{HEADERS} -o /dev/null -w 'dns:        %{time_namelookup}s\\n"
      "connect:    %{time_connect}s\\ntls:        %{time_appconnect}s\\n"
      "first byte: %{time_starttransfer}s\\ntotal:      %{time_total}s\\n' {URL}", SHAPE_ANY },

    { TOOL_CURL, "curl_verbose", "Debug a request: show the TLS handshake and all headers (-v)",
      "debug https://example.com",
      KW("debug", "verbose", "verbosely", "trace", "troubleshoot", "handshake", "details",
         "detailed", "diagnose", "everything"), REQ(U),
      "curl -v{OPTS}{HEADERS} -o /dev/null {URL}", SHAPE_ANY },

    /* ============================== magick ==============================
     * ImageMagick 7 (`magick`). The lexer finds the input image(s) (FILE,
     * globs like 'frames/\*.png' are passed quoted: magick expands them
     * itself), the output (DEST: "to out.webp", "into small/"), formats
     * (EXT; "to webp" is moved last), a geometry (RES: 800x600, width 800,
     * 50%, 2x), quality (NUM), an amount (FACTOR: fuzz %, degrees, blur
     * sigma, border px), a colour (COLOR) and resize modifiers (flags '!'
     * exact, '>' only shrink). Every slot given must be used, so e.g. a
     * quality on a rotate is refused rather than dropped. Builders
     * (expander.c): {RESIZE} {CROP} {QUALITY} {INPUTS} {GLOB} {DELAY} {OUT_NAME}.
     * {OUT_FILE||?sfx} adds the suffix only when the name would otherwise
     * overwrite the input. Resize and convert come first: they are the
     * defaults when no verb is recognized ("hero.png 800x600", "a.png to webp"). */
#define MOUT(sfx) "{OUT_FILE||?_" sfx "}"
#define C  SLOT_COLOR
#define D  SLOT_DEST

    { TOOL_MAGICK, "magick_resize", "Resize an image (keeps aspect ratio; 'exactly' or 'only shrink' to change that)",
      "resize hero.png to width 800 convert to webp quality 85",
      KW("resize", "scale", "shrink", "downscale", "upscale", "enlarge", "smaller", "bigger",
         "larger", "width", "height", "size", "fit", "dimensions"), REQ(F, R),
      "magick {FILE:0}{RESIZE}{QUALITY} " MOUT("resized"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_convert", "Convert an image to another format",
      "convert hero.png to webp quality 85",
      KW("convert", "change", "turn", "save", "export", "transcode", "format"), REQ(F, E),
      "magick {FILE:0}{RESIZE}{QUALITY} {OUT_FILE}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_convert_to", "Convert an image to an explicit output file",
      "convert hero.png to hero.avif",
      KW("convert", "change", "turn", "save", "export", "transcode", "format"), REQ(F, D),
      "magick {FILE:0}{RESIZE}{QUALITY} {OUT_FILE}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_compress", "Shrink file size for the web (strip metadata, quality 80)",
      "compress photo.jpg",
      KW("compress", "optimize", "optimise", "smaller", "reduce", "lighter", "web", "shrink",
         "minify", "lossy", "filesize"), REQ(F),
      "magick {FILE:0}{RESIZE} -strip -quality {NUM:0|80} " MOUT("compressed"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_strip", "Remove EXIF/GPS metadata (auto-orients first so photos stay upright)",
      "remove exif metadata from photo.jpg",
      KW("strip", "metadata", "exif", "gps", "location", "privacy", "profile", "profiles",
         "remove", "clean", "scrub", "anonymize"), REQ(F),
      "magick {FILE:0} -auto-orient -strip " MOUT("clean"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_transparent", "Make a background colour transparent (default white, 5% fuzz)",
      "make sprite.png transparent background",
      KW("transparent", "transparency", "background", "remove", "alpha", "cutout", "knockout",
         "see-through", "clear", "bg"), REQ(F),
      "magick {FILE:0} -fuzz {FACTOR:0|5}% -transparent {COLOR:0|white} {OUT_FILE|png|_transparent}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_flatten", "Replace transparency with a solid background (default white)",
      "put logo.png on a white background",
      KW("flatten", "background", "solid", "fill", "opaque", "behind", "replace", "put",
         "onto", "bg"), REQ(F, C),
      "magick {FILE:0} -background {COLOR:0} -alpha remove -alpha off " MOUT("flat"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_flatten_white", "Remove transparency (white background)",
      "remove transparency from logo.png",
      KW("flatten", "opaque", "transparency", "alpha", "remove", "drop", "strip", "solid"), REQ(F),
      "magick {FILE:0} -background white -alpha remove -alpha off " MOUT("flat"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_rotate", "Rotate clockwise (default 90 degrees)",
      "rotate photo.jpg 90 degrees",
      KW("rotate", "rotated", "turn", "clockwise", "right", "cw", "degrees"), REQ(F),
      "magick {FILE:0} -rotate {FACTOR:0|90} " MOUT("rotated"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_rotate_ccw", "Rotate counter-clockwise (default 90 degrees)",
      "rotate photo.jpg counterclockwise",
      KW("rotate", "rotated", "turn", "counterclockwise", "counter-clockwise", "anticlockwise",
         "counter", "left", "ccw"), REQ(F),
      "magick {FILE:0} -rotate -{FACTOR:0|90} " MOUT("rotated"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_auto_orient", "Fix sideways photos using their EXIF orientation",
      "fix the orientation of photo.jpg",
      KW("orientation", "orient", "auto-orient", "upright", "sideways", "exif", "fix",
         "straighten", "correct"), REQ(F),
      "magick {FILE:0} -auto-orient " MOUT("oriented"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_flop", "Mirror horizontally (left-right, -flop)",
      "mirror photo.jpg horizontally",
      KW("mirror", "flop", "horizontal", "horizontally", "flip", "left-right"), REQ(F),
      "magick {FILE:0} -flop " MOUT("mirrored"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_flip", "Flip vertically (upside down, -flip)",
      "flip photo.jpg upside down",
      KW("flip", "vertical", "vertically", "upside", "down", "top-bottom"), REQ(F),
      "magick {FILE:0} -flip " MOUT("flipped"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_grayscale", "Convert to grayscale",
      "make photo.jpg black and white",
      KW("grayscale", "greyscale", "gray", "grey", "monochrome", "desaturate", "colorless"), REQ(F),
      "magick {FILE:0} -colorspace Gray " MOUT("gray"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_crop", "Crop to a size around the center",
      "crop photo.jpg to 800x600",
      KW("crop", "cut", "trim", "center", "centre", "middle"), REQ(F, R),
      "magick {FILE:0} -gravity center -crop {CROP} +repage " MOUT("cropped"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_trim", "Trim uniform borders / whitespace around the image",
      "trim the whitespace around logo.png",
      KW("trim", "whitespace", "autocrop", "borders", "edges", "empty", "space", "margins",
         "padding", "around"), REQ(F),
      "magick {FILE:0} -trim +repage " MOUT("trimmed"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_pad", "Pad to an exact canvas size, centered (default white)",
      "pad photo.jpg to 1080x1080 with a black background",
      KW("pad", "padding", "canvas", "extent", "letterbox", "square", "center", "centre",
         "extend", "fill"), REQ(F, R),
      "magick {FILE:0} -gravity center -background {COLOR:0|white} -extent {RES:0} " MOUT("padded"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_thumbnail", "Make a thumbnail (fits in 256x256 by default, strips metadata)",
      "make a 200x200 thumbnail of photo.jpg",
      KW("thumbnail", "thumbnails", "thumb", "preview", "icon", "avatar"), REQ(F),
      "magick {FILE:0} -thumbnail {RES:0|256x256} " MOUT("thumb"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_blur", "Gaussian blur (sigma, default 4)",
      "blur photo.jpg by 8",
      KW("blur", "blurred", "blurry", "soften", "gaussian", "smooth", "unfocus"), REQ(F),
      "magick {FILE:0} -blur 0x{FACTOR:0|4} " MOUT("blurred"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_sharpen", "Sharpen (sigma, default 1)",
      "sharpen photo.jpg",
      KW("sharpen", "sharper", "crisp", "crisper", "enhance", "sharpness"), REQ(F),
      "magick {FILE:0} -sharpen 0x{FACTOR:0|1} " MOUT("sharpened"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_border", "Add a border (default 10px black)",
      "add a 10px white border to photo.jpg",
      KW("border", "frame", "outline", "edge", "margin"), REQ(F),
      "magick {FILE:0} -bordercolor {COLOR:0|black} -border {FACTOR:0|10} " MOUT("border"), SHAPE_ANY },

    { TOOL_MAGICK, "magick_append", "Join images side by side (a horizontal strip / sprite sheet)",
      "combine a.png b.png c.png side by side into sprite.png",
      KW("combine", "join", "append", "merge", "side", "horizontal", "horizontally", "row",
         "sprite", "spritesheet", "strip", "stitch", "concatenate"), REQ(F),
      "magick{INPUTS} +append {OUT_NAME|appended|png}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_append_v", "Stack images on top of each other (vertical strip)",
      "stack a.png b.png vertically into column.png",
      KW("stack", "vertical", "vertically", "column", "top", "below", "above", "combine",
         "join", "append", "stitch"), REQ(F),
      "magick{INPUTS} -append {OUT_NAME|stacked|png}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_favicon", "Make a multi-size favicon.ico (16-256px)",
      "make a favicon from logo.png",
      KW("favicon", "ico", "icon", "icons", "browser", "website"), REQ(F),
      "magick {FILE:0} -background none -define icon:auto-resize=256,64,48,32,16 {OUT_NAME|favicon|ico}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_gif", "Animate several images into a looping GIF (default 10 fps)",
      "make a gif from 'frames/*.png' at 12 fps",
      KW("gif", "animation", "animated", "animate", "loop", "looping", "slideshow", "frames",
         "from"), REQ(F),
      "magick{DELAY} -loop 0{INPUTS} {OUT_NAME|animation|gif}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_batch_convert", "Convert every image of one format in a folder (originals kept)",
      "convert all png files in assets/ to webp",
      KW("convert", "all", "every", "each", "batch", "bulk", "files", "folder", "directory",
         "change", "turn", "mass"), REQ(E, E),
      "magick mogrify -format {EXT:1}{RESIZE}{QUALITY} {GLOB}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_batch_resize", "Resize every image of one format into another folder (default resized/)",
      "resize all jpg files in photos/ to width 1200 into small/",
      KW("resize", "scale", "shrink", "all", "every", "each", "batch", "bulk", "files",
         "folder", "directory", "mass", "downscale", "smaller"), REQ(E, R),
      "mkdir -p {DEST:0|resized} && magick mogrify -path {DEST:0|resized}{RESIZE}{QUALITY} {GLOB}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_size", "Print the width and height",
      "what are the dimensions of photo.jpg",
      KW("dimensions", "dimension", "size", "width", "height", "resolution", "big", "large",
         "wide", "tall", "pixels", "how"), REQ(F),
      "magick identify -format '%wx%h\\n' {FILE:0}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_info", "Show format, size, colour space and file size",
      "show info about photo.jpg",
      KW("info", "information", "identify", "inspect", "describe", "format", "type", "what",
         "show", "details"), REQ(F),
      "magick identify {FILE:0}", SHAPE_ANY },

    { TOOL_MAGICK, "magick_info_verbose", "Show every property, including EXIF metadata",
      "show all metadata of photo.jpg",
      KW("metadata", "exif", "verbose", "everything", "all", "properties", "full", "camera",
         "details", "show", "list", "print", "read", "view"), REQ(F),
      "magick identify -verbose {FILE:0}", SHAPE_ANY },

};

const size_t NL_TEMPLATE_COUNT = sizeof NL_TEMPLATES / sizeof NL_TEMPLATES[0];
