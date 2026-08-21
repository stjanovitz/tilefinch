static bool style_property_display(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[48];
        if (style_resolve_value(sheet, value, value_length, resolved, sizeof(resolved), 0)) {
            bool recognized = true;
            if (strcmp(resolved, "none") == 0) style->display = DISPLAY_NONE;
            else if (strcmp(resolved, "table") == 0) {
                style->display = DISPLAY_TABLE;
            }
            else if (strcmp(resolved, "table-row") == 0) {
                style->display = DISPLAY_TABLE_ROW;
            }
            else if (strcmp(resolved, "table-cell") == 0) {
                style->display = DISPLAY_TABLE_CELL;
            }
            else if (strcmp(resolved, "table-column") == 0
                     || strcmp(resolved, "table-column-group") == 0) {
                style->display = DISPLAY_TABLE_COLUMN;
            }
            else if (strcmp(resolved, "table-row-group") == 0) {
                style->display = DISPLAY_TABLE_ROW_GROUP;
            }
            else if (strcmp(resolved, "table-header-group") == 0) {
                style->display = DISPLAY_TABLE_HEADER_GROUP;
                style->order = -100000;
                *parse->mask_high |= S2_ORDER;
            }
            else if (strcmp(resolved, "table-footer-group") == 0) {
                style->display = DISPLAY_TABLE_FOOTER_GROUP;
                style->order = 100000;
                *parse->mask_high |= S2_ORDER;
            }
            else if (strcmp(resolved, "contents") == 0) {
                style->display = DISPLAY_CONTENTS;
            }
            else if (strcmp(resolved, "inline-block") == 0) {
                style->display = DISPLAY_INLINE_BLOCK;
            }
            else if (strcmp(resolved, "inline-table") == 0) {
                /*
                 * The compact layout model has no separate inline-table
                 * enum. Its outer display behavior is nevertheless atomic,
                 * so retain that standards-visible half instead of silently
                 * falling back to an ordinary inline box.
                 */
                style->display = DISPLAY_INLINE_BLOCK;
            }
            else if (strcmp(resolved, "inline-flex") == 0
                     || strcmp(resolved, "-webkit-inline-box") == 0
                     || strcmp(resolved, "-webkit-inline-flex") == 0
                     || strcmp(resolved, "-ms-inline-flexbox") == 0
                     || strcmp(resolved, "-moz-inline-box") == 0) {
                style->display = DISPLAY_INLINE_FLEX;
            }
            else if (strcmp(resolved, "inline-grid") == 0) {
                style->display = DISPLAY_INLINE_GRID;
            }
            else if (strcmp(resolved, "flow-root") == 0) {
                style->display = DISPLAY_FLOW_ROOT;
            }
            else if (strcmp(resolved, "block") == 0
                     || strcmp(resolved, "list-item") == 0
                     || strcmp(resolved, "table-caption") == 0) {
                style->display = DISPLAY_BLOCK;
            }
            else if (strcmp(resolved, "flex") == 0
                     || strcmp(resolved, "-webkit-box") == 0
                     || strcmp(resolved, "-webkit-flex") == 0
                     || strcmp(resolved, "-ms-flexbox") == 0
                     || strcmp(resolved, "-moz-box") == 0) {
                style->display = DISPLAY_FLEX;
            }
            else if (strcmp(resolved, "grid") == 0) style->display = DISPLAY_GRID;
            else if (strcmp(resolved, "inline") == 0) {
                style->display = DISPLAY_INLINE;
            } else {
                recognized = false;
            }
            if (recognized) *mask |= S_DISPLAY;
        }
    
    return true;
}

static bool style_property_appearance(const StylePropertyParse *parse)
{
    char resolved[32];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    static const char *const names[] = {
        "none", "auto", "base", "base-select", "button", "checkbox",
        "listbox", "menulist-button", "meter", "progress-bar", "radio",
        "searchfield", "textarea", "textfield"
    };
    if (strcmp(resolved, "initial") == 0
        || strcmp(resolved, "unset") == 0
        || strcmp(resolved, "revert") == 0
        || strcmp(resolved, "revert-layer") == 0) {
        parse->style->appearance = (AppearanceMode) (
            parse->style->appearance & STYLE_JUSTIFY_SELF_MASK);
        *parse->mask_high |= S2_APPEARANCE;
        return true;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(resolved, names[i]) != 0) continue;
        parse->style->appearance = (AppearanceMode) (
            (parse->style->appearance & STYLE_JUSTIFY_SELF_MASK)
            | (uint8_t) i);
        *parse->mask_high |= S2_APPEARANCE;
        break;
    }
    return true;
}

static bool style_property_color(const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "initial")
        || span_case_equal(parse->value, parse->value_length, "revert")
        || span_case_equal(parse->value, parse->value_length,
                           "revert-layer")) {
        parse->style->color = 0x000000;
        parse->style->color_alpha = 255;
        *parse->mask |= S_COLOR;
        return true;
    }
    if (span_case_equal(parse->value, parse->value_length, "inherit")
        || span_case_equal(parse->value, parse->value_length, "unset")) {
        *parse->inherit_mask |= S_COLOR;
        *parse->mask |= S_COLOR;
        return true;
    }
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            if (style_parse_color_with_alpha(sheet, value, value_length,
                                   &style->color,
                                   &style->color_alpha)) {
            *mask |= S_COLOR;
        }
    
    return true;
}

static bool style_property_webkit_text_fill_color(
    const StylePropertyParse *parse)
{
    /* Tilefinch has one inherited glyph-paint colour.  Treat WebKit's text
       fill as that colour so transparent and gradient-backed headings do not
       disappear. A transparent fill is normally paired with
       `background-clip:text`; until gradient glyph masks are available,
       preserve the previously cascaded text colour instead of hiding the
       glyphs. This avoids another word in every PSP ComputedStyle. */
    if (span_case_equal(parse->value, parse->value_length, "transparent")) {
        return true;
    }
    return style_property_color(parse);
}

/* True when a top-level `background` shorthand token names a component this
   engine understands.  This decides only whether the value carried ANY
   recognisable component: a shorthand made entirely of things the engine
   cannot parse (an unsupported function such as linear-gradient(), or an
   unresolvable var()) is an invalid declaration, and CSS requires invalid
   declarations to be dropped at parse time so the previously cascaded
   background stays in force. */
static bool background_shorthand_token_known(const char *token, size_t length)
{
    static const char *const keywords[] = {
        "none", "repeat", "repeat-x", "repeat-y", "no-repeat", "space",
        "round", "scroll", "fixed", "local", "border-box", "padding-box",
        "content-box", "text", "cover", "contain", "auto", "left", "right",
        "top", "bottom", "center", "inherit", "initial", "unset", "revert",
        "revert-layer"
    };
    if (length == 0) return false;
    if (length == 1 && (token[0] == '/' || token[0] == ',')) return true;
    if (length >= 4 && strncasecmp(token, "url(", 4) == 0) return true;
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (span_case_equal(token, length, keywords[i])) return true;
    }
    /* A leading digit/sign/dot marks a <length>/<percentage> position or
       size component. */
    char c = token[0];
    return c == '-' || c == '+' || c == '.' || isdigit((unsigned char) c);
}

static bool background_shorthand_recognized(const char *resolved)
{
    size_t length = strlen(resolved);
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) resolved[at])) at++;
        size_t start = at;
        int depth = 0;
        char quote = 0;
        while (at < length) {
            char c = resolved[at];
            if (quote != 0) {
                if (c == quote) quote = 0;
                at++;
                continue;
            }
            if (c == '\'' || c == '"') { quote = c; at++; continue; }
            if (c == '(') depth++;
            else if (c == ')') { if (depth > 0) depth--; }
            else if (depth == 0 && isspace((unsigned char) c)) break;
            at++;
        }
        if (at > start
            && background_shorthand_token_known(resolved + start,
                                                at - start)) {
            return true;
        }
    }
    return false;
}

static bool style_replace_background_image_paint(
    Stylesheet *sheet, ComputedStyle *style,
    const StyleGradient *gradient, bool reset_geometry, bool *retained)
{
    if (retained != NULL) *retained = false;
    if (sheet == NULL || style == NULL) return false;
    StylePaintStack stack = style_paint_stack_copy(sheet, style);
    bool had_geometry = stack.background_count != 0;
    if (reset_geometry) {
        memset(stack.backgrounds, 0, sizeof(stack.backgrounds));
        stack.background_count = 0;
        stack.components &= (uint8_t)
            ~STYLE_PAINT_COMPONENT_BACKGROUND_BOX;
    } else {
        for (size_t i = 0; i < STYLE_PAINT_LAYER_LIMIT; i++) {
            stack.backgrounds[i].image = NULL;
            memset(&stack.backgrounds[i].gradient, 0,
                   sizeof(stack.backgrounds[i].gradient));
            stack.backgrounds[i].kind = STYLE_PAINT_IMAGE_NONE;
        }
    }
    stack.components &= (uint8_t)
        ~STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE;
    if (gradient != NULL) {
        if (stack.background_count == 0) stack.background_count = 1;
        stack.background_count = 1;
        stack.backgrounds[0].kind = STYLE_PAINT_IMAGE_GRADIENT;
        stack.backgrounds[0].gradient = *gradient;
        if (reset_geometry || !had_geometry) {
            stack.backgrounds[0].origin = STYLE_PAINT_BOX_PADDING;
            stack.backgrounds[0].clip = STYLE_PAINT_BOX_BORDER;
        }
        stack.components |= STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE;
    }
    return style_apply_paint_stack(sheet, style, &stack, retained);
}

static bool style_property_background(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
    uint64_t *mask_high = parse->mask_high;
            bool shorthand = span_equal(name, name_length, "background");
        bool image_property = shorthand
                              || span_equal(name, name_length,
                                            "background-image");
        bool color_property = shorthand
                              || span_equal(name, name_length,
                                            "background-color");
        if (span_case_equal(value, value_length, "initial")
            || span_case_equal(value, value_length, "unset")
            || span_case_equal(value, value_length, "revert")
            || span_case_equal(value, value_length, "revert-layer")) {
            if (image_property) {
                style->background_image = NULL;
                style->background_image_kind = STYLE_BACKGROUND_IMAGE_NONE;
                *mask |= S_BACKGROUND_IMAGE;
            }
            if (color_property) {
                style->has_background = false;
                style->background = 0;
                style->background_alpha = 0;
                *mask |= S_BACKGROUND;
            }
            return true;
        }
        if (span_case_equal(value, value_length, "inherit")) {
            if (image_property) *parse->inherit_mask |= S_BACKGROUND_IMAGE;
            if (color_property) *parse->inherit_mask |= S_BACKGROUND;
            if (image_property) *mask |= S_BACKGROUND_IMAGE;
            if (color_property) *mask |= S_BACKGROUND;
            return true;
        }
        bool multilayer = false;
        /* The shorthand resets every background sub-property, so it stages
           those resets in a scratch style and commits them only once some
           component of the value has actually been recognised.  Without
           this, the near-universal progressive-enhancement pattern
           `background:#ff9770` followed by `background:linear-gradient(...)`
           loses BOTH declarations instead of falling back to the solid
           colour.

           The fallback is deliberately confined to a background this same
           declaration block already established.  "Unparseable" here means
           "this engine could not parse it", which is a strictly larger set
           than "invalid CSS": a media control can use the
           relative-colour form `rgb(from #000 r g b/65%)`, which is valid
           and which Chrome renders as a translucent dark fill.  Dropping
           such a declaration outright would resurrect whatever lower-
           priority rule sat beneath it -- for example, a light UA button
           background that repaints a dark media pill opaque white.
           Falling back only within the block captures exactly the author's
           own progressive-enhancement chain and leaves cross-rule cascade
           behaviour byte-for-byte unchanged. */
        bool block_has_background =
            (*mask & (S_BACKGROUND | S_BACKGROUND_IMAGE)) != 0;
        ComputedStyle work = *style;
        uint64_t work_mask = 0;
        uint64_t work_mask_high = 0;
        bool recognized = !shorthand;
        ComputedStyle *target = shorthand ? &work : style;
        uint64_t *target_mask = shorthand ? &work_mask : mask;
        uint64_t *target_mask_high = shorthand ? &work_mask_high : mask_high;
        bool gradient = false;
        StyleGradient parsed_gradient = {0};
        if (shorthand) {
            target->background_image = NULL;
            target->background_image_kind = STYLE_BACKGROUND_IMAGE_NONE;
            *target_mask |= S_BACKGROUND_IMAGE;
            target->has_background = false;
            target->background_alpha = 0;
            *target_mask |= S_BACKGROUND;
            target->background_fit = 0;
            target->background_width = 0;
            target->background_height = 0;
            target->background_size_flags = 0;
            target->background_position_x = 0;
            target->background_position_y = 0;
            *target_mask_high |= S2_BACKGROUND_POSITION;
            char resolved[512];
            bool resolved_ok = style_resolve_value(
                sheet, value, value_length, resolved, sizeof(resolved), 0);
            /* Unresolvable: fall back only if this block has a background to
               fall back to.  Otherwise fall through untouched, so the reset
               and the sub-value attempts below run exactly as they did
               before this fallback existed. */
            if (!resolved_ok && block_has_background) return true;
            if (resolved_ok) {
                /* A single-layer gradient is checked first: its internal commas
                   live inside parentheses, so it can never be mistaken for a
                   multi-layer value, and taking it here keeps the layer path
                   free of image-kind branching. */
                if (style_parse_gradient(sheet, resolved, strlen(resolved),
                                         &parsed_gradient)) {
                    target->background_image_kind =
                        STYLE_BACKGROUND_IMAGE_GRADIENT;
                    gradient = true;
                    recognized = true;
                }
                /* A comma-separated multi-layer shorthand carries the image,
                   position and repeat inside the chosen layer; the single
                   scalar path below only runs for one-layer values. */
                multilayer = !gradient
                    && style_parse_background_shorthand_image(
                           sheet, resolved, strlen(resolved), target, true);
                if (multilayer) {
                    target->background_image_kind =
                        target->background_image != NULL
                        ? STYLE_BACKGROUND_IMAGE_URL
                        : target->background_image_kind;
                    *target_mask |= S_BACKGROUND_IMAGE;
                    *target_mask |= S_BACKGROUND_SIZE;
                    *target_mask_high |= S2_BACKGROUND_REPEAT;
                    *target_mask_high |= S2_BACKGROUND_LAYERS;
                    recognized = true;
                }
                /* A gradient falls through here too: it has no repeat or size
                   component, and the sniff below simply leaves those at the
                   reset values while still publishing their mask bits. */
                if (!multilayer) {
                    target->background_fit = strstr(resolved, "cover") != NULL
                                            ? 1
                                            : (strstr(resolved, "contain")
                                               != NULL ? 2 : 0);
                    *target_mask |= S_BACKGROUND_SIZE;
                    if (strstr(resolved, "no-repeat") != NULL) {
                        target->background_size_flags |=
                            STYLE_BACKGROUND_NO_REPEAT_X
                            | STYLE_BACKGROUND_NO_REPEAT_Y;
                    } else if (strstr(resolved, "repeat-x") != NULL) {
                        target->background_size_flags |=
                            STYLE_BACKGROUND_NO_REPEAT_Y;
                    } else if (strstr(resolved, "repeat-y") != NULL) {
                        target->background_size_flags |=
                            STYLE_BACKGROUND_NO_REPEAT_X;
                    }
                    *target_mask_high |= S2_BACKGROUND_REPEAT;
                    if (style_parse_background_shorthand_position(
                            sheet, resolved, strlen(resolved), target)) {
                        *target_mask_high |= S2_BACKGROUND_POSITION;
                    }
                    if (background_shorthand_recognized(resolved)) {
                        recognized = true;
                    }
                }
            }
        }
        if (image_property && !shorthand
            && style_parse_background_shorthand_image(
                   sheet, value, value_length, target, false)) {
            multilayer = true;
            target->background_image_kind = target->background_image != NULL
                ? STYLE_BACKGROUND_IMAGE_URL
                : target->background_image_kind;
            *target_mask |= S_BACKGROUND_IMAGE;
            *target_mask_high |= S2_BACKGROUND_LAYERS;
            recognized = true;
        }
        if (image_property && !multilayer && !gradient
            && !shorthand
            && style_parse_gradient(sheet, value, value_length,
                                    &parsed_gradient)) {
            /* `background-image: linear-gradient(...)` longhand; the
               shorthand already took its own gradient above. */
            target->background_image = NULL;
            target->background_image_kind = STYLE_BACKGROUND_IMAGE_GRADIENT;
            gradient = true;
            *target_mask |= S_BACKGROUND_IMAGE;
        }
        if (image_property && !multilayer && !gradient
            && style_parse_image_url(sheet, value, value_length,
                               &target->background_image)) {
            target->background_image_kind =
                target->background_image != NULL
                ? STYLE_BACKGROUND_IMAGE_URL
                : STYLE_BACKGROUND_IMAGE_NONE;
            *target_mask |= S_BACKGROUND_IMAGE;
            recognized = true;
        }
        if (!multilayer
            && ((shorthand && (recognized || !block_has_background))
                || (image_property
                    && (*target_mask & S_BACKGROUND_IMAGE) != 0))) {
            bool retained = false;
            if (!style_replace_background_image_paint(
                    sheet, target, gradient ? &parsed_gradient : NULL,
                    shorthand,
                    &retained)) return false;
            if (!retained) return true;
        }
        if (color_property) {
            bool transparent = false;
            bool parsed = shorthand
                ? style_parse_background_shorthand_color(
                      sheet, value, value_length, &target->background,
                      &target->background_alpha, &transparent)
                : style_parse_color_with_alpha(
                      sheet, value, value_length, &target->background,
                      &target->background_alpha);
            if (parsed) {
                target->has_background = shorthand
                                        ? !transparent
                                        : target->background_alpha != 0;
                *target_mask |= S_BACKGROUND;
                recognized = true;
            }
        }
        if (shorthand && (recognized || !block_has_background)) {
            style->background_image = work.background_image;
            style->background_image_kind = work.background_image_kind;
            style->has_background = work.has_background;
            style->background = work.background;
            style->background_alpha = work.background_alpha;
            style->background_fit = work.background_fit;
            style->background_width = work.background_width;
            style->background_height = work.background_height;
            style->background_size_flags = work.background_size_flags;
            style->background_position_x = work.background_position_x;
            style->background_position_y = work.background_position_y;
            computed_style_set_paint_stack_id(
                style, computed_style_paint_stack_id(&work));
            *mask |= work_mask;
            *mask_high |= work_mask_high;
        }

    return true;
}

static bool style_property_background_size(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            if (style_parse_background_size(
                    sheet, value, value_length, style)
                && style_parse_background_layer_size(
                    sheet, value, value_length, style)) {
            *mask |= S_BACKGROUND_SIZE;
        }
    
    return true;
}

static bool style_property_background_position(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
    if (style_parse_background_layer_position(
            sheet, value, value_length, style)) {
        /* The hot first-layer fields cover the common one/two-token form.
           Three/four-token edge offsets and lists live losslessly in the
           retained paint layer. */
        (void) style_parse_background_position(
            sheet, value, value_length, style);
        *mask_high |= S2_BACKGROUND_POSITION;
    }

    return true;
}

static bool style_property_background_position_axis(
    const StylePropertyParse *parse)
{
    bool horizontal = parse->name_length != 0
        && parse->name[parse->name_length - 1u] == 'x';
    if (!style_parse_background_layer_position_axis(
            parse->sheet, parse->value, parse->value_length,
            parse->style, horizontal)) {
        return true;
    }

    /* Keep the allocation-free first-layer fields in step with the retained
       paint layer.  The retained layer carries independent per-axis pixel
       flags; the compact legacy fields conservatively use pixel mode when
       either authored axis is a length. */
    const StylePaintStack *paint = stylesheet_paint_stack(
        parse->sheet, computed_style_paint_stack_id(parse->style));
    if (paint != NULL && paint->background_count != 0) {
        const StylePaintLayer *layer = &paint->backgrounds[0];
        parse->style->background_position_x = layer->position_x;
        parse->style->background_position_y = layer->position_y;
        if ((layer->position_edges
             & (STYLE_PAINT_POSITION_X_PIXELS
                | STYLE_PAINT_POSITION_Y_PIXELS)) != 0) {
            parse->style->background_size_flags |=
                STYLE_BACKGROUND_POSITION_PIXELS;
        } else {
            parse->style->background_size_flags &=
                (uint8_t) ~STYLE_BACKGROUND_POSITION_PIXELS;
        }
    }
    *parse->mask_high |= S2_BACKGROUND_POSITION;
    return true;
}

static bool style_property_background_box(const StylePropertyParse *parse)
{
    bool origin = span_equal(parse->name, parse->name_length,
                             "background-origin");
    bool clip = span_equal(parse->name, parse->name_length,
                           "background-clip");
    if (style_parse_background_box(
            parse->sheet, parse->value, parse->value_length,
            parse->style, origin, clip)) {
        *parse->mask_high |= S2_BACKGROUND_BOX;
    }
    return true;
}

static bool style_property_object_fit(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[48];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->object_fit = strstr(resolved, "scale-down") != NULL
                ? STYLE_OBJECT_FIT_SCALE_DOWN
                : (strstr(resolved, "cover") != NULL
                   ? STYLE_OBJECT_FIT_COVER
                   : (strstr(resolved, "contain") != NULL
                      ? STYLE_OBJECT_FIT_CONTAIN
                      : (strstr(resolved, "none") != NULL
                         ? STYLE_OBJECT_FIT_NONE
                         : STYLE_OBJECT_FIT_FILL)));
            *mask |= S_OBJECT_FIT;
        }
    
    return true;
}

static int object_position_keyword(const char *text)
{
    if (strcmp(text, "left") == 0) return 1;
    if (strcmp(text, "right") == 0) return 2;
    if (strcmp(text, "top") == 0) return 3;
    if (strcmp(text, "bottom") == 0) return 4;
    if (strcmp(text, "center") == 0) return 5;
    return 0;
}

static bool object_position_number(const char *text,
                                   bool *percent, int *value)
{
    char *end = NULL;
    double number = strtod(text, &end);
    if (end == text || !isfinite(number)) return false;
    if (strcmp(end, "%") == 0) *percent = true;
    else if (strcmp(end, "px") == 0
             || (*end == '\0' && number == 0.0)) {
        *percent = false;
    } else {
        return false;
    }
    if (number < -1000000.0) number = -1000000.0;
    if (number > 1000000.0) number = 1000000.0;
    *value = (int) (number < 0.0 ? number - 0.5 : number + 0.5);
    return true;
}

static bool object_position_set_keyword(
    int keyword, const char *distance, uint16_t *x, uint16_t *y)
{
    int anchor = keyword == 2 || keyword == 4 ? 100
                 : (keyword == 5 ? 50 : 0);
    bool percent = false;
    int value = 0;
    if (distance != NULL
        && !object_position_number(distance, &percent, &value)) return false;
    int used_percent = distance != NULL && percent
        ? (anchor == 100 ? 100 - value : value) : anchor;
    int offset = distance != NULL && !percent
        ? (anchor == 100 ? -value : value) : 0;
    uint16_t encoded = style_object_position_encode(used_percent, offset);
    if (keyword <= 2) *x = encoded;
    else if (keyword <= 4) *y = encoded;
    else return false;
    return true;
}

static bool style_property_object_position(const StylePropertyParse *parse)
{
    char resolved[96];
    if (!style_resolve_value(
            parse->sheet, parse->value, parse->value_length,
            resolved, sizeof(resolved), 0)) return true;
    char token[4][16] = {{0}};
    char extra = '\0';
    int count = sscanf(resolved, "%15s %15s %15s %15s %c",
                       token[0], token[1], token[2], token[3], &extra);
    if (count < 1 || count > 4) return true;
    uint16_t x = style_object_position_encode(50, 0);
    uint16_t y = style_object_position_encode(50, 0);
    if (count == 4) {
        int first = object_position_keyword(token[0]);
        int second = object_position_keyword(token[2]);
        if (first == 0 || first == 5 || second == 0 || second == 5
            || (first <= 2) == (second <= 2)
            || !object_position_set_keyword(first, token[1], &x, &y)
            || !object_position_set_keyword(second, token[3], &x, &y)) {
            return true;
        }
    } else if (count == 2) {
        int first = object_position_keyword(token[0]);
        int second = object_position_keyword(token[1]);
        if (first != 0 && second != 0) {
            if (first != 5 && second != 5
                && (first <= 2) == (second <= 2)) return true;
            if ((first != 5 && !object_position_set_keyword(
                                   first, NULL, &x, &y))
                || (second != 5 && !object_position_set_keyword(
                                      second, NULL, &x, &y))) {
                return true;
            }
        } else if (first != 0) {
            if (first == 5
                || !object_position_set_keyword(
                    first, token[1], &x, &y)) return true;
        } else {
            bool percent = false;
            int value = 0;
            if (!object_position_number(token[0], &percent, &value)) {
                return true;
            }
            x = style_object_position_encode(percent ? value : 0,
                                             percent ? 0 : value);
            if (!object_position_number(token[1], &percent, &value)) {
                return true;
            }
            y = style_object_position_encode(percent ? value : 0,
                                             percent ? 0 : value);
        }
    } else {
        int keyword = object_position_keyword(token[0]);
        if (keyword != 0 && keyword != 5) {
            if (!object_position_set_keyword(
                    keyword, NULL, &x, &y)) return true;
        } else if (keyword == 0) {
            bool percent = false;
            int value = 0;
            if (!object_position_number(token[0], &percent, &value)) {
                return true;
            }
            x = style_object_position_encode(percent ? value : 0,
                                             percent ? 0 : value);
        }
    }
    parse->style->object_position_x = x;
    parse->style->object_position_y = y;
    *parse->mask_high |= S2_OBJECT_POSITION;
    return true;
}

static bool style_property_transform_origin(const StylePropertyParse *parse)
{
    char resolved[96];
    if (!style_resolve_value(
            parse->sheet, parse->value, parse->value_length,
            resolved, sizeof(resolved), 0)) return true;
    char token[3][24] = {{0}};
    char extra = '\0';
    int count = sscanf(resolved, "%23s %23s %23s %c",
                       token[0], token[1], token[2], &extra);
    /* Z origins and calc() require a wider representation. Reject them
       explicitly rather than accepting a different two-dimensional origin. */
    if (count < 1 || count > 2) return true;

    uint16_t x = style_object_position_encode(50, 0);
    uint16_t y = style_object_position_encode(50, 0);
    int first_keyword = object_position_keyword(token[0]);
    int second_keyword = count == 2
        ? object_position_keyword(token[1]) : 0;
    bool percent = false;
    int number = 0;
    if (count == 1) {
        if (first_keyword != 0 && first_keyword != 5) {
            if (!object_position_set_keyword(
                    first_keyword, NULL, &x, &y)) return true;
        } else if (first_keyword == 0) {
            if (!object_position_number(token[0], &percent, &number)) {
                return true;
            }
            x = style_object_position_encode(
                percent ? number : 0, percent ? 0 : number);
        }
    } else if (first_keyword != 0 && second_keyword != 0) {
        if (first_keyword != 5 && second_keyword != 5
            && (first_keyword <= 2) == (second_keyword <= 2)) {
            return true;
        }
        if (first_keyword != 5
            && !object_position_set_keyword(
                first_keyword, NULL, &x, &y)) return true;
        if (second_keyword != 5
            && !object_position_set_keyword(
                second_keyword, NULL, &x, &y)) return true;
    } else if (first_keyword != 0) {
        if (!object_position_number(token[1], &percent, &number)) return true;
        uint16_t numeric = style_object_position_encode(
            percent ? number : 0, percent ? 0 : number);
        if (first_keyword <= 2 || first_keyword == 5) {
            if (first_keyword != 5
                && !object_position_set_keyword(
                    first_keyword, NULL, &x, &y)) return true;
            y = numeric;
        } else {
            if (!object_position_set_keyword(
                    first_keyword, NULL, &x, &y)) return true;
            x = numeric;
        }
    } else {
        if (!object_position_number(token[0], &percent, &number)) return true;
        x = style_object_position_encode(
            percent ? number : 0, percent ? 0 : number);
        if (second_keyword != 0) {
            if (second_keyword <= 2) return true;
            if (second_keyword != 5
                && !object_position_set_keyword(
                    second_keyword, NULL, &x, &y)) return true;
        } else {
            if (!object_position_number(
                    token[1], &percent, &number)) return true;
            y = style_object_position_encode(
                percent ? number : 0, percent ? 0 : number);
        }
    }

    StylePaintStack stack = {0};
    const StylePaintStack *current = stylesheet_paint_stack(
        parse->sheet, computed_style_paint_stack_id(parse->style));
    if (current != NULL) stack = *current;
    stack.transform_origin_x = x;
    stack.transform_origin_y = y;
    stack.components |= STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN;
    uint8_t id = 0;
    StylePaintInternResult result =
        style_intern_paint_stack(parse->sheet, &stack, &id);
    if (result == STYLE_PAINT_INTERN_LIMIT) return true;
    if (result != STYLE_PAINT_INTERN_RETAINED) return false;
    computed_style_set_paint_stack_id(parse->style, id);
    /* S2_BACKGROUND_BOX is the existing independently merged optional-paint
       lane. apply_paint_values checks component bits, so this does not reset
       an unrelated background-origin/clip declaration. */
    *parse->mask_high |= S2_BACKGROUND_BOX;
    return true;
}
