/* NanoSVG's ordinary entry treats every <svg> as the document root. Keep
   root dimensions stable and compose nested viewport transforms instead.
   Like the underlying decoder this does not implement SVG clipping paths. */
#define SVG_VIEWPORT_DEPTH_LIMIT 32u

typedef struct {
    float min_x, min_y, width, height;
    int align_x, align_y, align_type;
} SvgViewportState;

typedef struct {
    NSVGparser *parser;
    SvgViewportState states[SVG_VIEWPORT_DEPTH_LIMIT];
    unsigned depth;
    bool failed;
} SvgViewportParser;

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

static void svg_viewport_start(void *opaque, const char *name,
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

static void svg_viewport_end(void *opaque, const char *name)
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

static void svg_viewport_content(void *opaque, const char *text)
{
    SvgViewportParser *context = opaque;
    if (!context->failed) nsvg__content(context->parser, text);
}

static NSVGimage *svg_parse_viewports(char *source, Budget *budget)
{
    SvgViewportParser *context = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*context));
    if (context == NULL) return NULL;
    NSVGparser *p = nsvg__createParser();
    context->parser = p;
    NSVGimage *result = NULL;
    if (p != NULL) {
        p->dpi = 96;
        nsvg__parseXML(source, svg_viewport_start, svg_viewport_end,
                       svg_viewport_content, context);
        if (!context->failed && context->depth == 0) {
            nsvg__createGradients(p);
            nsvg__scaleToViewbox(p, "px");
            result = p->image;
            p->image = NULL;
        }
        nsvg__deleteParser(p);
    }
    budget_free(budget, context);
    return result;
}
