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

};

const size_t NL_TEMPLATE_COUNT = sizeof NL_TEMPLATES / sizeof NL_TEMPLATES[0];
