/* Inline-SVG expansion: <use> and <symbol> resolution against the page and
   external sprites, presentation inheritance, currentColor, the default
   viewport, and serialization of the expanded tree into decodable markup.
   Included once by image.c; the loading, refresh and publication of the
   resulting rasters stay there. */
typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
} InlineSvgBuffer;

typedef struct {
    const PocDocument *document;
    lxb_dom_node_t *lookup_root;
    lxb_dom_node_t *symbols[INLINE_SVG_USE_DEPTH_LIMIT];
    size_t use_count;
    size_t expanded_nodes;
} InlineSvgUseValidation;

static lxb_status_t inline_svg_receive(const lxb_char_t *data, size_t length,
                                       void *opaque)
{
    InlineSvgBuffer *buffer = opaque;
    if (buffer == NULL || data == NULL
        || length > INLINE_SVG_SOURCE_LIMIT - buffer->length) {
        return LXB_STATUS_ERROR;
    }
    size_t wanted = buffer->length + length + 1;
    if (wanted > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 512 : buffer->capacity;
        while (capacity < wanted && capacity < INLINE_SVG_SOURCE_LIMIT + 1u) {
            size_t grown = capacity <= (INLINE_SVG_SOURCE_LIMIT + 1u) / 2u
                           ? capacity * 2u : INLINE_SVG_SOURCE_LIMIT + 1u;
            if (grown <= capacity) break;
            capacity = grown;
        }
        if (capacity < wanted) return LXB_STATUS_ERROR;
        char *expanded = budget_realloc(buffer->budget, buffer->data,
                                        capacity);
        if (expanded == NULL) return LXB_STATUS_ERROR;
        buffer->data = expanded;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return LXB_STATUS_OK;
}

static lxb_dom_node_t *inline_svg_next_node(lxb_dom_node_t *root,
                                            lxb_dom_node_t *node,
                                            bool skip_children)
{
    if (!skip_children && node->first_child != NULL) {
        return node->first_child;
    }
    while (node != root && node->next == NULL) node = node->parent;
    return node == root ? NULL : node->next;
}

static bool inline_svg_definition_container(lxb_dom_node_t *node)
{
    return image_name_is(node, "symbol") || image_name_is(node, "defs");
}

static const char *inline_svg_use_href(lxb_dom_node_t *node, size_t *length)
{
    const char *href = document_attribute(node, "href", length);
    if (href == NULL) {
        href = document_attribute(node, "xlink:href", length);
    }
    return href;
}

/* External SVG sprites are a common way for responsive navigation controls
   to share a few tiny icons. Treat only the deterministic shape as an image
   resource: one rendered <use> child with a path plus bounded fragment. */
static const char *inline_svg_external_use_href(lxb_dom_node_t *root,
                                                size_t *length)
{
    if (length != NULL) *length = 0;
    if (root == NULL || !document_node_is_tag(root, LXB_TAG_SVG)) return NULL;
    lxb_dom_node_t *use = NULL;
    for (lxb_dom_node_t *child = root->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (use != NULL || !image_name_is(child, "use")) return NULL;
        use = child;
    }
    size_t href_length = 0;
    const char *href = use == NULL ? NULL
        : inline_svg_use_href(use, &href_length);
    if (href == NULL || href_length < 3 || href_length >= 2048) return NULL;
    size_t hash = href_length;
    while (hash != 0 && href[hash - 1] != '#') hash--;
    if (hash <= 1 || hash == href_length
        || href_length - hash > EXTERNAL_SVG_SYMBOL_ID_LIMIT) return NULL;
    for (size_t i = hash; i < href_length; i++) {
        if (href[i] == '#' || isspace((unsigned char) href[i])) return NULL;
    }
    if (length != NULL) *length = href_length;
    return href;
}

static bool external_svg_identifier_matches(const char *tag, size_t length,
                                             const char *wanted,
                                             size_t wanted_length)
{
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) tag[at])) at++;
        size_t name_start = at;
        while (at < length
               && (isalnum((unsigned char) tag[at]) || tag[at] == '-'
                   || tag[at] == '_' || tag[at] == ':')) at++;
        size_t name_length = at - name_start;
        while (at < length && isspace((unsigned char) tag[at])) at++;
        if (at >= length || tag[at] != '=') {
            while (at < length && !isspace((unsigned char) tag[at])) at++;
            continue;
        }
        at++;
        while (at < length && isspace((unsigned char) tag[at])) at++;
        if (at >= length || (tag[at] != '\'' && tag[at] != '"')) return false;
        char quote = tag[at++];
        size_t value_start = at;
        while (at < length && tag[at] != quote) at++;
        if (at == length) return false;
        size_t value_length = at - value_start;
        if (name_length == 2
            && strncasecmp(tag + name_start, "id", 2) == 0
            && value_length == wanted_length
            && memcmp(tag + value_start, wanted, wanted_length) == 0) {
            return true;
        }
        at++;
    }
    return false;
}

static const char external_svg_symbol_open[] = "<symbol";
#define EXTERNAL_SVG_SYMBOL_OPEN_LENGTH (sizeof(external_svg_symbol_open) - 1u)
/* Distinct symbol tags indexed per sprite; fragments past the limit are
   found by scanning the sprite text. */
#define EXTERNAL_SVG_SYMBOL_INDEX_LIMIT 4096u

/* A complete <symbol ...> start tag at `at`: its attributes span
   [*after_name, *end) and source[*end] is the closing '>'. */
static bool external_svg_symbol_tag(const char *source, size_t length,
                                    size_t at, size_t *after_name_out,
                                    size_t *end_out)
{
    if (source[at] != '<'
        || strncasecmp(source + at, external_svg_symbol_open,
                       EXTERNAL_SVG_SYMBOL_OPEN_LENGTH) != 0) return false;
    size_t after_name = at + EXTERNAL_SVG_SYMBOL_OPEN_LENGTH;
    if (after_name >= length
        || (!isspace((unsigned char) source[after_name])
            && source[after_name] != '>')) return false;
    size_t end = after_name;
    char quote = '\0';
    while (end < length && end - at <= EXTERNAL_SVG_SYMBOL_TAG_LIMIT) {
        char c = source[end];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '>') {
            break;
        }
        end++;
    }
    if (end == length || source[end] != '>') return false;
    *after_name_out = after_name;
    *end_out = end;
    return true;
}

static bool external_svg_symbol_close(const char *source, size_t length,
                                      size_t after_name, size_t end,
                                      size_t *attributes_start,
                                      size_t *open_end, size_t *close_start)
{
    static const char close_name[] = "</symbol";
    /* A body longer than the symbol limit is refused either way, so the
       search for its end stops there instead of at the end of the sprite. */
    size_t close = end + 1u;
    while (close + sizeof(close_name) - 1u < length
           && close - (end + 1u) <= EXTERNAL_SVG_SYMBOL_LIMIT) {
        if (source[close] == '<'
            && strncasecmp(source + close, close_name,
                           sizeof(close_name) - 1u) == 0) break;
        close++;
    }
    if (close + sizeof(close_name) - 1u >= length
        || close - (end + 1u) > EXTERNAL_SVG_SYMBOL_LIMIT) return false;
    *attributes_start = after_name;
    *open_end = end;
    *close_start = close;
    return true;
}

/* The start tags of one sprite response in document order, built once so
   each sibling icon looks its fragment up without rescanning the sprite.
   `resume` is where an unindexed tail begins when the index is full. */
typedef struct ExternalSvgSymbolIndex {
    const char *source;
    size_t length;
    uint32_t (*tags)[2];
    size_t count;
    size_t resume;
} ExternalSvgSymbolIndex;

static void external_svg_symbol_index_build(Budget *budget,
    const char *source, size_t length, ExternalSvgSymbolIndex *index)
{
    memset(index, 0, sizeof(*index));
    index->source = source;
    index->length = length;
    if (length > UINT32_MAX) return; /* Unindexed: resume scanning at 0. */
    size_t capacity = 0;
    for (size_t at = 0; at + EXTERNAL_SVG_SYMBOL_OPEN_LENGTH < length; at++) {
#ifndef __PSP__
        tilefinch_test_faults()->external_svg_symbol_scan_bytes++;
#endif
        size_t after_name = 0, end = 0;
        if (!external_svg_symbol_tag(source, length, at, &after_name, &end))
            continue;
        if (index->count == capacity) {
            size_t grown = capacity == 0 ? 32u : capacity * 2u;
            if (capacity == EXTERNAL_SVG_SYMBOL_INDEX_LIMIT) {
                index->resume = at;
                return;
            }
            uint32_t (*tags)[2] = budget_realloc(budget, index->tags,
                grown * sizeof(*tags));
            if (tags == NULL) {
                /* Unindexed: look every fragment up by scanning the text. */
                budget_free(budget, index->tags);
                index->tags = NULL;
                index->count = 0;
                index->resume = 0;
                return;
            }
            index->tags = tags;
            capacity = grown;
        }
        /* EXTERNAL_SVG_SPRITE_FETCH_LIMIT keeps every offset in 32 bits. */
        index->tags[index->count][0] = (uint32_t) after_name;
        index->tags[index->count][1] = (uint32_t) end;
        index->count++;
    }
    index->resume = length;
}

static bool external_svg_symbol_bounds(const char *source, size_t length,
                                       const ExternalSvgSymbolIndex *index,
                                       const char *identifier,
                                       size_t identifier_length,
                                       size_t *attributes_start,
                                       size_t *open_end, size_t *close_start)
{
    size_t start = 0;
    if (index != NULL && index->source == source && index->length == length) {
        for (size_t i = 0; i < index->count; i++) {
            size_t after_name = index->tags[i][0], end = index->tags[i][1];
            if (external_svg_identifier_matches(
                    source + after_name, end - after_name,
                    identifier, identifier_length))
                return external_svg_symbol_close(source, length, after_name,
                    end, attributes_start, open_end, close_start);
        }
        start = index->resume;
    }
    for (size_t at = start; at + EXTERNAL_SVG_SYMBOL_OPEN_LENGTH < length; at++) {
#ifndef __PSP__
        tilefinch_test_faults()->external_svg_symbol_scan_bytes++;
#endif
        size_t after_name = 0, end = 0;
        if (!external_svg_symbol_tag(source, length, at, &after_name, &end)
            || !external_svg_identifier_matches(
                   source + after_name, end - after_name,
                   identifier, identifier_length)) continue;
        return external_svg_symbol_close(source, length, after_name, end,
            attributes_start, open_end, close_start);
    }
    return false;
}

static bool image_style_chain_for_node(const Stylesheet *sheet,
                                       lxb_dom_node_t *node,
                                       ComputedStyle *result)
{
    enum { MAXIMUM_ANCESTORS = 64 };
    lxb_dom_node_t *ancestors[MAXIMUM_ANCESTORS];
    size_t count = 0;
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == MAXIMUM_ANCESTORS) return false;
        ancestors[count++] = at;
    }
    ComputedStyle computed = {0};
    bool has_parent = false;
    while (count != 0) {
        computed = style_for_node(sheet, ancestors[--count],
                                  has_parent ? &computed : NULL);
        if (computed.display == DISPLAY_NONE || computed.hidden) return false;
        has_parent = true;
    }
    if (!has_parent) return false;
    *result = computed;
    return true;
}

static lxb_dom_node_t *inline_svg_find_id(lxb_dom_node_t *root,
                                          const char *identifier,
                                          size_t identifier_length)
{
    if (root == NULL || identifier == NULL
        || identifier_length == 0) return NULL;
    for (lxb_dom_node_t *node = root; node != NULL;
         node = inline_svg_next_node(root, node, false)) {
        size_t id_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        if (id != NULL && id_length == identifier_length
            && memcmp(id, identifier, identifier_length) == 0) {
            return node;
        }
    }
    return NULL;
}

static bool inline_svg_reference_target(lxb_dom_node_t *target)
{
    if (target == NULL || target->type != LXB_DOM_NODE_TYPE_ELEMENT
        || image_name_is(target, "use") || image_name_is(target, "defs")) {
        return false;
    }
    for (lxb_dom_node_t *at = target; at != NULL; at = at->parent) {
        if (document_node_is_tag(at, LXB_TAG_SVG)) return true;
    }
    return false;
}

static lxb_dom_node_t *inline_svg_resolve_target(
    const InlineSvgUseValidation *validation, lxb_dom_node_t *use)
{
    size_t href_length = 0;
    const char *href = inline_svg_use_href(use, &href_length);
    if (href == NULL || href_length < 2 || href_length > 129
        || href[0] != '#') return NULL;
    for (size_t i = 1; i < href_length; i++) {
        if (href[i] == '#' || isspace((unsigned char) href[i])) return NULL;
    }
    lxb_dom_node_t *target = inline_svg_find_id(
        validation->lookup_root, href + 1, href_length - 1);
    if (target == NULL && validation->document != NULL
        && validation->document->html != NULL) {
        target = inline_svg_find_id(
            lxb_dom_interface_node(validation->document->html),
            href + 1, href_length - 1);
    }
    return inline_svg_reference_target(target) ? target : NULL;
}

static size_t inline_svg_subtree_nodes(lxb_dom_node_t *root, size_t limit)
{
    size_t count = 0;
    for (lxb_dom_node_t *node = root->first_child; node != NULL;
         node = inline_svg_next_node(root, node, false)) {
        if (++count > limit) return count;
    }
    return count;
}

static bool inline_svg_validate_tree(InlineSvgUseValidation *validation,
                                     lxb_dom_node_t *container,
                                     size_t depth);

static bool inline_svg_validate_use(InlineSvgUseValidation *validation,
                                    lxb_dom_node_t *use, size_t depth)
{
    if (validation->use_count >= INLINE_SVG_USE_LIMIT
        || depth >= INLINE_SVG_USE_DEPTH_LIMIT) return false;
    lxb_dom_node_t *symbol = inline_svg_resolve_target(validation, use);
    if (symbol == NULL) return false;
    for (size_t i = 0; i < depth; i++) {
        if (validation->symbols[i] == symbol) return false;
    }
    size_t remaining = INLINE_SVG_EXPANSION_NODE_LIMIT
                       - validation->expanded_nodes;
    size_t nodes = inline_svg_subtree_nodes(symbol, remaining);
    if (nodes > remaining) return false;
    validation->expanded_nodes += nodes;
    validation->use_count++;
    validation->symbols[depth] = symbol;
    bool valid = inline_svg_validate_tree(validation, symbol, depth + 1);
    validation->symbols[depth] = NULL;
    return valid;
}

static bool inline_svg_validate_tree(InlineSvgUseValidation *validation,
                                     lxb_dom_node_t *container,
                                     size_t depth)
{
    for (lxb_dom_node_t *node = container->first_child; node != NULL;
         node = inline_svg_next_node(
             container, node, inline_svg_definition_container(node))) {
        if (image_name_is(node, "use")
            && !inline_svg_validate_use(validation, node, depth)) {
            return false;
        }
    }
    return true;
}

static bool inline_svg_skip_instance_attribute(const char *name,
                                               size_t length)
{
    static const char *const skipped[] = {
        "href", "xlink:href", "id", "x", "y", "width", "height",
        "viewBox", "preserveAspectRatio"
    };
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        size_t wanted = strlen(skipped[i]);
        if (length == wanted && strncasecmp(name, skipped[i], wanted) == 0) {
            return true;
        }
    }
    return false;
}

static bool inline_svg_copy_instance_attributes(lxb_dom_element_t *target,
                                                lxb_dom_node_t *source)
{
    if (target == NULL || source == NULL
        || source->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *element = lxb_dom_interface_element(source);
    for (lxb_dom_attr_t *attribute = element->first_attr;
         attribute != NULL; attribute = attribute->next) {
        size_t name_length = 0, value_length = 0;
        const lxb_char_t *name = lxb_dom_attr_qualified_name(
            attribute, &name_length);
        const lxb_char_t *value = lxb_dom_attr_value(
            attribute, &value_length);
        if (name == NULL || inline_svg_skip_instance_attribute(
                (const char *) name, name_length)) continue;
        if (lxb_dom_element_set_attribute(target, name, name_length,
                                          value, value_length) == NULL) {
            return false;
        }
    }
    return true;
}

static bool inline_svg_apply_use_translation(lxb_dom_element_t *group,
                                             lxb_dom_node_t *symbol,
                                             lxb_dom_node_t *use)
{
    size_t x_length = 0, y_length = 0, transform_length = 0;
    const char *x = document_attribute(use, "x", &x_length);
    const char *y = document_attribute(use, "y", &y_length);
    const char *transform = document_attribute(use, "transform",
                                                &transform_length);
    if (transform == NULL) {
        transform = document_attribute(symbol, "transform",
                                       &transform_length);
    }
    if ((x == NULL || x_length == 0) && (y == NULL || y_length == 0)) {
        return true;
    }
    if (x_length > 96 || y_length > 96 || transform_length > 256) {
        return false;
    }
    if (x == NULL || x_length == 0) {
        x = "0";
        x_length = 1;
    }
    if (y == NULL || y_length == 0) {
        y = "0";
        y_length = 1;
    }
    char combined[512];
    int written = snprintf(
        combined, sizeof(combined), "translate(%.*s %.*s)%s%.*s",
        (int) x_length, x, (int) y_length, y,
        transform_length == 0 ? "" : " ", (int) transform_length,
        transform == NULL ? "" : transform);
    return written > 0 && (size_t) written < sizeof(combined)
        && lxb_dom_element_set_attribute(
               group, (const lxb_char_t *) "transform", 9,
               (const lxb_char_t *) combined, (size_t) written) != NULL;
}

static bool inline_svg_expand_one_use(InlineSvgUseValidation *validation,
                                      lxb_dom_node_t *use)
{
    lxb_dom_node_t *target = inline_svg_resolve_target(validation, use);
    if (target == NULL || use->parent == NULL) return false;
    static const lxb_char_t group_name[] = "g";
    lxb_dom_element_t *group = lxb_dom_document_create_element(
        use->owner_document, group_name, 1, NULL);
    if (group == NULL
        || !inline_svg_copy_instance_attributes(group, target)
        || !inline_svg_copy_instance_attributes(group, use)
        || !inline_svg_apply_use_translation(group, target, use)) {
        if (group != NULL) {
            lxb_dom_node_destroy_deep(lxb_dom_interface_node(group));
        }
        return false;
    }
    lxb_dom_node_t *group_node = lxb_dom_interface_node(group);
    bool container = image_name_is(target, "symbol")
        || image_name_is(target, "g") || document_node_is_tag(target, LXB_TAG_SVG);
    lxb_dom_node_t *child = container ? target->first_child : target;
    while (child != NULL) {
        lxb_dom_node_t *next = container ? child->next : NULL;
        lxb_dom_node_t *copy = lxb_dom_node_clone(child, true);
        if (copy == NULL
            || lxb_dom_node_append_child(group_node, copy)
                   != LXB_DOM_EXCEPTION_OK) {
            if (copy != NULL && copy->parent == NULL) {
                lxb_dom_node_destroy_deep(copy);
            }
            lxb_dom_node_destroy_deep(group_node);
            return false;
        }
        child = next;
    }
    lxb_dom_node_t *parent = use->parent;
    if (lxb_dom_node_replace_child(parent, group_node, use)
            != LXB_DOM_EXCEPTION_OK) {
        lxb_dom_node_destroy_deep(group_node);
        return false;
    }
    lxb_dom_node_destroy_deep(use);
    return true;
}

static lxb_dom_node_t *inline_svg_first_rendered_use(lxb_dom_node_t *root)
{
    for (lxb_dom_node_t *node = root->first_child; node != NULL;
         node = inline_svg_next_node(
             root, node, inline_svg_definition_container(node))) {
        if (image_name_is(node, "use")) return node;
    }
    return NULL;
}

static void inline_svg_remove_symbols(lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root->first_child;
    while (node != NULL) {
        bool symbol = image_name_is(node, "symbol");
        lxb_dom_node_t *next = inline_svg_next_node(root, node, symbol);
        if (symbol) {
            lxb_dom_node_destroy_deep(node);
        }
        node = next;
    }
}

static bool inline_svg_apply_single_symbol_viewbox(
    InlineSvgUseValidation *validation, lxb_dom_node_t *root,
    lxb_dom_node_t *original_root)
{
    size_t existing_length = 0;
    if (document_attribute(original_root, "viewBox", &existing_length) != NULL
        || validation->use_count != 1) return true;
    lxb_dom_node_t *use = inline_svg_first_rendered_use(original_root);
    lxb_dom_node_t *symbol = use == NULL ? NULL
        : inline_svg_resolve_target(validation, use);
    size_t viewbox_length = 0;
    const char *viewbox = symbol == NULL ? NULL
        : document_attribute(symbol, "viewBox", &viewbox_length);
    if (viewbox == NULL || viewbox_length == 0) return true;
    return lxb_dom_element_set_attribute(
        lxb_dom_interface_element(root), (const lxb_char_t *) "viewBox", 7,
        (const lxb_char_t *) viewbox, viewbox_length) != NULL;
}

static bool inline_svg_apply_default_viewport(
    lxb_dom_node_t *root, const ComputedStyle *style)
{
    if (root == NULL || root->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *element = lxb_dom_interface_element(root);
    size_t width_length = 0;
    size_t height_length = 0;
    const char *width_attribute = document_attribute(
        root, "width", &width_length);
    const char *height_attribute = document_attribute(
        root, "height", &height_length);
    bool width_missing = width_attribute == NULL;
    bool height_missing = height_attribute == NULL;
    char width[16] = "300";
    char height[16] = "150";
    float numeric_width = 0.0f;
    float numeric_height = 0.0f;
    if (numeric_width <= 0.0f && width_attribute != NULL
        && width_length > 0u && width_length < 32u) {
        char value[32];
        memcpy(value, width_attribute, width_length);
        value[width_length] = '\0';
        char *end = NULL;
        float parsed = strtof(value, &end);
        while (end != NULL && isspace((unsigned char) *end)) end++;
        if (end != value && (*end == '\0' || strcasecmp(end, "px") == 0)
            && isfinite(parsed) && parsed > 0.0f && parsed <= 32767.0f) {
            numeric_width = parsed;
        }
    }
    if (numeric_height <= 0.0f && height_attribute != NULL
        && height_length > 0u && height_length < 32u) {
        char value[32];
        memcpy(value, height_attribute, height_length);
        value[height_length] = '\0';
        char *end = NULL;
        float parsed = strtof(value, &end);
        while (end != NULL && isspace((unsigned char) *end)) end++;
        if (end != value && (*end == '\0' || strcasecmp(end, "px") == 0)
            && isfinite(parsed) && parsed > 0.0f && parsed <= 32767.0f) {
            numeric_height = parsed;
        }
    }
    float authored_width = numeric_width, authored_height = numeric_height;
    if (style != NULL && style->has_width && !style->width_percent
        && style->width_offset == 0 && style->width > 0
        && style->width <= 32767) {
        snprintf(width, sizeof(width), "%d", style->width);
        numeric_width = (float) style->width;
    }
    if (style != NULL && style->has_height && !style->height_percent
        && style->height > 0 && style->height <= 32767) {
        snprintf(height, sizeof(height), "%d", style->height);
        numeric_height = (float) style->height;
    }
    /* Uniform resizing may retain the authored raster for display
       retargeting. A changed viewport ratio must instead be applied before
       viewBox mapping: stretching its old pixels distorts the artwork.
       The resolved viewport is part of the serialized cache identity. */
    if (style != NULL && style->has_width && !style->width_percent
        && style->width_offset == 0 && style->has_height && !style->height_percent
        && authored_width > 0 && authored_height > 0
        && numeric_width > 0 && numeric_height > 0
        && fabsf(numeric_width * authored_height
                 - numeric_height * authored_width) > 0.01f) {
        snprintf(width, sizeof(width), "%d", (int) ceilf(numeric_width));
        snprintf(height, sizeof(height), "%d", (int) ceilf(numeric_height));
        width_missing = height_missing = true;
    }
    /* A viewBox gives the SVG an intrinsic ratio. If exactly one dimension
       is known -- from CSS or from an attribute -- the other follows that
       ratio instead of HTML's 300x150 legacy default; a CSS height with
       width:auto must not rasterize a 300px-wide image that layout then
       stretches (BBC's logo). With neither known the raster keeps the
       300px default width and takes its height from the ratio, so layout
       scales it into its container undistorted (the NYT masthead). Only
       the 300x150 default remains for SVGs without a usable viewBox. */
    bool width_known = numeric_width > 0.0f;
    bool height_known = numeric_height > 0.0f;
    if (!width_known && !height_known && width_missing && height_missing) {
        numeric_width = 300.0f;
        width_known = true;
    }
    if (width_known != height_known
        && (width_known ? height_missing : width_missing)) {
        size_t viewbox_length = 0;
        const char *viewbox = document_attribute(
            root, "viewBox", &viewbox_length);
        if (viewbox != NULL && viewbox_length > 0u && viewbox_length < 96u) {
            char value[96];
            memcpy(value, viewbox, viewbox_length);
            value[viewbox_length] = '\0';
            for (size_t at = 0; at < viewbox_length; at++) {
                if (value[at] == ',') value[at] = ' ';
            }
            float min_x = 0.0f, min_y = 0.0f;
            float viewbox_width = 0.0f, viewbox_height = 0.0f;
            if (sscanf(value, "%f %f %f %f", &min_x, &min_y,
                       &viewbox_width, &viewbox_height) == 4
                && isfinite(viewbox_width) && isfinite(viewbox_height)
                && viewbox_width > 0.0f && viewbox_height > 0.0f) {
                if (!height_known) {
                    float derived = numeric_width * viewbox_height
                                    / viewbox_width;
                    if (isfinite(derived) && derived > 0.0f
                        && derived <= 32767.0f) {
                        numeric_height = fmaxf(1.0f, roundf(derived));
                        snprintf(height, sizeof(height), "%d",
                                 (int) numeric_height);
                    }
                } else {
                    float derived = numeric_height * viewbox_width
                                    / viewbox_height;
                    if (isfinite(derived) && derived > 0.0f
                        && derived <= 32767.0f) {
                        numeric_width = fmaxf(1.0f, roundf(derived));
                        snprintf(width, sizeof(width), "%d",
                                 (int) numeric_width);
                    }
                }
            }
        }
    }
    if (width_missing
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "width", 5,
               (const lxb_char_t *) width, strlen(width)) == NULL) {
        return false;
    }
    if (height_missing
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "height", 6,
               (const lxb_char_t *) height, strlen(height)) == NULL) {
        return false;
    }
    return true;
}

static bool inline_svg_value_is(const char *value, const char *wanted)
{
    return value != NULL && strcasecmp(value, wanted) == 0;
}

static const char *inline_svg_initial_presentation(const char *name)
{
    if (strcmp(name, "fill") == 0) return "#000000";
    if (strcmp(name, "stroke") == 0) return "none";
    if (strcmp(name, "fill-opacity") == 0
        || strcmp(name, "stroke-opacity") == 0
        || strcmp(name, "opacity") == 0
        || strcmp(name, "stroke-width") == 0) return "1";
    if (strcmp(name, "stroke-miterlimit") == 0) return "4";
    if (strcmp(name, "fill-rule") == 0) return "nonzero";
    if (strcmp(name, "stroke-linecap") == 0) return "butt";
    if (strcmp(name, "stroke-linejoin") == 0) return "miter";
    if (strcmp(name, "stroke-dasharray") == 0) return "none";
    if (strcmp(name, "stroke-dashoffset") == 0) return "0";
    return NULL;
}

static bool inline_svg_inherited_presentation(const char *name)
{
    return strcmp(name, "opacity") != 0;
}

static bool inline_svg_apply_presentation_value(
    const Stylesheet *sheet, lxb_dom_node_t *original,
    lxb_dom_node_t *clone, const ComputedStyle *style, const char *name,
    const char *css_value)
{
    /* Room for an attribute's var() references once substituted. */
    char value[STYLE_CUSTOM_RESOLVED_CAPACITY];
    size_t name_length = strlen(name);
    bool from_css = css_value != NULL;
    bool invalid = false;
    if (from_css) snprintf(value, sizeof(value), "%s", css_value);
    if (!from_css) {
        size_t attribute_length = 0;
        const char *attribute = document_attribute(
            original, name, &attribute_length);
        if (attribute != NULL
            && attribute_length < STYLE_RETAINED_VALUE_CAPACITY) {
            memcpy(value, attribute, attribute_length);
            value[attribute_length] = '\0';
            /* Presentation attributes are CSS values: `fill="var(--x)"`
               resolves on the element exactly like the same declaration
               (the masthead logo of a large news page is filled with a
               custom property; utility-CSS tokens are oklch()). One
               that cannot resolve is invalid at computed-value time, so
               the property is unset. */
            if (strstr(value, "var(") != NULL
                && !style_resolve_value_on_node(
                       sheet, original, attribute, attribute_length,
                       value, sizeof(value))) {
                value[0] = '\0';
                invalid = true;
            }
        } else {
            value[0] = '\0';
        }
    }
    bool inherited = inline_svg_inherited_presentation(name);
    if (inline_svg_value_is(value, "inherit")
        || (inherited && inline_svg_value_is(value, "unset"))) {
        value[0] = '\0';
    } else if (inline_svg_value_is(value, "initial")
               || (!inherited && inline_svg_value_is(value, "unset"))) {
        const char *initial = inline_svg_initial_presentation(name);
        if (initial == NULL) return true;
        snprintf(value, sizeof(value), "%s", initial);
    }
    if (value[0] == '\0' && inherited && clone->parent != NULL
        && clone->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t parent_length = 0;
        const char *parent = document_attribute(
            clone->parent, name, &parent_length);
        if (parent != NULL
            && parent_length < STYLE_RETAINED_VALUE_CAPACITY) {
            memcpy(value, parent, parent_length);
            value[parent_length] = '\0';
        }
    }
    if (value[0] == '\0') {
        /* The clone still carries the unresolved var() text, which the
           rasterizer would read as an unknown colour. */
        if (invalid) {
            (void) lxb_dom_element_remove_attribute(
                lxb_dom_interface_element(clone),
                (const lxb_char_t *) name, name_length);
        }
        return true;
    }
    /* The rasterizer reads hex and rgb() colors only: settle CSS color
       functions it does not know (light-dark(), color-mix(), hsl(), ...)
       to the hex the cascade would compute. MDN's logo is filled with
       light-dark(). */
    if (strchr(value, '(') != NULL && strncasecmp(value, "url(", 4) != 0
        && strncasecmp(value, "rgb", 3) != 0
        && (strcmp(name, "fill") == 0 || strcmp(name, "stroke") == 0
            || strcmp(name, "stop-color") == 0)) {
        uint32_t color = 0;
        uint8_t alpha = 255;
        if (style_color_parse(value, strlen(value), &color, &alpha)) {
            if (alpha == 0) snprintf(value, sizeof(value), "none");
            else snprintf(value, sizeof(value), "#%06x",
                          (unsigned) (color & UINT32_C(0xffffff)));
        }
    }
    if (inline_svg_value_is(value, "currentcolor")) {
        snprintf(value, sizeof(value), "#%06x",
                 (unsigned) (style->color & UINT32_C(0xffffff)));
    }
    return lxb_dom_element_set_attribute(
        lxb_dom_interface_element(clone),
        (const lxb_char_t *) name, name_length,
        (const lxb_char_t *) value, strlen(value)) != NULL;
}

static bool inline_svg_apply_node_presentation(
    ImageLoadContext *context, lxb_dom_node_t *original,
    lxb_dom_node_t *clone, const ComputedStyle *style)
{
    StyleRetainedPresentationValues retained;
    if (!style_retained_presentation_values(
            context->stylesheet, original, &retained)) return false;
    for (size_t i = 0; i < STYLE_RETAINED_PRESENTATION_COUNT; i++) {
        const char *name = style_retained_presentation_name(i);
        uint16_t bit = (uint16_t) (UINT16_C(1) << i);
        const char *css_value = (retained.present_mask & bit) != 0
            ? retained.values[i] : NULL;
        if (name == NULL
            || !inline_svg_apply_presentation_value(
                context->stylesheet, original, clone, style, name,
                css_value)) return false;
    }
    return true;
}

static bool inline_svg_apply_presentation(
    ImageLoadContext *context, lxb_dom_node_t *original,
    lxb_dom_node_t *clone, const ComputedStyle *parent_style,
    size_t depth, size_t *node_count)
{
    if (original == NULL || clone == NULL || node_count == NULL
        || depth > INLINE_SVG_STYLE_DEPTH_LIMIT
        || ++*node_count > INLINE_SVG_STYLE_NODE_LIMIT) return false;
    ComputedStyle style = *parent_style;
    if (original->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        if (depth != 0) {
            style = style_for_node(
                context->stylesheet, original, parent_style);
        }
        if (!inline_svg_apply_node_presentation(
                context, original, clone, &style)) return false;
    }
    lxb_dom_node_t *original_child = original->first_child;
    lxb_dom_node_t *clone_child = clone->first_child;
    while (original_child != NULL && clone_child != NULL) {
        if (!inline_svg_apply_presentation(
                context, original_child, clone_child, &style,
                depth + 1, node_count)) return false;
        original_child = original_child->next;
        clone_child = clone_child->next;
    }
    return original_child == NULL && clone_child == NULL;
}

static bool inline_svg_serialize_expanded(ImageLoadContext *context,
                                          lxb_dom_node_t *node,
                                          const ComputedStyle *root_style,
                                          InlineSvgBuffer *source)
{
    InlineSvgUseValidation validation = {
        .document = context->document,
        .lookup_root = lxb_dom_interface_node(context->document->html)
    };
    if (!inline_svg_validate_tree(&validation, node, 0)) return false;
    lxb_dom_node_t *clone = lxb_dom_node_clone(node, true);
    size_t styled_nodes = 0;
    if (clone == NULL || root_style == NULL
        || !inline_svg_apply_presentation(
            context, node, clone, root_style, 0, &styled_nodes)) {
        if (clone != NULL) lxb_dom_node_destroy_deep(clone);
        return false;
    }
    validation.lookup_root = clone;
    if (!inline_svg_apply_default_viewport(clone, root_style)
        || !inline_svg_apply_single_symbol_viewbox(
               &validation, clone, node)) {
        lxb_dom_node_destroy_deep(clone);
        return false;
    }
    size_t expanded = 0;
    lxb_dom_node_t *use = NULL;
    while ((use = inline_svg_first_rendered_use(clone)) != NULL) {
        if (expanded++ >= validation.use_count
            || !inline_svg_expand_one_use(&validation, use)) {
            lxb_dom_node_destroy_deep(clone);
            return false;
        }
    }
    inline_svg_remove_symbols(clone);
    bool serialized = lxb_html_serialize_tree_cb(
        clone, inline_svg_receive, source) == LXB_STATUS_OK
        && source->data != NULL && source->length != 0;
    lxb_dom_node_destroy_deep(clone);
    return serialized;
}

/* NanoSVG deliberately implements a small SVG subset and does not resolve
   the CSS currentColor keyword.  Resolve it to the root SVG's computed color
   before rasterization; this preserves the common inherited icon case while
   keeping the retained raster independent of the stylesheet lifetime. */
static void inline_svg_resolve_current_color(InlineSvgBuffer *buffer,
                                             uint32_t color)
{
    if (buffer == NULL || buffer->data == NULL) return;
    char replacement[8];
    snprintf(replacement, sizeof(replacement), "#%06x",
             (unsigned) (color & UINT32_C(0xffffff)));
    static const char keyword[] = "currentColor";
    size_t read = 0, write = 0;
    while (read < buffer->length) {
        if (buffer->length - read >= sizeof(keyword) - 1u
            && strncasecmp(buffer->data + read, keyword,
                           sizeof(keyword) - 1u) == 0) {
            memcpy(buffer->data + write, replacement, 7);
            write += 7;
            read += sizeof(keyword) - 1u;
        } else {
            buffer->data[write++] = buffer->data[read++];
        }
    }
    buffer->length = write;
    buffer->data[write] = '\0';
}

static bool external_svg_materialize_symbol(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const FetchResult *fetched, InlineSvgBuffer *output)
{
    size_t href_length = 0;
    const char *href = inline_svg_external_use_href(node, &href_length);
    if (href == NULL || fetched == NULL || fetched->data == NULL
        || output == NULL) return false;
    size_t fragment = href_length;
    while (fragment != 0 && href[fragment - 1] != '#') fragment--;
    if (fragment == 0 || fragment == href_length) return false;
    size_t attributes_start = 0, open_end = 0, close_start = 0;
    if (!external_svg_symbol_bounds(
            fetched->data, fetched->length, context->external_svg_symbols,
            href + fragment,
            href_length - fragment, &attributes_start, &open_end,
            &close_start)) return false;

    ComputedStyle style = {0};
    if (!image_style_chain_for_node(context->stylesheet, node, &style)) {
        return false;
    }
    int width = style.has_width && !style.width_percent
                && style.width_offset == 0 && style.width > 0
                && style.width <= 1024 ? style.width : 16;
    int height = style.has_height && !style.height_percent
                 && style.height > 0 && style.height <= 1024
                 ? style.height : width;
    size_t attributes_length = open_end - attributes_start;
    size_t content_start = open_end + 1u;
    size_t content_length = close_start - content_start;
    if (attributes_length > EXTERNAL_SVG_SYMBOL_TAG_LIMIT
        || content_length > EXTERNAL_SVG_SYMBOL_LIMIT) return false;
    size_t capacity = attributes_length + content_length + 192u;
    output->budget = context->budget;
    output->data = budget_malloc(context->budget, capacity);
    if (output->data == NULL) return false;
    output->capacity = capacity;
    int prefix = snprintf(
        output->data, capacity,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" "
        "height=\"%d\"%.*s>",
        width, height, (int) attributes_length,
        fetched->data + attributes_start);
    if (prefix < 0 || (size_t) prefix >= capacity
        || content_length > capacity - (size_t) prefix - 7u) {
        budget_free(context->budget, output->data);
        memset(output, 0, sizeof(*output));
        return false;
    }
    memcpy(output->data + prefix, fetched->data + content_start,
           content_length);
    output->length = (size_t) prefix + content_length;
    memcpy(output->data + output->length, "</svg>", 7u);
    output->length += 6u;
    inline_svg_resolve_current_color(output, style.color);
    return true;
}
