/*
 * Where a YouTube page carries its embedded JSON: ytInitialData (the page's
 * renderers) and ytInitialPlayerResponse (the watch page's player facts).
 * Included by youtube_lite.c; the one locator every provider route uses.
 *
 * YouTube has served each value in several carriers, and rolls new ones out
 * by A/B experiment, so the locator accepts all of them:
 *
 *   var ytInitialData = '\x7b\x22...';   a JavaScript string (mobile; the
 *                                        string is the JSON text, hex-escaped)
 *   var ytInitialData = {...};           a JavaScript object literal (desktop,
 *   window["ytInitialData"] = {...};     and the mobile player response)
 *   <script id="yt-initial-data" type="application/json">{...}</script>
 *   <script id="yt-initial-player-response" ...>{...}</script>
 *
 * A script element's content is raw text: the HTML parser decodes no
 * character references in it, so the JSON is taken byte for byte, and
 * markup-significant characters in it arrive as JSON escapes (<,
 * &), which the JSON scanner decodes where a string is read. An
 * assignment of anything else (`var ytInitialPlayerResponse = null;`, a
 * read such as `= window['ytInitialPlayerResponse']`) is not a carrier and
 * the search goes on past it.
 *
 * Nothing here evaluates script. The locator reads ahead of its window only
 * as far as one carrier's own syntax (a bounded tag or assignment), so a
 * caller can walk a page in bounded windows with no overlap. The decoder
 * produces the JSON text into a caller buffer sized from the bytes after the
 * carrier's start, in bounded steps.
 */

typedef enum {
    YOUTUBE_PAGE_DATA_INITIAL,
    YOUTUBE_PAGE_DATA_PLAYER
} YoutubePageDataKind;

typedef enum {
    YOUTUBE_PAGE_DATA_NONE,
    /* A JavaScript string literal; `value` is the byte after its quote. */
    YOUTUBE_PAGE_DATA_STRING,
    /* A raw JSON object; `value` is its opening brace. */
    YOUTUBE_PAGE_DATA_OBJECT,
    /* A raw JSON object as a script element's text; `value` is its brace. */
    YOUTUBE_PAGE_DATA_ELEMENT
} YoutubePageDataForm;

typedef struct {
    YoutubePageDataForm form;
    char quote;
    size_t value;
} YoutubePageDataSite;

typedef struct {
    YoutubePageDataSite site;
    size_t input;
    size_t depth;
    bool in_string;
    bool escaped;
} YoutubePageDataDecoder;

typedef enum {
    YOUTUBE_PAGE_DATA_MORE,
    YOUTUBE_PAGE_DATA_DONE,
    YOUTUBE_PAGE_DATA_FAILED
} YoutubePageDataStatus;

/* A start tag longer than this is not one YouTube writes. */
enum { YOUTUBE_PAGE_DATA_TAG_LIMIT = 512u };

static bool youtube_page_data_identifier_byte(char byte)
{
    return isalnum((unsigned char) byte) || byte == '_' || byte == '$';
}

static size_t youtube_page_data_space(const char *source, size_t length,
                                      size_t at)
{
    while (at < length && isspace((unsigned char) source[at])) at++;
    return at;
}

/* `name` met at `at` as a script variable: `name = <value>`, also as
   window["name"] = or window['name'] =. */
static bool youtube_page_data_assignment(const char *source, size_t length,
    size_t at, size_t name_length, YoutubePageDataSite *site)
{
    if (at > 0 && youtube_page_data_identifier_byte(source[at - 1u]))
        return false;
    at += name_length;
    if (at < length && youtube_page_data_identifier_byte(source[at]))
        return false;
    if (at > name_length && at < length
        && (source[at] == '"' || source[at] == '\'')) {
        /* The bracket form: the name is a quoted property key. */
        size_t open = at - name_length - 1u;
        if (source[open] != source[at] || open == 0
            || source[open - 1u] != '[') return false;
        at++;
        if (at >= length || source[at] != ']') return false;
        at++;
    }
    at = youtube_page_data_space(source, length, at);
    if (at >= length || source[at] != '='
        || (at + 1u < length && source[at + 1u] == '=')) return false;
    at = youtube_page_data_space(source, length, at + 1u);
    if (at >= length) return false;
    if (source[at] == '\'' || source[at] == '"') {
        *site = (YoutubePageDataSite) {
            YOUTUBE_PAGE_DATA_STRING, source[at], at + 1u};
        return true;
    }
    if (source[at] == '{') {
        *site = (YoutubePageDataSite) {YOUTUBE_PAGE_DATA_OBJECT, '\0', at};
        return true;
    }
    return false;
}

/* `id` met at `at` as the id attribute value of a script element whose text
   is a JSON object. */
static bool youtube_page_data_element(const char *source, size_t length,
    size_t at, size_t id_length, YoutubePageDataSite *site)
{
    size_t name = at;
    char quote = '\0';
    if (name > 0 && (source[name - 1u] == '"' || source[name - 1u] == '\'')) {
        quote = source[--name];
    }
    if (name < 3u || source[name - 1u] != '='
        || tolower((unsigned char) source[name - 2u]) != 'd'
        || tolower((unsigned char) source[name - 3u]) != 'i') return false;
    size_t end = at + id_length;
    if (end >= length) return false;
    if (quote != '\0' ? source[end] != quote
        : !(isspace((unsigned char) source[end]) || source[end] == '>'))
        return false;
    /* The element's own start tag: back to its `<`, which must open a
       script, and forward to its `>`, both within one bounded tag. */
    size_t floor = at > YOUTUBE_PAGE_DATA_TAG_LIMIT
        ? at - YOUTUBE_PAGE_DATA_TAG_LIMIT : 0;
    size_t open = name - 3u;
    while (open > floor && source[open] != '<' && source[open] != '>') open--;
    if (source[open] != '<' || length - open < 8u
        || strncasecmp(source + open + 1u, "script", 6u) != 0
        || !isspace((unsigned char) source[open + 7u])) return false;
    size_t ceiling = length - at > YOUTUBE_PAGE_DATA_TAG_LIMIT
        ? at + YOUTUBE_PAGE_DATA_TAG_LIMIT : length;
    char value_quote = '\0';
    for (at = end + (quote != '\0'); at < ceiling; at++) {
        char byte = source[at];
        if (value_quote != '\0') {
            if (byte == value_quote) value_quote = '\0';
        } else if (byte == '"' || byte == '\'') {
            value_quote = byte;
        } else if (byte == '>') {
            break;
        } else if (byte == '<') {
            return false;
        }
    }
    if (at >= ceiling) return false;
    at = youtube_page_data_space(source, length, at + 1u);
    if (at >= length || source[at] != '{') return false;
    *site = (YoutubePageDataSite) {YOUTUBE_PAGE_DATA_ELEMENT, '\0', at};
    return true;
}

/*
 * The first carrier of `kind` whose name starts in [*from, stop). On a find
 * *from is left at the name; otherwise at `stop`, where a later call
 * continues with no overlap.
 */
static bool youtube_page_data_find(const char *source, size_t length,
    size_t *from, size_t stop, YoutubePageDataKind kind,
    YoutubePageDataSite *site)
{
    const char *variable = kind == YOUTUBE_PAGE_DATA_PLAYER
        ? "ytInitialPlayerResponse" : "ytInitialData";
    const char *id = kind == YOUTUBE_PAGE_DATA_PLAYER
        ? "yt-initial-player-response" : "yt-initial-data";
    size_t variable_length = strlen(variable), id_length = strlen(id);
    if (stop > length) stop = length;
    for (size_t at = *from; at < stop; at++) {
        const char *candidate = memchr(source + at, 'y', stop - at);
        if (candidate == NULL) break;
        at = (size_t) (candidate - source);
        size_t left = length - at;
        if (((left >= variable_length
              && source[at + 1u] == 't'
              && memcmp(source + at, variable, variable_length) == 0
              && youtube_page_data_assignment(
                  source, length, at, variable_length, site))
             || (left >= id_length && source[at + 1u] == 't'
                 && memcmp(source + at, id, id_length) == 0
                 && youtube_page_data_element(
                     source, length, at, id_length, site)))) {
            *from = at;
            return true;
        }
    }
    *from = stop;
    return false;
}

static void youtube_page_data_decoder_init(YoutubePageDataDecoder *decoder,
                                           const YoutubePageDataSite *site)
{
    *decoder = (YoutubePageDataDecoder) {.site = *site, .input = site->value};
}

/* Room the decoded value can need: no carrier's text grows in decoding. */
static size_t youtube_page_data_capacity(const YoutubePageDataSite *site,
                                         size_t length)
{
    return length - site->value + 1u;
}

static YoutubePageDataStatus youtube_page_data_decode_string(
    YoutubePageDataDecoder *decoder, const char *source, size_t length,
    size_t stop, char *output, size_t capacity, size_t *used)
{
    const char *at = source + decoder->input;
    const char *end = source + length;
    const char *limit = source + stop;
    char quote = decoder->site.quote;
    YoutubePageDataStatus status = YOUTUBE_PAGE_DATA_MORE;
    while (at < limit && *at != quote) {
        unsigned char byte = (unsigned char) *at++;
        if (byte != '\\') {
            if (*used + 1u >= capacity) return YOUTUBE_PAGE_DATA_FAILED;
            output[(*used)++] = (char) byte;
            continue;
        }
        if (at >= end) return YOUTUBE_PAGE_DATA_FAILED;
        char escape = *at++;
        char simple = '\0';
        if (escape == 'n') simple = '\n';
        else if (escape == 'r') simple = '\r';
        else if (escape == 't') simple = '\t';
        else if (escape == 'b') simple = '\b';
        else if (escape == 'f') simple = '\f';
        else if (escape == '\n') continue;
        else if (escape != 'x' && escape != 'u') simple = escape;
        if (escape == 'x' || escape == 'u') {
            unsigned codepoint = 0;
            if (!lite_decode_escape_codepoint(&at, end, escape, &codepoint)
                || !lite_append_utf8(output, capacity, used, codepoint))
                return YOUTUBE_PAGE_DATA_FAILED;
        } else {
            if (*used + 1u >= capacity) return YOUTUBE_PAGE_DATA_FAILED;
            output[(*used)++] = simple;
        }
    }
    if (at < end && *at == quote) {
        at++;
        status = YOUTUBE_PAGE_DATA_DONE;
    } else if (at >= end) {
        status = YOUTUBE_PAGE_DATA_FAILED;
    }
    decoder->input = (size_t) (at - source);
    return status;
}

/* A raw JSON object is copied up to its closing brace. In a script
   element, raw text ends at `</script`, so meeting that first means the
   object was cut short. */
static YoutubePageDataStatus youtube_page_data_decode_object(
    YoutubePageDataDecoder *decoder, const char *source, size_t length,
    size_t stop, char *output, size_t capacity, size_t *used)
{
    size_t at = decoder->input;
    bool element = decoder->site.form == YOUTUBE_PAGE_DATA_ELEMENT;
    YoutubePageDataStatus status = YOUTUBE_PAGE_DATA_MORE;
    for (; at < stop; at++) {
        char byte = source[at];
        if (element && byte == '<' && length - at >= 8u
            && source[at + 1u] == '/'
            && strncasecmp(source + at + 2u, "script", 6u) == 0)
            return YOUTUBE_PAGE_DATA_FAILED;
        if (*used + 1u >= capacity) return YOUTUBE_PAGE_DATA_FAILED;
        output[(*used)++] = byte;
        if (decoder->in_string) {
            if (decoder->escaped) decoder->escaped = false;
            else if (byte == '\\') decoder->escaped = true;
            else if (byte == '"') decoder->in_string = false;
        } else if (byte == '"') {
            decoder->in_string = true;
        } else if (byte == '{' || byte == '[') {
            decoder->depth++;
        } else if (byte == '}' || byte == ']') {
            if (decoder->depth == 0) return YOUTUBE_PAGE_DATA_FAILED;
            if (--decoder->depth == 0) {
                at++;
                status = YOUTUBE_PAGE_DATA_DONE;
                break;
            }
        }
    }
    if (status == YOUTUBE_PAGE_DATA_MORE && at >= length)
        status = YOUTUBE_PAGE_DATA_FAILED;
    decoder->input = at;
    return status;
}

/*
 * Decodes the carrier's JSON text into `output` from where the last call
 * stopped, reading input up to `stop` (clamped to the page). `output` holds
 * `capacity` bytes, at least youtube_page_data_capacity(); on DONE it is
 * NUL-terminated at *used.
 */
static YoutubePageDataStatus youtube_page_data_decode(
    YoutubePageDataDecoder *decoder, const char *source, size_t length,
    size_t stop, char *output, size_t capacity, size_t *used)
{
    if (stop > length) stop = length;
    if (decoder->input > length || capacity == 0)
        return YOUTUBE_PAGE_DATA_FAILED;
    YoutubePageDataStatus status =
        decoder->site.form == YOUTUBE_PAGE_DATA_STRING
        ? youtube_page_data_decode_string(
            decoder, source, length, stop, output, capacity, used)
        : decoder->site.form == YOUTUBE_PAGE_DATA_NONE
        ? YOUTUBE_PAGE_DATA_FAILED
        : youtube_page_data_decode_object(
            decoder, source, length, stop, output, capacity, used);
    if (status == YOUTUBE_PAGE_DATA_DONE) output[*used] = '\0';
    return status;
}
