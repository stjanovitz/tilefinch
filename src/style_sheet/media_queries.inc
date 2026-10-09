/* One allocation-free Media Queries evaluator for stylesheets, resources,
   and matchMedia. Syntax/unknown values are not inverted into a match.
   Query, condition, and math work retain separate explicit ceilings. */
typedef enum { MEDIA_FALSE, MEDIA_TRUE, MEDIA_UNKNOWN, MEDIA_INVALID } MediaTruth;
typedef struct {
    const Stylesheet *sheet;
    double resolution;
    int width, height;
    unsigned conditions;
} MediaEvaluation;

static bool media_equal(const char *text, size_t length, const char *wanted)
{
    return style_math_identifier_equal(text, length, wanted);
}

static MediaTruth media_not(MediaTruth value)
{
    return value == MEDIA_TRUE ? MEDIA_FALSE
           : value == MEDIA_FALSE ? MEDIA_TRUE : value;
}

static bool media_word(const char *text, size_t length, const char *word,
                       size_t *used)
{
    size_t count = strlen(word);
    if (length < count || !media_equal(text, count, word)
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
        if (!media_equal(text, name, "calc") && !media_equal(text, name, "min")
            && !media_equal(text, name, "max")
            && !media_equal(text, name, "clamp")) return false;
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
            evaluation->width, evaluation->height, &pixels_q8)) return false;
    *value = (double) pixels_q8 / 256.0;
    return true;
}

static bool media_dimension(MediaEvaluation *evaluation, const char *text,
                            size_t length, double *value)
{
    trim(&text, &length);
    if (media_equal(text, length, "width") || media_equal(text, length, "device-width"))
        *value = evaluation->width;
    else if (media_equal(text, length, "height") || media_equal(text, length, "device-height"))
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
        double operands[3];
        bool dimensions[3] = {false};
        size_t start = 0;
        for (size_t i = 0; i <= count; i++) {
            size_t end = i < count ? operators[i] : length;
            dimensions[i] = media_dimension(evaluation, text + start, end - start, &operands[i]);
            if (!dimensions[i] && !media_length(evaluation, text + start, end - start, &operands[i]))
                return MEDIA_UNKNOWN;
            start = i < count ? end + sizes[i] : end;
        }
        if ((count == 1 && dimensions[0] == dimensions[1])
            || (count == 2 && (!dimensions[1] || dimensions[0] || dimensions[2]
                || text[operators[0]] == '=' || text[operators[1]] == '='
                || text[operators[0]] != text[operators[1]]))) return MEDIA_INVALID;
        bool match = true;
        for (size_t i = 0; i < count; i++)
            match &= media_compare(operands[i], text + operators[i], sizes[i], operands[i + 1]);
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
    if (name_length > 4 && (media_equal(name, 4, "min-") || media_equal(name, 4, "max-"))) {
        relation = tolower((unsigned char) name[1]) == 'i' ? 1 : -1;
        name += 4; name_length -= 4;
    }
    double actual, wanted;
    bool match = false;
    if (media_dimension(evaluation, name, name_length, &actual)) {
        if (colon == NULL) match = actual != 0 && !relation;
        else if (!media_length(evaluation, value, value_length, &wanted)) return MEDIA_UNKNOWN;
        else match = relation > 0 ? actual >= wanted : relation < 0 ? actual <= wanted : actual == wanted;
    } else if (media_equal(name, name_length, "aspect-ratio") || media_equal(name, name_length, "device-aspect-ratio")) {
        char buffer[64];
        if (!value_length || value_length >= sizeof(buffer)) return MEDIA_UNKNOWN;
        memcpy(buffer, value, value_length); buffer[value_length] = 0;
        char *end;
        double numerator = strtod(buffer, &end), denominator = 1;
        if (end == buffer) return MEDIA_UNKNOWN;
        while (isspace((unsigned char) *end)) end++;
        if (*end == '/') { char *right = end + 1; denominator = strtod(right, &end); if (end == right) return MEDIA_UNKNOWN; }
        while (isspace((unsigned char) *end)) end++;
        if (*end || numerator < 0 || denominator <= 0 || !isfinite(numerator) || !isfinite(denominator)) return MEDIA_UNKNOWN;
        actual = evaluation->width * denominator;
        wanted = evaluation->height * numerator;
        match = relation > 0 ? actual >= wanted : relation < 0 ? actual <= wanted : actual == wanted;
    } else if (media_equal(name, name_length, "resolution")) {
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
    } else if (media_equal(name, name_length, "color")
               || media_equal(name, name_length, "monochrome")) {
        actual = media_equal(name, name_length, "color") ? 8 : 0;
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
        const char *expected = NULL;
        if (media_equal(name, name_length, "orientation")) expected = evaluation->width > evaluation->height ? "landscape" : "portrait";
        else if (media_equal(name, name_length, "hover") || media_equal(name, name_length, "any-hover")) expected = "none";
        else if (media_equal(name, name_length, "pointer") || media_equal(name, name_length, "any-pointer")) expected = "coarse";
        else if (media_equal(name, name_length, "prefers-color-scheme")) expected = stylesheet_prefers_dark_color_scheme() ? "dark" : "light";
        else if (media_equal(name, name_length, "prefers-reduced-motion")) expected = "reduce";
        else if (media_equal(name, name_length, "prefers-contrast")) expected = "no-preference";
        else if (media_equal(name, name_length, "forced-colors")) expected = "none";
        else if (media_equal(name, name_length, "color-gamut")) expected = "srgb";
        else {
            if (evaluation->sheet != NULL)
                diagnostic_saturating_add(&((Stylesheet *) evaluation->sheet)->diagnostic_unknown_media_features, 1);
            return MEDIA_UNKNOWN;
        }
        if (colon) {
            const char *alternatives = NULL;
            if (media_equal(name, name_length, "orientation")) alternatives = "portrait landscape";
            else if (media_equal(name, name_length, "hover") || media_equal(name, name_length, "any-hover")) alternatives = "none hover";
            else if (media_equal(name, name_length, "pointer") || media_equal(name, name_length, "any-pointer")) alternatives = "none coarse fine";
            else if (media_equal(name, name_length, "prefers-color-scheme")) alternatives = "light dark";
            else if (media_equal(name, name_length, "prefers-reduced-motion")) alternatives = "reduce no-preference";
            else if (media_equal(name, name_length, "prefers-contrast")) alternatives = "no-preference less more custom";
            else if (media_equal(name, name_length, "forced-colors")) alternatives = "none active";
            else if (media_equal(name, name_length, "color-gamut")) alternatives = "srgb p3 rec2020";
            bool known = false;
            for (const char *at = alternatives; at && *at;) {
                const char *end = strchr(at, ' ');
                size_t count = end ? (size_t) (end - at) : strlen(at);
                if (value_length == count && strncasecmp(value, at, count) == 0) known = true;
                at = end ? end + 1 : NULL;
            }
            if (!known) return MEDIA_UNKNOWN;
        }
        match = colon ? media_equal(value, value_length, expected)
            : strcmp(expected, "none") && strcmp(expected, "no-preference");
    }
    return match ? MEDIA_TRUE : MEDIA_FALSE;
}

static MediaTruth media_condition(MediaEvaluation *evaluation, const char *text,
                                  size_t length, unsigned depth)
{
    trim(&text, &length);
    if (depth > 8 || ++evaluation->conditions > 64 || !length) return MEDIA_INVALID;
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
    if (!length || length > 512) return MEDIA_INVALID;
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
    bool screen = media_equal(text, type_length, "screen") || media_equal(text, type_length, "all");
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

static bool media_evaluate(const Stylesheet *sheet, const char *text, size_t length,
                           double resolution, int width, int height)
{
    if (!text || length > 4096) return false;
    const char *content = text; size_t content_length = length; trim(&content, &content_length);
    if (!content_length) return true;
    MediaEvaluation evaluation = {.sheet = sheet, .resolution = resolution,
                                  .width = width, .height = height};
    bool result = false;
    size_t start = 0; unsigned clauses = 0, nesting = 0;
    for (size_t at = 0; at <= length; at++) {
        if (at < length && text[at] == '(') { if (++nesting > 32) return false; }
        else if (at < length && text[at] == ')') { if (!nesting) return false; nesting--; }
        if (at == length || (!nesting && text[at] == ',')) {
            if (++clauses > 32 || nesting) return false;
            result |= media_query(&evaluation, text + start, at - start) == MEDIA_TRUE;
            start = at + 1;
        }
    }
    return result;
}
