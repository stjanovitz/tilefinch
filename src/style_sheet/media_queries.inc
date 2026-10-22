/* One allocation-free Media Queries evaluator for stylesheets, resources,
   and matchMedia, whose condition grammar also evaluates size container
   queries. Syntax/unknown values are not inverted into a match.
   Query, condition, and math work retain separate explicit ceilings. */
typedef enum { MEDIA_FALSE, MEDIA_TRUE, MEDIA_UNKNOWN, MEDIA_INVALID } MediaTruth;
/* A comma-separated list: text bytes, parenthesis nesting and clauses. */
#define MEDIA_LIST_TEXT_LIMIT 4096u
#define MEDIA_LIST_NESTING_LIMIT 32u
#define MEDIA_LIST_CLAUSE_LIMIT 32u
/* One query's text, and its conditions' nesting depth and count. */
#define MEDIA_QUERY_TEXT_LIMIT 512u
#define MEDIA_CONDITION_DEPTH_LIMIT 8u
#define MEDIA_CONDITION_LIMIT 64u
typedef struct {
    const Stylesheet *sheet;
    double resolution;
    /* The queried box: the viewport, or a size container's content box. */
    int width, height;
    /* What vw/vh resolve against: always the viewport. */
    int viewport_width, viewport_height;
    /* Size container features (width, height, inline-size, block-size,
       aspect-ratio, orientation) instead of media features. */
    bool container;
    unsigned conditions;
} MediaEvaluation;

static MediaTruth media_not(MediaTruth value)
{
    return value == MEDIA_TRUE ? MEDIA_FALSE
           : value == MEDIA_FALSE ? MEDIA_TRUE : value;
}

static bool media_word(const char *text, size_t length, const char *word,
                       size_t *used)
{
    size_t count = strlen(word);
    if (length < count || !span_case_equal(text, count, word)
        || (count < length && (isalnum((unsigned char) text[count])
                               || text[count] == '-'))) return false;
    *used = count;
    return true;
}

static bool media_length(MediaEvaluation *evaluation, const char *text,
                         size_t length, double *value)
{
    trim(&text, &length);
    /* The CSS expression compiler supplies dimensional type checks and all
       existing math quotas. No custom properties or authored font context
       are visible here: media units use initial values. */
    if (length == 0 || length >= STYLE_MATH_SOURCE_CAPACITY) return false;
    bool function = isalpha((unsigned char) text[0]);
    if (function) {
        const char *open = memchr(text, '(', length);
        if (open == NULL || text[length - 1] != ')') return false;
        size_t name = (size_t) (open - text);
        if (!span_case_equal(text, name, "calc") && !span_case_equal(text, name, "min")
            && !span_case_equal(text, name, "max")
            && !span_case_equal(text, name, "clamp")) return false;
    }
    /* CSS calc sum operators require whitespace on both sides. Keep it
       rather than stripping it before invoking the shared compiler. */
    unsigned nesting = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '(') nesting++;
        else if (text[i] == ')') {
            if (!nesting) return false;
            nesting--;
            if (function && !nesting && i + 1 != length) return false;
        }
        else if (text[i] == '+' || text[i] == '-') {
            size_t previous = i;
            while (previous && isspace((unsigned char) text[previous - 1])) previous--;
            char prior = previous ? text[previous - 1] : '(';
            bool binary = previous && prior != '(' && prior != ','
                && prior != '*' && prior != '/' && prior != '+' && prior != '-';
            bool exponent = previous == i && (prior == 'e' || prior == 'E');
            if (binary && !exponent
                && (!nesting || i == 0 || !isspace((unsigned char) text[i - 1])
                    || i + 1 == length || !isspace((unsigned char) text[i + 1])))
                return false;
        } else if (!function && (text[i] == '*' || text[i] == '/')) return false;
    }
    if (!function && !isdigit((unsigned char) text[0]) && text[0] != '.'
        && text[0] != '+' && text[0] != '-') return false;
    if (nesting) return false;
    int32_t pixels_q8;
    if (!style_math_resolve_media_length(text, length,
            evaluation->viewport_width, evaluation->viewport_height,
            &pixels_q8)) return false;
    *value = (double) pixels_q8 / 256.0;
    return true;
}

static bool media_dimension(MediaEvaluation *evaluation, const char *text,
                            size_t length, double *value)
{
    trim(&text, &length);
    /* A size container's axes are its inline and block sizes; the device
       features exist only for media. */
    const char *wide = evaluation->container ? "inline-size" : "device-width";
    const char *tall = evaluation->container ? "block-size" : "device-height";
    if (span_case_equal(text, length, "width") || span_case_equal(text, length, wide))
        *value = evaluation->width;
    else if (span_case_equal(text, length, "height") || span_case_equal(text, length, tall))
        *value = evaluation->height;
    else return false;
    return true;
}

static bool media_compare(double left, const char *operator, size_t length,
                          double right)
{
    return length == 1 && operator[0] == '=' ? left == right
        : operator[0] == '<' ? (length == 2 ? left <= right : left < right)
        : length == 2 ? left >= right : left > right;
}

/* The ratio feature: aspect-ratio, plus device-aspect-ratio for media. The
   queried box's own ratio is width / height. */
static bool media_ratio_feature(const MediaEvaluation *evaluation,
                                const char *text, size_t length)
{
    trim(&text, &length);
    return span_case_equal(text, length, "aspect-ratio")
        || (!evaluation->container
            && span_case_equal(text, length, "device-aspect-ratio"));
}

/* <ratio> = <number [0,inf]> [ / <number (0,inf]> ]? */
static bool media_ratio(const char *text, size_t length,
                        double *numerator, double *denominator)
{
    trim(&text, &length);
    char buffer[64];
    if (!length || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length); buffer[length] = 0;
    char *end;
    *numerator = strtod(buffer, &end);
    *denominator = 1;
    if (end == buffer) return false;
    while (isspace((unsigned char) *end)) end++;
    if (*end == '/') { char *right = end + 1; *denominator = strtod(right, &end); if (end == right) return false; }
    while (isspace((unsigned char) *end)) end++;
    return !*end && *numerator >= 0 && *denominator > 0
        && isfinite(*numerator) && isfinite(*denominator);
}

/* Whether a range form's operands are well placed: one feature against one
   value, or `value op feature op value` with both operators pointing the same
   way and neither one '='. */
static bool media_range_shape(const char *text, const size_t operators[2],
                              size_t count, const bool features[3])
{
    if (count == 1) return features[0] != features[1];
    return features[1] && !features[0] && !features[2]
        && text[operators[0]] != '=' && text[operators[1]] != '='
        && text[operators[0]] == text[operators[1]];
}

static MediaTruth media_feature(MediaEvaluation *evaluation, const char *text,
                                size_t length)
{
    trim(&text, &length);
    size_t operators[2], sizes[2], count = 0;
    unsigned nesting = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '(') nesting++;
        else if (text[i] == ')') { if (!nesting) return MEDIA_INVALID; nesting--; }
        else if (!nesting && (text[i] == '<' || text[i] == '>' || text[i] == '=')) {
            if (count == 2) return MEDIA_INVALID;
            operators[count] = i;
            sizes[count] = text[i] != '=' && i + 1 < length && text[i + 1] == '=' ? 2 : 1;
            i += sizes[count++] - 1;
        }
    }
    if (nesting) return MEDIA_INVALID;
    if (count) {
        size_t starts[3], ends[3];
        bool ratio = false;
        for (size_t i = 0, start = 0; i <= count; i++) {
            starts[i] = start;
            ends[i] = i < count ? operators[i] : length;
            ratio |= media_ratio_feature(evaluation, text + starts[i], ends[i] - starts[i]);
            start = i < count ? ends[i] + sizes[i] : ends[i];
        }
        /* Each operand is a fraction: lengths over 1, ratios as written, and
           the ratio feature as width over height, compared crosswise. */
        double numerators[3], denominators[3];
        bool features[3] = {false};
        for (size_t i = 0; i <= count; i++) {
            const char *operand = text + starts[i];
            size_t operand_length = ends[i] - starts[i];
            denominators[i] = 1;
            if (ratio) {
                features[i] = media_ratio_feature(evaluation, operand, operand_length);
                if (features[i]) {
                    numerators[i] = evaluation->width;
                    denominators[i] = evaluation->height;
                } else if (!media_ratio(operand, operand_length,
                                        &numerators[i], &denominators[i]))
                    return MEDIA_UNKNOWN;
            } else {
                features[i] = media_dimension(evaluation, operand, operand_length, &numerators[i]);
                if (!features[i] && !media_length(evaluation, operand, operand_length, &numerators[i]))
                    return MEDIA_UNKNOWN;
            }
        }
        if (!media_range_shape(text, operators, count, features)) return MEDIA_INVALID;
        bool match = true;
        for (size_t i = 0; i < count; i++)
            match &= media_compare(numerators[i] * denominators[i + 1], text + operators[i], sizes[i],
                                   numerators[i + 1] * denominators[i]);
        return match ? MEDIA_TRUE : MEDIA_FALSE;
    }
    const char *colon = memchr(text, ':', length);
    size_t name_length = colon == NULL ? length : (size_t) (colon - text);
    const char *name = text;
    trim(&name, &name_length);
    const char *value = colon == NULL ? text + length : colon + 1;
    size_t value_length = colon == NULL ? 0 : length - (size_t) (value - text);
    trim(&value, &value_length);
    int relation = 0;
    if (name_length > 4 && (span_case_equal(name, 4, "min-") || span_case_equal(name, 4, "max-"))) {
        relation = tolower((unsigned char) name[1]) == 'i' ? 1 : -1;
        name += 4; name_length -= 4;
    }
    double actual, wanted;
    bool match = false;
    if (media_dimension(evaluation, name, name_length, &actual)) {
        if (colon == NULL) match = actual != 0 && !relation;
        else if (!media_length(evaluation, value, value_length, &wanted)) return MEDIA_UNKNOWN;
        else match = relation > 0 ? actual >= wanted : relation < 0 ? actual <= wanted : actual == wanted;
    } else if (media_ratio_feature(evaluation, name, name_length)) {
        double numerator, denominator;
        if (!media_ratio(value, value_length, &numerator, &denominator)) return MEDIA_UNKNOWN;
        actual = evaluation->width * denominator;
        wanted = evaluation->height * numerator;
        match = relation > 0 ? actual >= wanted : relation < 0 ? actual <= wanted : actual == wanted;
    } else if (evaluation->container
               && !span_case_equal(name, name_length, "orientation")) {
        return MEDIA_UNKNOWN;
    } else if (span_case_equal(name, name_length, "resolution")) {
        if (colon == NULL) return !relation && evaluation->resolution != 0
            ? MEDIA_TRUE : MEDIA_FALSE;
        char buffer[64];
        if (!value_length || value_length >= sizeof(buffer)) return MEDIA_UNKNOWN;
        memcpy(buffer, value, value_length); buffer[value_length] = 0;
        char *end; wanted = strtod(buffer, &end);
        if (end == buffer || !isfinite(wanted) || wanted < 0) return MEDIA_UNKNOWN;
        if (!strcasecmp(end, "dpi")) wanted /= 96;
        else if (!strcasecmp(end, "dpcm")) wanted *= 2.54 / 96;
        else if (strcasecmp(end, "dppx") && strcasecmp(end, "x")) return MEDIA_UNKNOWN;
        actual = evaluation->resolution;
        match = relation > 0 ? actual >= wanted : relation < 0 ? actual <= wanted : actual == wanted;
    } else if (span_case_equal(name, name_length, "color")
               || span_case_equal(name, name_length, "monochrome")) {
        actual = span_case_equal(name, name_length, "color") ? 8 : 0;
        if (!colon) match = !relation && actual != 0;
        else {
            char buffer[24];
            if (!value_length || value_length >= sizeof(buffer)) return MEDIA_UNKNOWN;
            memcpy(buffer, value, value_length); buffer[value_length] = 0;
            char *end; long bits = strtol(buffer, &end, 10);
            if (end == buffer || *end || bits < 0 || bits > 32767) return MEDIA_UNKNOWN;
            wanted = bits;
            match = relation > 0 ? actual >= wanted : relation < 0 ? actual <= wanted : actual == wanted;
        }
    } else {
        if (relation) return MEDIA_UNKNOWN;
        /* Discrete features: the device's value (computed for orientation
           and the color scheme) and every value the feature defines. */
        enum { MEDIA_VALUE_FIXED, MEDIA_VALUE_ORIENTATION, MEDIA_VALUE_SCHEME };
        static const struct {
            const char *name, *alias;
            unsigned char value_kind;
            const char *expected, *alternatives;
        } features[] = {
            {"orientation", NULL, MEDIA_VALUE_ORIENTATION, NULL,
             "portrait landscape"},
            {"hover", "any-hover", MEDIA_VALUE_FIXED, "none", "none hover"},
            {"pointer", "any-pointer", MEDIA_VALUE_FIXED, "coarse",
             "none coarse fine"},
            {"prefers-color-scheme", NULL, MEDIA_VALUE_SCHEME, NULL,
             "light dark"},
            {"prefers-reduced-motion", NULL, MEDIA_VALUE_FIXED, "reduce",
             "reduce no-preference"},
            {"prefers-contrast", NULL, MEDIA_VALUE_FIXED, "no-preference",
             "no-preference less more custom"},
            {"forced-colors", NULL, MEDIA_VALUE_FIXED, "none", "none active"},
            {"color-gamut", NULL, MEDIA_VALUE_FIXED, "srgb", "srgb p3 rec2020"},
        };
        size_t feature = 0;
        const size_t feature_count = sizeof(features) / sizeof(features[0]);
        while (feature < feature_count
               && !span_case_equal(name, name_length, features[feature].name)
               && !(features[feature].alias != NULL
                    && span_case_equal(name, name_length, features[feature].alias)))
            feature++;
        if (feature == feature_count) {
            if (evaluation->sheet != NULL)
                STYLE_DIAGNOSTIC_COUNT((Stylesheet *) evaluation->sheet,
                                       diagnostic_unknown_media_features);
            return MEDIA_UNKNOWN;
        }
        const char *expected = features[feature].expected;
        if (features[feature].value_kind == MEDIA_VALUE_ORIENTATION)
            expected = evaluation->width > evaluation->height ? "landscape" : "portrait";
        else if (features[feature].value_kind == MEDIA_VALUE_SCHEME)
            expected = stylesheet_prefers_dark_color_scheme() ? "dark" : "light";
        if (colon) {
            const char *alternatives = features[feature].alternatives;
            bool known = false;
            for (const char *at = alternatives; at && *at;) {
                const char *end = strchr(at, ' ');
                size_t count = end ? (size_t) (end - at) : strlen(at);
                if (value_length == count && strncasecmp(value, at, count) == 0) known = true;
                at = end ? end + 1 : NULL;
            }
            if (!known) return MEDIA_UNKNOWN;
        }
        match = colon ? span_case_equal(value, value_length, expected)
            : strcmp(expected, "none") && strcmp(expected, "no-preference");
    }
    return match ? MEDIA_TRUE : MEDIA_FALSE;
}

static MediaTruth media_condition(MediaEvaluation *evaluation, const char *text,
                                  size_t length, unsigned depth)
{
    trim(&text, &length);
    if (depth > MEDIA_CONDITION_DEPTH_LIMIT
        || ++evaluation->conditions > MEDIA_CONDITION_LIMIT || !length)
        return MEDIA_INVALID;
    size_t used = 0;
    if (media_word(text, length, "not", &used)) {
        const char *operand = text + used; size_t operand_length = length - used;
        trim(&operand, &operand_length);
        if (!operand_length || *operand != '(') return MEDIA_INVALID;
        unsigned nesting = 1; size_t end = 1;
        for (; end < operand_length && nesting; end++) {
            if (operand[end] == '(') nesting++;
            else if (operand[end] == ')') nesting--;
        }
        if (nesting || end != operand_length) return MEDIA_INVALID;
        return media_not(media_condition(evaluation, operand, operand_length, depth + 1));
    }
    if (*text != '(') return MEDIA_INVALID;
    size_t end = 1; unsigned nesting = 1;
    for (; end < length && nesting; end++) {
        if (text[end] == '(') nesting++;
        else if (text[end] == ')') nesting--;
    }
    if (nesting) return MEDIA_INVALID;
    const char *inside = text + 1;
    size_t inside_length = end - 2;
    trim(&inside, &inside_length);
    MediaTruth result = inside_length && (*inside == '(' || media_word(inside, inside_length, "not", &used))
        ? media_condition(evaluation, inside, inside_length, depth + 1)
        : media_feature(evaluation, inside, inside_length);
    const char *tail = text + end; size_t remaining = length - end;
    trim(&tail, &remaining);
    int operation = 0;
    while (remaining) {
        size_t operator_length;
        int next = media_word(tail, remaining, "and", &operator_length) ? 1
            : media_word(tail, remaining, "or", &operator_length) ? 2 : 0;
        if (!next || (operation && operation != next)) return MEDIA_INVALID;
        operation = next; tail += operator_length; remaining -= operator_length;
        trim(&tail, &remaining);
        if (!remaining || *tail != '(') return MEDIA_INVALID;
        size_t extent = 1; nesting = 1;
        for (; extent < remaining && nesting; extent++) {
            if (tail[extent] == '(') nesting++;
            else if (tail[extent] == ')') nesting--;
        }
        if (nesting) return MEDIA_INVALID;
        MediaTruth right = media_condition(evaluation, tail, extent, depth + 1);
        if (result == MEDIA_INVALID || right == MEDIA_INVALID) return MEDIA_INVALID;
        if (operation == 1) result = result == MEDIA_FALSE || right == MEDIA_FALSE ? MEDIA_FALSE
            : result == MEDIA_UNKNOWN || right == MEDIA_UNKNOWN ? MEDIA_UNKNOWN : MEDIA_TRUE;
        else result = result == MEDIA_TRUE || right == MEDIA_TRUE ? MEDIA_TRUE
            : result == MEDIA_UNKNOWN || right == MEDIA_UNKNOWN ? MEDIA_UNKNOWN : MEDIA_FALSE;
        tail += extent; remaining -= extent; trim(&tail, &remaining);
    }
    return result;
}

static MediaTruth media_query(MediaEvaluation *evaluation, const char *text, size_t length)
{
    trim(&text, &length);
    if (!length || length > MEDIA_QUERY_TEXT_LIMIT) return MEDIA_INVALID;
    size_t used; bool negated = false, only = false;
    if (media_word(text, length, "not", &used)) {
        const char *tail = text + used; size_t remaining = length - used; trim(&tail, &remaining);
        if (remaining && *tail == '(') return media_condition(evaluation, text, length, 0);
        negated = true; text = tail; length = remaining;
    } else if (media_word(text, length, "only", &used)) {
        only = true; text += used; length -= used; trim(&text, &length);
    }
    if (length && *text == '(') return only ? MEDIA_INVALID : media_condition(evaluation, text, length, 0);
    size_t type_length = 0;
    while (type_length < length && (isalpha((unsigned char) text[type_length]) || text[type_length] == '-')) type_length++;
    if (!type_length) return MEDIA_INVALID;
    bool screen = span_case_equal(text, type_length, "screen") || span_case_equal(text, type_length, "all");
    const char *tail = text + type_length; size_t remaining = length - type_length;
    trim(&tail, &remaining);
    MediaTruth result = screen ? MEDIA_TRUE : MEDIA_FALSE;
    if (remaining) {
        if (!media_word(tail, remaining, "and", &used)) return MEDIA_INVALID;
        /* A media type takes a condition-without-or. Authors can express
           an OR inside an additional parenthesized condition, not as a
           sibling of the media type's AND tail. */
        unsigned nesting = 0;
        for (size_t i = used; i < remaining; i++) {
            if (tail[i] == '(') nesting++;
            else if (tail[i] == ')' && nesting) nesting--;
            else if (!nesting && isalpha((unsigned char) tail[i])) {
                size_t word_length;
                if (media_word(tail + i, remaining - i, "or", &word_length))
                    return MEDIA_INVALID;
            }
        }
        MediaTruth condition = media_condition(evaluation, tail + used, remaining - used, 0);
        if (condition == MEDIA_INVALID) return condition;
        if (screen) result = condition;
    }
    return negated ? media_not(result) : result;
}

/* Whether any clause of a comma-separated list is true: media queries,
   or container conditions (one condition per clause). */
static bool media_evaluate_list(MediaEvaluation *evaluation, const char *text,
                                size_t length)
{
    if (!text || length > MEDIA_LIST_TEXT_LIMIT) return false;
    bool result = false;
    size_t start = 0; unsigned clauses = 0, nesting = 0;
    for (size_t at = 0; at <= length; at++) {
        if (at < length && text[at] == '(') { if (++nesting > MEDIA_LIST_NESTING_LIMIT) return false; }
        else if (at < length && text[at] == ')') { if (!nesting) return false; nesting--; }
        if (at == length || (!nesting && text[at] == ',')) {
            if (++clauses > MEDIA_LIST_CLAUSE_LIMIT || nesting) return false;
            MediaTruth clause = evaluation->container
                ? media_condition(evaluation, text + start, at - start, 0)
                : media_query(evaluation, text + start, at - start);
            result |= clause == MEDIA_TRUE;
            start = at + 1;
        }
    }
    return result;
}

static bool media_evaluate(const Stylesheet *sheet, const char *text, size_t length,
                           double resolution, int width, int height)
{
    if (!text || length > MEDIA_LIST_TEXT_LIMIT) return false;
    const char *content = text; size_t content_length = length; trim(&content, &content_length);
    if (!content_length) return true;
    MediaEvaluation evaluation = {.sheet = sheet, .resolution = resolution,
                                  .width = width, .height = height,
                                  .viewport_width = width,
                                  .viewport_height = height};
    return media_evaluate_list(&evaluation, text, length);
}

bool style_container_condition_matches(const char *text, size_t length,
                                       int width, int height,
                                       int viewport_width, int viewport_height)
{
    MediaEvaluation evaluation = {.width = width, .height = height,
                                  .viewport_width = viewport_width,
                                  .viewport_height = viewport_height,
                                  .container = true};
    return media_evaluate_list(&evaluation, text, length);
}
