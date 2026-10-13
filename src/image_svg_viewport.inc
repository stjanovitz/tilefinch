/* Keep nested SVG viewports and local clipping in the shared decoder. */
#define SVG_VIEWPORT_DEPTH_LIMIT 32u
#define SVG_CLIP_LIMIT 128u
#define SVG_CLIP_SHAPE_LIMIT 4096u

typedef struct SvgClipDefinition {
    char id[64];
    NSVGimage *image;
    bool object_space;
    struct SvgClipDefinition *next;
} SvgClipDefinition;

typedef struct SvgClipUse {
    struct SvgClipUse *parent;
    char id[64];
    float transform[6];
    float rectangle[4];
    bool viewport;
} SvgClipUse;

typedef struct {
    NSVGshape *shape;
    SvgClipUse *clip;
} SvgClipBinding;

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
    NSVGparser *clip_parser;
    SvgClipDefinition *definitions;
    SvgClipBinding *bindings;
    size_t binding_count, binding_capacity, binding_cursor;
    float root_transform[6];
    unsigned char *mask, *scratch_mask;
    size_t mask_bytes;
    SvgClipUse *cached_clip;
    bool mask_active, mask_capture;
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

static SvgClipUse *svg_clip_use(SvgViewportParser *context, SvgClipUse *parent)
{
    if (context->clip_count == SVG_CLIP_LIMIT) {
        context->failed = true;
        return NULL;
    }
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
        return;
    }
    SvgClipUse *clip = context->element_depth != 0
        ? context->clip_stack[context->element_depth - 1] : NULL;
    context->clip_stack[context->element_depth++] = clip;
    if (context->clip_parser != NULL) {
        if (strcmp(name, "clipPath") == 0) { context->failed = true; return; }
        nsvg__startElement(context->clip_parser, name, attributes);
        return;
    }
    if (strcmp(name, "clipPath") == 0) {
        if (context->definition_count == SVG_CLIP_LIMIT) {
            context->failed = true;
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
        const char *id = svg_clip_attribute(attributes, "id");
        if (id != NULL && strlen(id) < sizeof(definition->id))
            memcpy(definition->id, id, strlen(id) + 1);
        const char *units = svg_clip_attribute(attributes, "clipPathUnits");
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
        if (use == NULL) return;
        use->viewport = true;
        memcpy(use->rectangle, rectangle, sizeof(rectangle));
        memcpy(use->transform, parent_transform, sizeof(parent_transform));
        clip = use;
    }
    const char *reference = svg_clip_attribute(attributes, "clip-path");
    char reference_id[64];
    if (svg_clip_local_id(reference, reference_id)) {
        SvgClipUse *use = svg_clip_use(context, clip);
        if (use == NULL) return;
        memcpy(use->id, reference_id, strlen(reference_id) + 1);
        memcpy(use->transform, first != NULL ? first->xform
               : nsvg__getAttr(p)->xform, sizeof(use->transform));
        clip = use;
    }
    context->clip_stack[context->element_depth - 1] = clip;
    for (NSVGshape *shape = first; shape != NULL && clip != NULL; shape = shape->next) {
        if ((shape->flags & NSVG_FLAGS_VISIBLE) == 0) continue;
        if (context->binding_count == SVG_CLIP_SHAPE_LIMIT) {
            context->failed = true;
            return;
        }
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
    if (context->clip_parser != NULL) {
        if (strcmp(name, "clipPath") == 0) {
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
    if (!context->failed) nsvg__content(context->clip_parser != NULL
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
    if (context == NULL) return NULL;
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
        unsigned char *destination = svg_clips->scratch_mask
            + (size_t) row * (size_t) r->width;
        for (int x = first; x <= last; x++)
            if (coverage[x] > destination[x]) destination[x] = coverage[x];
        return false;
    }
    if (svg_clips->mask_active) {
        const unsigned char *mask = svg_clips->mask
            + (size_t) row * (size_t) r->width;
        for (int x = first; x <= last; x++)
            coverage[x] = (unsigned char) nsvg__div255(coverage[x] * mask[x]);
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
    NSVGpaint white = {.type = NSVG_PAINT_COLOR, .color = 0xffffffffu};
    NSVGcachedPaint paint;
    nsvg__initPaint(&paint, &white, 1);
    nsvg__rasterizeSortedEdges(r, 0, 0, scale, &paint, shape.fillRule);
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
    return true;
}

static void svg_raster_shape(void *opaque, int width, int height, float scale)
{
    SvgViewportParser *context = svg_clips;
    if (context == NULL) return;
    context->mask_active = false;
    if (context->binding_cursor == context->binding_count
        || context->bindings[context->binding_cursor].shape != opaque) return;
    SvgClipUse *clip = context->bindings[context->binding_cursor++].clip;
    if (context->cached_clip == clip) { context->mask_active = true; return; }
    size_t bytes = (size_t) width * (size_t) height;
    if (context->mask_bytes == 0) {
        context->mask = svg_malloc(bytes);
        context->scratch_mask = svg_malloc(bytes);
        if (context->mask == NULL || context->scratch_mask == NULL) return;
        context->mask_bytes = bytes;
    }
    memset(context->mask, 255, bytes);
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
        memset(context->scratch_mask, 0, bytes);
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
            SvgClipDefinition *definition = context->definitions;
            while (definition != NULL && strcmp(definition->id, use->id) != 0)
                definition = definition->next;
            if (definition != NULL && definition->image != NULL) {
                bool has_area = !definition->object_space
                    || svg_clip_object_transform(context, use, transform);
                for (NSVGshape *shape = definition->image->shapes;
                     has_area && okay && shape != NULL; shape = shape->next) {
                    if ((shape->flags & NSVG_FLAGS_VISIBLE) != 0)
                        okay = svg_clip_raster_shape(r, shape, transform, scale);
                }
            }
            /* An unresolved local clip is ignored, as an invalid URL value,
               rather than hiding otherwise valid artwork. */
            else memset(context->scratch_mask, 255, bytes);
        }
        if (!okay) { svg_allocation_failed = true; break; }
        for (size_t i = 0; i < bytes; i++)
            context->mask[i] = (unsigned char) nsvg__div255(
                context->mask[i] * context->scratch_mask[i]);
    }
    context->mask_capture = false;
    nsvgDeleteRasterizer(r);
    context->cached_clip = clip;
    context->mask_active = true;
}
