/* Keep nested SVG viewports and local clipping in the shared decoder. */
#define SVG_VIEWPORT_DEPTH_LIMIT 32u
#define SVG_CLIP_LIMIT 128u
#define SVG_CLIP_SHAPE_LIMIT 4096u

typedef struct SvgClipDefinition {
    char id[64];
    NSVGimage *image;
    bool object_space;
    bool luminance_mask;
    bool alpha_mask;
    bool region_object_space;
    float region[4];
    struct SvgClipDefinition *next;
} SvgClipDefinition;

typedef struct SvgClipUse {
    struct SvgClipUse *parent;
    char id[64];
    float transform[6];
    float rectangle[4];
    /* Object-bounding-box transform, resolved once after parsing. */
    float object_transform[6];
    unsigned char object_state;
    bool viewport;
    bool mask;
} SvgClipUse;

typedef struct {
    NSVGshape *shape;
    SvgClipUse *clip;
} SvgClipBinding;

/* Half-open [x0, y0, x1, y1] pixel rectangles bound the clip work: outside
   rect a mask is zero, and identity means it is 255 everywhere. */
typedef struct {
    unsigned char *pixels;
    SvgClipUse *clip;
    int rect[4];
    bool identity;
} SvgClipMask;

typedef struct {
    float min_x, min_y, width, height;
    int align_x, align_y, align_type;
} SvgViewportState;

typedef struct {
    NSVGparser *parser;
    SvgViewportState states[SVG_VIEWPORT_DEPTH_LIMIT];
    unsigned depth;
    bool failed;
    SvgClipUse *clip_stack[NSVG_MAX_ATTR];
    unsigned element_depth, clip_count, definition_count;
    /* Nonzero inside a clipPath or mask past the definition limit: its
       content is dropped rather than drawn as artwork. */
    unsigned discard_depth;
    /* A 256-bit filter of the ids of definitions dropped at that limit, so
       a mask that names one is ignored like an unresolved clip rather than
       hiding its artwork. A collision only ignores a missing mask. */
    uint32_t discarded_ids[8];
    NSVGparser *clip_parser;
    SvgClipDefinition *definitions;
    SvgClipBinding *bindings;
    size_t binding_count, binding_capacity, binding_cursor;
    float root_transform[6];
    unsigned char *scratch_mask;
    size_t mask_bytes;
    /* Two most recent masks, so A, B-inside-A, A alternation rebuilds
       neither. The second buffer is optional and only taken with ample
       budget headroom. Outside scratch_rect the scratch coverage is zero. */
    SvgClipMask masks[2];
    unsigned active_mask;
    bool second_mask_refused;
    int scratch_rect[4];
    int capture_row;
    bool mask_active, mask_capture;
    bool luminance_capture;
    unsigned mask_opacity, mask_luminance;
} SvgViewportParser;

static SvgViewportParser *svg_clips;

static void svg_viewport_parse_attributes(NSVGparser *p,
                                           const char **attributes)
{
    /* SVG's initial preserveAspectRatio is xMidYMid meet. NanoSVG's
       zero-initialized parser instead defaults to none. Authored values
       override these defaults independently for each viewport. */
    p->alignX = p->alignY = NSVG_ALIGN_MID;
    p->alignType = NSVG_ALIGN_MEET;
    nsvg__parseSVG(p, attributes);
    for (unsigned i = 0; attributes[i] != NULL; i += 2) {
        if (strcmp(attributes[i], "preserveAspectRatio") != 0) continue;
        /* NanoSVG recognizes lowercase yMin/yMid/yMax, but SVG's actual
           alignment keywords use uppercase Y (for example xMinYMin). */
        const char *value = attributes[i + 1];
        if (strstr(value, "YMin") != NULL) p->alignY = NSVG_ALIGN_MIN;
        else if (strstr(value, "YMid") != NULL) p->alignY = NSVG_ALIGN_MID;
        else if (strstr(value, "YMax") != NULL) p->alignY = NSVG_ALIGN_MAX;
    }
}

static void svg_viewport_start_native(void *opaque, const char *name,
                                const char **attributes)
{
    SvgViewportParser *context = opaque;
    NSVGparser *p = context->parser;
    if (context->failed) return;
    if (strcmp(name, "svg") != 0) {
        nsvg__startElement(p, name, attributes);
        return;
    }
    if (context->depth >= SVG_VIEWPORT_DEPTH_LIMIT
        || p->attrHead >= NSVG_MAX_ATTR - 1) {
        context->failed = true;
        svg_limit_refused = true;
        return;
    }
    SvgViewportState *state = &context->states[context->depth++];
    *state = (SvgViewportState) {
        p->viewMinx, p->viewMiny, p->viewWidth, p->viewHeight,
        p->alignX, p->alignY, p->alignType
    };
    if (context->depth == 1) {
        svg_viewport_parse_attributes(p, attributes);
        return;
    }
    float parent_width = p->viewWidth > 0 ? p->viewWidth : p->image->width;
    float parent_height = p->viewHeight > 0 ? p->viewHeight : p->image->height;
    float width = parent_width, height = parent_height, x = 0, y = 0;
    for (unsigned i = 0; attributes[i] != NULL; i += 2) {
        const char *key = attributes[i], *value = attributes[i + 1];
        if (strcmp(key, "x") == 0)
            x = nsvg__parseCoordinate(p, value, 0, parent_width);
        else if (strcmp(key, "y") == 0)
            y = nsvg__parseCoordinate(p, value, 0, parent_height);
        else if (strcmp(key, "width") == 0)
            width = nsvg__parseCoordinate(p, value, 0, parent_width);
        else if (strcmp(key, "height") == 0)
            height = nsvg__parseCoordinate(p, value, 0, parent_height);
    }
    float root_width = p->image->width, root_height = p->image->height;
    nsvg__pushAttr(p);
    p->viewMinx = p->viewMiny = 0;
    p->viewWidth = width;
    p->viewHeight = height;
    svg_viewport_parse_attributes(p, attributes);
    p->image->width = root_width;
    p->image->height = root_height;
    float sx = p->viewWidth > 0 ? width / p->viewWidth : 0;
    float sy = p->viewHeight > 0 ? height / p->viewHeight : 0;
    if (p->alignType == NSVG_ALIGN_MEET) sx = sy = nsvg__minf(sx, sy);
    else if (p->alignType == NSVG_ALIGN_SLICE) sx = sy = nsvg__maxf(sx, sy);
    float transform[6] = {
        sx, 0, 0, sy,
        x - p->viewMinx * sx + nsvg__viewAlign(p->viewWidth * sx, width, p->alignX),
        y - p->viewMiny * sy + nsvg__viewAlign(p->viewHeight * sy, height, p->alignY)
    };
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0
        || !isfinite(transform[0]) || !isfinite(transform[3])
        || !isfinite(transform[4]) || !isfinite(transform[5])) {
        nsvg__getAttr(p)->visible = 0;
    } else {
        nsvg__xformPremultiply(nsvg__getAttr(p)->xform, transform);
    }
}

static void svg_viewport_end_native(void *opaque, const char *name)
{
    SvgViewportParser *context = opaque;
    NSVGparser *p = context->parser;
    if (context->failed) return;
    if (strcmp(name, "svg") != 0) {
        nsvg__endElement(p, name);
        return;
    }
    if (context->depth == 0) { context->failed = true; return; }
    if (context->depth > 1) {
        const SvgViewportState *state = &context->states[context->depth - 1];
        p->viewMinx = state->min_x;
        p->viewMiny = state->min_y;
        p->viewWidth = state->width;
        p->viewHeight = state->height;
        p->alignX = state->align_x;
        p->alignY = state->align_y;
        p->alignType = state->align_type;
        nsvg__popAttr(p);
    }
    context->depth--;
}

/* Attribute CSS is bounded by the admitted SVG source. Only local URL
   references are supported; an external clip can never trigger a fetch. */
static const char *svg_clip_attribute(const char **attributes, const char *name)
{
    const char *style = NULL, *attribute = NULL;
    for (unsigned i = 0; attributes[i] != NULL; i += 2) {
        if (strcmp(attributes[i], name) == 0) attribute = attributes[i + 1];
        if (strcmp(attributes[i], "style") == 0) style = attributes[i + 1];
    }
    if (style == NULL) return attribute;
    size_t length = strlen(name);
    for (const char *at = style; *at != '\0';) {
        while (*at == ' ' || *at == '\t' || *at == ';') at++;
        if (strncmp(at, name, length) == 0) {
            const char *value = at + length;
            while (*value == ' ' || *value == '\t') value++;
            if (*value++ == ':') {
                while (*value == ' ' || *value == '\t') value++;
                return value;
            }
        }
        while (*at != '\0' && *at != ';') at++;
    }
    return attribute;
}

static unsigned svg_clip_id_bit(const char *id)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; id[i] != '\0'; i++)
        hash = (hash ^ (unsigned char) id[i]) * UINT32_C(16777619);
    return (unsigned) (hash & 255u);
}

static void svg_clip_note_discarded(SvgViewportParser *context,
                                    const char *id)
{
    if (id == NULL || id[0] == '\0') return;
    unsigned bit = svg_clip_id_bit(id);
    context->discarded_ids[bit / 32u] |= UINT32_C(1) << (bit % 32u);
}

static bool svg_clip_discarded(const SvgViewportParser *context,
                               const char *id)
{
    if (id == NULL || id[0] == '\0') return false;
    unsigned bit = svg_clip_id_bit(id);
    return (context->discarded_ids[bit / 32u] & (UINT32_C(1) << (bit % 32u)))
        != 0;
}

/* Past the bookkeeping limits the artwork degrades to unclipped, as it
   rendered before clipping support, rather than failing the image. */
static SvgClipUse *svg_clip_use(SvgViewportParser *context, SvgClipUse *parent)
{
    if (context->clip_count == SVG_CLIP_LIMIT) return NULL;
    SvgClipUse *use = svg_malloc(sizeof(*use));
    if (use == NULL) { context->failed = true; return NULL; }
    memset(use, 0, sizeof(*use));
    use->parent = parent;
    context->clip_count++;
    return use;
}

static bool svg_clip_local_id(const char *value, char id[64])
{
    if (value == NULL || strncmp(value, "url(", 4) != 0) return false;
    value += 4;
    while (*value == ' ' || *value == '\t') value++;
    char quote = *value == '\'' || *value == '"' ? *value++ : 0;
    if (*value++ != '#') return false;
    size_t length = 0;
    while (*value != '\0' && *value != ')' && *value != quote
           && *value != ' ' && *value != '\t') {
        if (length == 63) return false;
        id[length++] = *value++;
    }
    if (quote != 0 && *value++ != quote) return false;
    while (*value == ' ' || *value == '\t') value++;
    if (*value != ')' || length == 0) return false;
    id[length] = '\0';
    return true;
}

static void svg_viewport_start(void *opaque, const char *name,
                                const char **attributes)
{
    SvgViewportParser *context = opaque;
    if (context->failed) return;
    if (context->element_depth == NSVG_MAX_ATTR) {
        context->failed = true;
        svg_limit_refused = true;
        return;
    }
    SvgClipUse *clip = context->element_depth != 0
        ? context->clip_stack[context->element_depth - 1] : NULL;
    context->clip_stack[context->element_depth++] = clip;
    if (context->discard_depth != 0) return;
    if (context->clip_parser != NULL) {
        if (strcmp(name, "clipPath") == 0 || strcmp(name, "mask") == 0) {
            context->failed = true; return;
        }
        nsvg__startElement(context->clip_parser, name, attributes);
        return;
    }
    if (strcmp(name, "clipPath") == 0 || strcmp(name, "mask") == 0) {
        if (context->definition_count == SVG_CLIP_LIMIT) {
            svg_clip_note_discarded(
                context, svg_clip_attribute(attributes, "id"));
            context->discard_depth = context->element_depth;
            return;
        }
        SvgClipDefinition *definition = svg_malloc(sizeof(*definition));
        NSVGparser *parser = nsvg__createParser();
        if (definition == NULL || parser == NULL) {
            if (parser != NULL) nsvg__deleteParser(parser);
            context->failed = true;
            return;
        }
        memset(definition, 0, sizeof(*definition));
        definition->luminance_mask = strcmp(name, "mask") == 0;
        if (definition->luminance_mask) {
            const char *type = svg_clip_attribute(attributes, "mask-type");
            definition->alpha_mask = type != NULL && strcmp(type, "alpha") == 0;
            const char *units = svg_clip_attribute(attributes, "maskUnits");
            definition->region_object_space = units == NULL
                || strcmp(units, "userSpaceOnUse") != 0;
            static const char *const names[] = {"x", "y", "width", "height"};
            for (unsigned i = 0; i < 4; i++) {
                float basis = definition->region_object_space ? 1.0f
                    : (i % 2 == 0 ? context->parser->viewWidth
                                  : context->parser->viewHeight);
                const char *value = svg_clip_attribute(attributes, names[i]);
                definition->region[i] = nsvg__parseCoordinate(context->parser,
                    value != NULL ? value : i < 2 ? "-10%" : "120%", 0, basis);
            }
        }
        const char *id = svg_clip_attribute(attributes, "id");
        if (id != NULL && strlen(id) < sizeof(definition->id))
            memcpy(definition->id, id, strlen(id) + 1);
        const char *units = svg_clip_attribute(attributes,
            definition->luminance_mask ? "maskContentUnits" : "clipPathUnits");
        definition->object_space = units != NULL
            && strcmp(units, "objectBoundingBox") == 0;
        definition->next = context->definitions;
        context->definitions = definition;
        context->definition_count++;
        context->clip_parser = parser;
        parser->dpi = 96;
        parser->viewWidth = context->parser->viewWidth;
        parser->viewHeight = context->parser->viewHeight;
        nsvg__startElement(parser, "g", attributes);
        return;
    }
    NSVGparser *p = context->parser;
    NSVGshape *tail = p->shapesTail;
    float parent_transform[6];
    memcpy(parent_transform, nsvg__getAttr(p)->xform, sizeof(parent_transform));
    bool nested = strcmp(name, "svg") == 0 && context->depth != 0;
    float rectangle[4] = {0, 0, p->viewWidth, p->viewHeight};
    if (nested) {
        for (unsigned i = 0; attributes[i] != NULL; i += 2) {
            int coordinate = strcmp(attributes[i], "x") == 0 ? 0
                : strcmp(attributes[i], "y") == 0 ? 1
                : strcmp(attributes[i], "width") == 0 ? 2
                : strcmp(attributes[i], "height") == 0 ? 3 : -1;
            if (coordinate >= 0)
                rectangle[coordinate] = nsvg__parseCoordinate(p,
                    attributes[i + 1], 0,
                    coordinate % 2 == 0 ? p->viewWidth : p->viewHeight);
        }
        const char *transform = svg_clip_attribute(attributes, "transform");
        if (transform != NULL) {
            float local[6];
            nsvg__parseTransform(local, transform);
            nsvg__xformPremultiply(parent_transform, local);
        }
    }
    svg_viewport_start_native(context, name, attributes);
    if (context->failed) return;
    NSVGshape *first = tail != NULL ? tail->next : p->image->shapes;
    const char *overflow = svg_clip_attribute(attributes, "overflow");
    if (nested && (overflow == NULL || strncmp(overflow, "visible", 7) != 0)) {
        SvgClipUse *use = svg_clip_use(context, clip);
        if (context->failed) return;
        if (use != NULL) {
            use->viewport = true;
            memcpy(use->rectangle, rectangle, sizeof(rectangle));
            memcpy(use->transform, parent_transform, sizeof(parent_transform));
            clip = use;
        }
    }
    const char *reference = svg_clip_attribute(attributes, "clip-path");
    char reference_id[64];
    if (svg_clip_local_id(reference, reference_id)) {
        SvgClipUse *use = svg_clip_use(context, clip);
        if (context->failed) return;
        if (use != NULL) {
            memcpy(use->id, reference_id, strlen(reference_id) + 1);
            memcpy(use->transform, first != NULL ? first->xform
                   : nsvg__getAttr(p)->xform, sizeof(use->transform));
            clip = use;
        }
    }
    reference = svg_clip_attribute(attributes, "mask");
    if (svg_clip_local_id(reference, reference_id)) {
        SvgClipUse *use = svg_clip_use(context, clip);
        if (context->failed) return;
        if (use != NULL) {
            use->mask = true;
            memcpy(use->id, reference_id, strlen(reference_id) + 1);
            memcpy(use->transform, first != NULL ? first->xform
                   : nsvg__getAttr(p)->xform, sizeof(use->transform));
            clip = use;
        }
    }
    context->clip_stack[context->element_depth - 1] = clip;
    for (NSVGshape *shape = first; shape != NULL && clip != NULL; shape = shape->next) {
        if ((shape->flags & NSVG_FLAGS_VISIBLE) == 0) continue;
        if (context->binding_count == SVG_CLIP_SHAPE_LIMIT) break;
        if (context->binding_count == context->binding_capacity) {
            size_t capacity = context->binding_capacity != 0
                ? context->binding_capacity * 2 : 16;
            SvgClipBinding *bindings = svg_realloc(context->bindings,
                capacity * sizeof(*bindings));
            if (bindings == NULL) { context->failed = true; return; }
            context->bindings = bindings;
            context->binding_capacity = capacity;
        }
        context->bindings[context->binding_count++] = (SvgClipBinding) {shape, clip};
    }
}

static void svg_viewport_end(void *opaque, const char *name)
{
    SvgViewportParser *context = opaque;
    if (context->failed) return;
    if (context->element_depth == 0) { context->failed = true; return; }
    if (context->discard_depth != 0) {
        if (context->element_depth == context->discard_depth)
            context->discard_depth = 0;
        context->element_depth--;
        return;
    }
    if (context->clip_parser != NULL) {
        if (strcmp(name, "clipPath") == 0 || strcmp(name, "mask") == 0) {
            NSVGparser *parser = context->clip_parser;
            nsvg__endElement(parser, "g");
            context->definitions->image = parser->image;
            parser->image = NULL;
            nsvg__deleteParser(parser);
            context->clip_parser = NULL;
        } else nsvg__endElement(context->clip_parser, name);
    } else svg_viewport_end_native(context, name);
    context->element_depth--;
}

static void svg_viewport_content(void *opaque, const char *text)
{
    SvgViewportParser *context = opaque;
    if (!context->failed && context->discard_depth == 0) nsvg__content(context->clip_parser != NULL
        ? context->clip_parser : context->parser, text);
}

static void svg_clip_root_transform(SvgViewportParser *context, NSVGparser *p)
{
    float bounds[4];
    nsvg__imageBounds(p, bounds);
    float width = p->viewWidth > 0 ? p->viewWidth
        : (p->image->width > 0 ? p->image->width : bounds[2] - bounds[0]);
    float height = p->viewHeight > 0 ? p->viewHeight
        : (p->image->height > 0 ? p->image->height : bounds[3] - bounds[1]);
    float output_width = p->image->width > 0 ? p->image->width : width;
    float output_height = p->image->height > 0 ? p->image->height : height;
    float sx = width > 0 ? output_width / width : 0;
    float sy = height > 0 ? output_height / height : 0;
    if (p->alignType == NSVG_ALIGN_MEET) sx = sy = nsvg__minf(sx, sy);
    else if (p->alignType == NSVG_ALIGN_SLICE) sx = sy = nsvg__maxf(sx, sy);
    float min_x = p->viewWidth == 0 && p->image->width == 0 ? bounds[0] : p->viewMinx;
    float min_y = p->viewHeight == 0 && p->image->height == 0 ? bounds[1] : p->viewMiny;
    context->root_transform[0] = sx;
    context->root_transform[3] = sy;
    context->root_transform[4] = -min_x * sx
        + nsvg__viewAlign(width * sx, output_width, p->alignX);
    context->root_transform[5] = -min_y * sy
        + nsvg__viewAlign(height * sy, output_height, p->alignY);
}

static NSVGimage *svg_parse_viewports(char *source, Budget *budget)
{
    SvgViewportParser *context = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*context));
    if (context == NULL) {
        svg_allocation_failed = true;
        return NULL;
    }
    svg_clips = context;
    NSVGparser *p = nsvg__createParser();
    context->parser = p;
    NSVGimage *result = NULL;
    if (p != NULL) {
        p->dpi = 96;
        nsvg__parseXML(source, svg_viewport_start, svg_viewport_end,
                       svg_viewport_content, context);
        if (!context->failed && context->depth == 0
            && context->element_depth == 0 && context->clip_parser == NULL) {
            nsvg__createGradients(p);
            svg_clip_root_transform(context, p);
            nsvg__scaleToViewbox(p, "px");
            result = p->image;
            p->image = NULL;
        }
        nsvg__deleteParser(p);
    }
    return result;
}

static bool svg_raster_coverage(void *opaque, unsigned char *coverage,
                                 int first, int last, int row)
{
    if (svg_clips == NULL) return true;
    NSVGrasterizer *r = opaque;
    if (svg_clips->mask_capture) {
        row += svg_clips->capture_row;
        unsigned char *destination = svg_clips->scratch_mask
            + (size_t) row * (size_t) r->width;
        int *rect = svg_clips->scratch_rect;
        if (first < rect[0]) rect[0] = first;
        if (row < rect[1]) rect[1] = row;
        if (last + 1 > rect[2]) rect[2] = last + 1;
        if (row + 1 > rect[3]) rect[3] = row + 1;
        for (int x = first; x <= last; x++) {
            if (svg_clips->luminance_capture) {
                unsigned alpha = (unsigned) nsvg__div255(
                    coverage[x] * svg_clips->mask_opacity);
                destination[x] = (unsigned char) nsvg__div255(
                    alpha * svg_clips->mask_luminance
                    + (255u - alpha) * destination[x]);
            } else if (coverage[x] > destination[x]) destination[x] = coverage[x];
        }
        return false;
    }
    const SvgClipMask *active = &svg_clips->masks[svg_clips->active_mask];
    if (svg_clips->mask_active && !active->identity) {
        const int *rect = active->rect;
        const unsigned char *mask = active->pixels
            + (size_t) row * (size_t) r->width;
        bool inside = row >= rect[1] && row < rect[3];
        for (int x = first; x <= last; x++)
            coverage[x] = inside && x >= rect[0] && x < rect[2]
                ? (unsigned char) nsvg__div255(coverage[x] * mask[x]) : 0;
    }
    return true;
}

static bool svg_clip_raster_shape(NSVGrasterizer *r, const NSVGshape *source,
                                    float *transform, float scale)
{
    NSVGshape shape = *source;
    shape.paths = NULL;
    NSVGpath **tail = &shape.paths;
    for (const NSVGpath *path = source->paths; path != NULL; path = path->next) {
        NSVGpath *copy = nsvgDuplicatePath((NSVGpath *) path);
        if (copy == NULL) return false;
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
        for (int i = 0; i < copy->npts; i++) {
            float x = copy->pts[i * 2], y = copy->pts[i * 2 + 1];
            nsvg__xformPoint(&copy->pts[i * 2], &copy->pts[i * 2 + 1],
                            x, y, transform);
            if (!isfinite(copy->pts[i * 2]) || !isfinite(copy->pts[i * 2 + 1]))
                return false; /* Scratch-owner rollback releases the paths. */
        }
    }
    nsvg__resetPool(r);
    r->freelist = NULL;
    r->nedges = 0;
    nsvg__flattenShape(r, &shape, scale);
    for (int i = 0; i < r->nedges; i++) {
        r->edges[i].y0 *= NSVG__SUBSAMPLES;
        r->edges[i].y1 *= NSVG__SUBSAMPLES;
    }
    if (r->nedges != 0)
        qsort(r->edges, (size_t) r->nedges, sizeof(NSVGedge), nsvg__cmpEdge);
    /* Rows outside the edges' vertical extent produce no coverage. Shift
       the sorted edges by whole rows so the scanline loop visits only the
       covered band; integer shifts of values below 2^23 are exact, so the
       coverage is unchanged. */
    int full_height = r->height, row0 = 0, row1 = 0;
    if (r->nedges != 0) {
        float top = r->edges[0].y0, bottom = r->edges[0].y1;
        for (int i = 1; i < r->nedges; i++)
            if (r->edges[i].y1 > bottom) bottom = r->edges[i].y1;
        const float limit = 8388608.0f;
        float rows = (float) full_height;
        if (top >= -limit && bottom <= limit) {
            float first = floorf(top / NSVG__SUBSAMPLES) - 1.0f;
            float last = ceilf(bottom / NSVG__SUBSAMPLES) + 1.0f;
            row0 = first <= 0.0f ? 0 : first >= rows ? full_height : (int) first;
            row1 = last <= 0.0f ? 0 : last >= rows ? full_height : (int) last;
        } else row1 = full_height;
    }
    if (row1 > row0) {
        float shift = (float) (row0 * NSVG__SUBSAMPLES);
        for (int i = 0; shift != 0.0f && i < r->nedges; i++) {
            r->edges[i].y0 -= shift;
            r->edges[i].y1 -= shift;
        }
        NSVGpaint white = {.type = NSVG_PAINT_COLOR, .color = 0xffffffffu};
        NSVGcachedPaint paint;
        nsvg__initPaint(&paint, &white, 1);
        r->height = row1 - row0;
        svg_clips->capture_row = row0;
        nsvg__rasterizeSortedEdges(r, 0, 0, scale, &paint, shape.fillRule);
        svg_clips->capture_row = 0;
        r->height = full_height;
    }
    while (shape.paths != NULL) {
        NSVGpath *next = shape.paths->next;
        svg_free(shape.paths->pts);
        svg_free(shape.paths);
        shape.paths = next;
    }
    return !svg_allocation_failed;
}

/* Shape bounds are already transformed into the root viewport. A clip's
   object bounding box instead belongs to its own user coordinate system:
   recover the actual cubic bounds there, not an inverse of an axis-aligned
   root-space rectangle (which is too large after rotation). */
static bool svg_clip_object_transform(SvgViewportParser *context,
                                      SvgClipUse *use, float transform[6])
{
    /* Every caller passes the use's own transform under the fixed root
       transform, so the result is a property of the use. */
    if (use->object_state != 0) {
        if (use->object_state == 1)
            memcpy(transform, use->object_transform, sizeof(use->object_transform));
        return use->object_state == 1;
    }
    use->object_state = 2;
    float determinant = transform[0] * transform[3]
                        - transform[1] * transform[2];
    if (!isfinite(determinant) || fabsf(determinant) < 1e-6f) return false;
    float inverse[6], bounds[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    nsvg__xformInverse(inverse, transform);
    for (size_t i = 0; i < context->binding_count; i++) {
        SvgClipUse *ancestor = context->bindings[i].clip;
        for (unsigned d = 0; ancestor != NULL && d < SVG_VIEWPORT_DEPTH_LIMIT;
             d++, ancestor = ancestor->parent) {
            if (ancestor != use) continue;
            for (NSVGpath *path = context->bindings[i].shape->paths;
                 path != NULL; path = path->next) {
                for (int at = 0; at + 3 < path->npts; at += 3) {
                    float curve[8], b[4];
                    for (int point = 0; point < 4; point++)
                        nsvg__xformPoint(&curve[point * 2], &curve[point * 2 + 1],
                            path->pts[(at + point) * 2],
                            path->pts[(at + point) * 2 + 1], inverse);
                    nsvg__curveBounds(b, curve);
                    bounds[0] = nsvg__minf(bounds[0], b[0]);
                    bounds[1] = nsvg__minf(bounds[1], b[1]);
                    bounds[2] = nsvg__maxf(bounds[2], b[2]);
                    bounds[3] = nsvg__maxf(bounds[3], b[3]);
                }
            }
            break;
        }
    }
    if (!isfinite(bounds[0]) || bounds[2] <= bounds[0]
        || !isfinite(bounds[1]) || bounds[3] <= bounds[1]) return false;
    float box[6] = {bounds[2] - bounds[0], 0, 0,
                   bounds[3] - bounds[1], bounds[0], bounds[1]};
    nsvg__xformMultiply(box, transform);
    memcpy(transform, box, sizeof(box));
    memcpy(use->object_transform, box, sizeof(box));
    use->object_state = 1;
    return true;
}

static void svg_clip_scratch_clear(SvgViewportParser *context, int width)
{
    int *rect = context->scratch_rect;
    for (int y = rect[1]; y < rect[3]; y++)
        memset(context->scratch_mask + (size_t) y * (size_t) width + (size_t) rect[0],
               0, (size_t) (rect[2] - rect[0]));
    rect[0] = rect[1] = INT_MAX;
    rect[2] = rect[3] = INT_MIN;
}

/* mask *= scratch, restricted to where both can be nonzero. */
static void svg_clip_mask_multiply(SvgViewportParser *context,
                                   SvgClipMask *target, int width)
{
    const int *s = context->scratch_rect;
    int *m = target->rect;
    int x0 = s[0] > m[0] ? s[0] : m[0], y0 = s[1] > m[1] ? s[1] : m[1];
    int x1 = s[2] < m[2] ? s[2] : m[2], y1 = s[3] < m[3] ? s[3] : m[3];
    if (x0 >= x1 || y0 >= y1) x0 = y0 = x1 = y1 = 0;
    for (int y = y0; y < y1; y++) {
        size_t at = (size_t) y * (size_t) width;
        unsigned char *mask = target->pixels + at;
        const unsigned char *scratch = context->scratch_mask + at;
        /* div255(255 * s) == s exactly for every byte s. */
        if (target->identity)
            memcpy(mask + x0, scratch + x0, (size_t) (x1 - x0));
        else for (int x = x0; x < x1; x++)
            mask[x] = (unsigned char) nsvg__div255(mask[x] * scratch[x]);
    }
    m[0] = x0; m[1] = y0; m[2] = x1; m[3] = y1;
    target->identity = false;
}

static const SvgClipDefinition *svg_clip_definition(
    const SvgViewportParser *context, const char *id)
{
    const SvgClipDefinition *definition = context->definitions;
    while (definition != NULL && strcmp(definition->id, id) != 0)
        definition = definition->next;
    return definition;
}

/* Exported artwork repeats one clip on many sibling groups, each with its
   own use. Uses with identical fields and ancestry produce the same mask
   unless an object bounding box ties it to the bound shapes. */
static bool svg_clip_equivalent(const SvgViewportParser *context,
                                const SvgClipUse *a, const SvgClipUse *b)
{
    for (unsigned depth = 0; depth <= SVG_VIEWPORT_DEPTH_LIMIT; depth++) {
        if (a == b) return true;
        if (a == NULL || b == NULL || a->viewport != b->viewport
            || a->mask != b->mask || strcmp(a->id, b->id) != 0
            || memcmp(a->transform, b->transform, sizeof(a->transform)) != 0
            || memcmp(a->rectangle, b->rectangle, sizeof(a->rectangle)) != 0)
            return false;
        if (!a->viewport) {
            const SvgClipDefinition *definition = svg_clip_definition(context, a->id);
            if (definition != NULL && (definition->object_space
                || (definition->luminance_mask
                    && definition->region_object_space))) return false;
        }
        a = a->parent;
        b = b->parent;
    }
    return false;
}

static void svg_raster_shape(void *opaque, int width, int height, float scale)
{
    SvgViewportParser *context = svg_clips;
    if (context == NULL) return;
    context->mask_active = false;
    if (context->binding_cursor == context->binding_count
        || context->bindings[context->binding_cursor].shape != opaque) return;
    SvgClipUse *clip = context->bindings[context->binding_cursor++].clip;
    for (unsigned i = 0; i < 2; i++) {
        unsigned slot = (context->active_mask + i) & 1u;
        SvgClipMask *cached = &context->masks[slot];
        if (cached->clip == NULL
            || !svg_clip_equivalent(context, cached->clip, clip)) continue;
        cached->clip = clip;
        context->active_mask = slot;
        context->mask_active = true;
        return;
    }
    size_t bytes = (size_t) width * (size_t) height;
    if (context->mask_bytes == 0) {
        context->masks[0].pixels = svg_malloc(bytes);
        context->scratch_mask = svg_malloc(bytes);
        if (context->masks[0].pixels == NULL || context->scratch_mask == NULL)
            return;
        context->mask_bytes = bytes;
        memset(context->scratch_mask, 0, bytes);
        context->scratch_rect[0] = context->scratch_rect[1] = INT_MAX;
        context->scratch_rect[2] = context->scratch_rect[3] = INT_MIN;
    }
    unsigned target_slot = context->active_mask;
    if (context->masks[target_slot].clip != NULL) {
        /* Keep the current mask as the alternate. A refused or unaffordable
           second buffer only means rebuilding in place, never a failure. */
        SvgClipMask *other = &context->masks[target_slot ^ 1u];
        if (other->pixels == NULL && !context->second_mask_refused) {
            if (bytes <= budget_remaining(svg_budget) / 4u)
                other->pixels = budget_malloc_category(
                    svg_budget, BUDGET_CATEGORY_RESOURCE, bytes);
            context->second_mask_refused = other->pixels == NULL;
        }
        if (other->pixels != NULL) target_slot ^= 1u;
    }
#ifndef __PSP__
    tilefinch_test_faults()->svg_clip_mask_builds++;
#endif
    SvgClipMask *target = &context->masks[target_slot];
    target->clip = NULL;
    target->identity = true;
    target->rect[0] = target->rect[1] = 0;
    target->rect[2] = width;
    target->rect[3] = height;
    context->active_mask = target_slot;
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if (r == NULL) return;
    r->width = width;
    r->height = height;
    r->scanline = svg_malloc((size_t) width);
    r->cscanline = width;
    if (r->scanline == NULL) { nsvgDeleteRasterizer(r); return; }
    context->mask_capture = true;
    unsigned depth = 0;
    for (SvgClipUse *use = clip; use != NULL; use = use->parent) {
        if (++depth > SVG_VIEWPORT_DEPTH_LIMIT) {
            svg_allocation_failed = true;
            break;
        }
        svg_cooperate();
        svg_clip_scratch_clear(context, width);
        bool identity = false;
        float transform[6];
        memcpy(transform, use->transform, sizeof(transform));
        nsvg__xformPremultiply(transform, context->root_transform);
        bool okay = true;
        if (use->viewport) {
            float x = use->rectangle[0], y = use->rectangle[1];
            float w = use->rectangle[2], h = use->rectangle[3];
            float points[] = {x,y, x,y, x+w,y, x+w,y,
                x+w,y, x+w,y+h, x+w,y+h,
                x+w,y+h, x,y+h, x,y+h, x,y+h, x,y, x,y};
            NSVGpath path = {.pts = points, .npts = 13, .closed = 1};
            NSVGshape shape = {.paths = &path, .fillRule = NSVG_FILLRULE_NONZERO};
            if (w > 0 && h > 0)
                okay = svg_clip_raster_shape(r, &shape, transform, scale);
        } else {
            const SvgClipDefinition *definition = svg_clip_definition(context, use->id);
            if (definition != NULL && definition->image != NULL
                && definition->luminance_mask == use->mask) {
                bool has_area = !definition->object_space
                    || svg_clip_object_transform(context, use, transform);
                for (NSVGshape *shape = definition->image->shapes;
                     has_area && okay && shape != NULL; shape = shape->next) {
                    context->luminance_capture = definition->luminance_mask;
                    if (definition->luminance_mask) {
                        /* Solid-color luminance masks share the existing
                           bounded coverage buffers; no RGBA surface. Complex
                           paints/strokes are refused rather than silently
                           treated as an opaque clipping silhouette. */
                        if (shape->stroke.type != NSVG_PAINT_NONE
                            || (shape->fill.type != NSVG_PAINT_COLOR
                                && shape->fill.type != NSVG_PAINT_NONE)) {
                            okay = false;
                            break;
                        }
                        if (shape->fill.type == NSVG_PAINT_NONE) continue;
                        unsigned color = shape->fill.color;
                        float red = (float) (color & 255u) / 255.0f;
                        float green = (float) ((color >> 8) & 255u) / 255.0f;
                        float blue = (float) ((color >> 16) & 255u) / 255.0f;
                        red = red <= .04045f ? red / 12.92f
                            : powf((red + .055f) / 1.055f, 2.4f);
                        green = green <= .04045f ? green / 12.92f
                            : powf((green + .055f) / 1.055f, 2.4f);
                        blue = blue <= .04045f ? blue / 12.92f
                            : powf((blue + .055f) / 1.055f, 2.4f);
                        context->mask_luminance = definition->alpha_mask ? 255u
                            : (unsigned) (
                            (red * .2126f + green * .7152f + blue * .0722f)
                            * 255.0f + .5f);
                        context->mask_opacity = (unsigned) (
                            (float) (color >> 24) * shape->opacity + .5f);
                    }
                    if ((shape->flags & NSVG_FLAGS_VISIBLE) != 0)
                        okay = svg_clip_raster_shape(r, shape, transform, scale);
                }
                context->luminance_capture = false;
                if (okay && definition->luminance_mask) {
                    svg_clip_mask_multiply(context, target, width);
                    svg_clip_scratch_clear(context, width);
                    memcpy(transform, use->transform, sizeof(transform));
                    nsvg__xformPremultiply(transform, context->root_transform);
                    bool area = !definition->region_object_space
                        || svg_clip_object_transform(context, use, transform);
                    float x = definition->region[0], y = definition->region[1];
                    float w = definition->region[2], h = definition->region[3];
                    float points[] = {x,y, x,y, x+w,y, x+w,y,
                        x+w,y, x+w,y+h, x+w,y+h,
                        x+w,y+h, x,y+h, x,y+h, x,y+h, x,y, x,y};
                    NSVGpath path = {.pts = points, .npts = 13, .closed = 1};
                    NSVGshape rectangle = {.paths = &path,
                        .fillRule = NSVG_FILLRULE_NONZERO};
                    if (area && w > 0 && h > 0)
                        okay = svg_clip_raster_shape(r, &rectangle, transform, scale);
                }
            }
            /* An unresolved local clip is ignored, as an invalid URL value,
               rather than hiding otherwise valid artwork; so is a mask whose
               definition fell past our definition limit. */
            else if (!use->mask || svg_clip_discarded(context, use->id))
                identity = true;
        }
        if (!okay) { svg_allocation_failed = true; break; }
        if (!identity) svg_clip_mask_multiply(context, target, width);
    }
    context->mask_capture = false;
    nsvgDeleteRasterizer(r);
    target->clip = clip;
    context->mask_active = true;
}
