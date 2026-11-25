/* A cache that follows a streaming parse (included by layout_reuse.c).

   The streaming preview's cache serves styles across builds while the
   parser keeps adding to the tree, and no journal records what it added.
   Tree construction appends at the current node, the open element the
   parser hands the cache before each build (layout_reuse_cache_parser_at),
   or at one of its open ancestors once it closes: every addition lands
   after the last child some element on the path from the document to that
   node (the frontier) had. The current node is not always on the document's
   last-child chain: an element foster-parented out of a table sits before
   the table, and after </body> a comment follows <body>. So the cache
   records that path, and each element's last child, at each build
   (layout_reuse_cache_prepare), and before the next one restyles what
   content arriving after it can move, as a script append after the last
   sibling would (layout_reuse_cache_invalidate_appended):

     - the parent's :empty, when it had no nonempty child;
     - the earlier siblings' tests counting from the end (:last-child,
       :last-of-type, :nth-last-*, :only-*) and what reads them through a
       sibling or descendant combinator, when one can sit there (the
       trailing keys);
     - the :has() answers the new children can move;
     - the measurements of every element whose content grew, text the
       parser merged into the node before a table included.

   Anything else at a frontier element (a child inserted before the path
   or before its last child by foster parenting, a move by the adoption
   agency, an attribute the parser merged into <html> or <body>, a subtree
   a script freed) restyles that element's children, or its whole subtree
   when the path below it is gone. */

#define LAYOUT_REUSE_FRONTIER_DEPTH 128u
/* New children noted one by one for :has(); past this the parent is
   noted once for all of them. */
#define LAYOUT_REUSE_FRONTIER_NOTE_LIMIT 64u
/* Children walked back from the last one to find the recorded one. */
#define LAYOUT_REUSE_FRONTIER_WALK_LIMIT 4096u

struct LayoutReuseParserFrontier {
    lxb_dom_node_t *nodes[LAYOUT_REUSE_FRONTIER_DEPTH];
    /* Each recorded node's previous sibling (with its data length when it
       is text, SIZE_MAX otherwise), an element's last attribute, and its
       last child and that child's previous sibling, when recorded. Only
       compared, never read: a recorded node is read once a walk from the
       root reaches it. */
    const lxb_dom_node_t *previous[LAYOUT_REUSE_FRONTIER_DEPTH];
    size_t previous_text[LAYOUT_REUSE_FRONTIER_DEPTH];
    const void *attributes[LAYOUT_REUSE_FRONTIER_DEPTH];
    lxb_dom_node_t *last[LAYOUT_REUSE_FRONTIER_DEPTH];
    const lxb_dom_node_t *last_previous[LAYOUT_REUSE_FRONTIER_DEPTH];
    lxb_dom_node_t *root;
    /* The parser's current node for the next record only. */
    lxb_dom_node_t *current;
    size_t depth;
    /* The leaf's data length, when it is text. */
    size_t text_length;
    /* The leaf had children past the depth bound. */
    bool cut;
    /* A script freed the recorded child of the leaf. */
    bool removed;
};

bool layout_reuse_cache_follow_parser(LayoutReuseCache *cache,
                                      lxb_dom_node_t *root)
{
    if (cache == NULL || root == NULL) return false;
    if (cache->parser == NULL) {
        cache->parser = budget_calloc(cache->budget, 1,
                                      sizeof(*cache->parser));
        if (cache->parser == NULL) return false;
        cache->stats.retained_bytes += sizeof(*cache->parser);
    }
    if (cache->parser->root != root) {
        memset(cache->parser, 0, sizeof(*cache->parser));
        cache->parser->root = root;
    }
    cache->parser_settling = false;
    return true;
}

void layout_reuse_cache_parser_at(LayoutReuseCache *cache,
                                  lxb_dom_node_t *current)
{
    if (cache != NULL && cache->parser != NULL)
        cache->parser->current = current;
}

static void layout_reuse_parser_release(LayoutReuseCache *cache)
{
    cache->parser_settling = false;
    if (cache->parser == NULL) return;
    cache->stats.retained_bytes -= sizeof(*cache->parser);
    budget_free(cache->budget, cache->parser);
    cache->parser = NULL;
}

static const void *layout_frontier_attributes(const lxb_dom_node_t *node)
{
    return node->type == LXB_DOM_NODE_TYPE_ELEMENT
        ? (const void *) lxb_dom_interface_element(node)->last_attr : NULL;
}

static size_t layout_frontier_text_length(const lxb_dom_node_t *node)
{
    size_t length = SIZE_MAX;
    if (node != NULL && node->type == LXB_DOM_NODE_TYPE_TEXT)
        (void) document_text_data((lxb_dom_node_t *) node, &length);
    return length;
}

/* The path from the root to the parser's current node, then on down the
   last-child chain (all of it when no current node was handed over, or it
   is not below the root). */
static void layout_frontier_record(LayoutReuseParserFrontier *frontier)
{
    frontier->depth = 0;
    frontier->cut = frontier->removed = false;
    frontier->text_length = 0;
    lxb_dom_node_t *current = frontier->current;
    frontier->current = NULL;
    size_t above = 0;
    const lxb_dom_node_t *up = current;
    while (up != NULL && up != frontier->root
           && above < LAYOUT_REUSE_FRONTIER_WALK_LIMIT) {
        above++;
        up = up->parent;
    }
    lxb_dom_node_t *at = frontier->root;
    if (current != NULL && up == frontier->root) {
        /* above + 1 nodes, root first; past the bound only the top ones. */
        size_t total = above + 1u;
        size_t index = total;
        for (lxb_dom_node_t *node = current; index != 0;
             node = node->parent) {
            index--;
            if (index < LAYOUT_REUSE_FRONTIER_DEPTH)
                frontier->nodes[index] = node;
        }
        if (total > LAYOUT_REUSE_FRONTIER_DEPTH) {
            frontier->depth = LAYOUT_REUSE_FRONTIER_DEPTH;
            at = frontier->nodes[LAYOUT_REUSE_FRONTIER_DEPTH - 1u]
                ->last_child;
        } else {
            frontier->depth = total - 1u;
            at = current;
        }
    }
    for (; at != NULL && frontier->depth < LAYOUT_REUSE_FRONTIER_DEPTH;
         at = at->last_child)
        frontier->nodes[frontier->depth++] = at;
    if (frontier->depth == LAYOUT_REUSE_FRONTIER_DEPTH)
        frontier->cut = frontier->nodes[frontier->depth - 1u]->last_child
            != NULL;
    for (size_t i = 0; i < frontier->depth; i++) {
        lxb_dom_node_t *node = frontier->nodes[i];
        frontier->previous[i] = node->prev;
        frontier->previous_text[i] = layout_frontier_text_length(node->prev);
        frontier->attributes[i] = layout_frontier_attributes(node);
        frontier->last[i] = node->last_child;
        frontier->last_previous[i] = node->last_child == NULL
            ? NULL : node->last_child->prev;
    }
    lxb_dom_node_t *leaf = frontier->nodes[frontier->depth - 1u];
    if (leaf->type == LXB_DOM_NODE_TYPE_TEXT)
        (void) document_text_data(leaf, &frontier->text_length);
}

/* Whether `child` is still a child of `parent` after the same previous
   sibling. Only `parent`'s live children are followed: a recorded node
   that left (or was freed) is compared, never read. */
static bool layout_frontier_in_place(const lxb_dom_node_t *parent,
                                     const lxb_dom_node_t *child,
                                     const lxb_dom_node_t *previous)
{
    size_t walked = 0;
    for (const lxb_dom_node_t *at = parent->last_child; at != NULL;
         at = at->prev) {
        if (++walked > LAYOUT_REUSE_FRONTIER_WALK_LIMIT) return false;
        if (at == child) return at->prev == previous;
    }
    return false;
}

/* Whether an element or nonempty text among `last` and its earlier
   siblings already defeated `parent`'s :empty. */
static bool layout_frontier_was_nonempty(lxb_dom_node_t *last)
{
    size_t walked = 0;
    for (lxb_dom_node_t *at = last; at != NULL; at = at->prev) {
        if (++walked > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT) return false;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) return true;
        if (at->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            (void) document_text_data(at, &length);
            if (length != 0) return true;
        }
    }
    return false;
}

/* Children arrived under `parent` after `last` (NULL: its first ones).
   `moved` when a node may have moved in from elsewhere this period: the
   new children's subtrees then go too. False when a :has() note reset the
   cache. */
static bool layout_reuse_parser_appended(LayoutReuseCache *cache,
                                         const LayoutSheetFacts *facts,
                                         lxb_dom_node_t *parent,
                                         lxb_dom_node_t *last, bool moved)
{
    lxb_dom_node_t *first = last == NULL ? parent->first_child : last->next;
    bool was_nonempty = last != NULL && layout_frontier_was_nonempty(last);
    bool element = false, nonempty = was_nonempty;
    size_t notes = 0;
    for (lxb_dom_node_t *child = first; child != NULL; child = child->next) {
        bool is_element = child->type == LXB_DOM_NODE_TYPE_ELEMENT;
        /* Text is visible to selectors only through :empty: the first
           nonempty text of an empty parent. */
        bool visible = is_element;
        if (!is_element && !nonempty
            && child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            (void) document_text_data(child, &length);
            visible = length != 0;
        }
        element = element || is_element;
        nonempty = nonempty || visible;
        if (moved && is_element) layout_reuse_drop_subtree(cache, child, true);
        if (!visible || !cache->selector_has_has
            || notes > LAYOUT_REUSE_FRONTIER_NOTE_LIMIT) continue;
        if (facts != NULL
            && !style_has_argument_keys_reach(facts->has_arguments,
                                              is_element ? child : NULL,
                                              parent)) continue;
        lxb_dom_node_t *noted = ++notes > LAYOUT_REUSE_FRONTIER_NOTE_LIMIT
            ? parent : child;
        if (!layout_reuse_has_note(cache, noted, true, NULL, NULL, 0))
            return false;
    }
    bool siblings = false;
    /* The earliest earlier sibling a test counting from the end can sit
       on, when only the siblings' own keys place such tests. */
    lxb_dom_node_t *from = NULL;
    if (element && last != NULL) {
        siblings = facts == NULL;
        size_t count = 0;
        int ancestors = -1;
        for (lxb_dom_node_t *earlier = last;
             !siblings && facts != NULL && facts->structure
             && earlier != NULL; earlier = earlier->prev) {
            if (earlier->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++count > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT) {
                siblings = true;
            } else if (layout_trailing_reach(facts->trailing, earlier,
                                             parent, &ancestors)) {
                from = earlier;
                siblings = ancestors != 0;
            }
        }
        if (siblings) from = NULL;
        else siblings = from != NULL;
    }
    bool earlier_element = false;
    for (const lxb_dom_node_t *at = from == NULL ? NULL : from->prev;
         at != NULL && !earlier_element; at = at->prev)
        earlier_element = at->type == LXB_DOM_NODE_TYPE_ELEMENT;
    if (siblings && earlier_element && was_nonempty) {
        /* Appended children move only the answers of tests counting from
           the end, and none sits before `from`: it and the siblings after
           it restyle, with their subtrees (what reads them through `+`,
           `~` or a descendant combinator). */
        layout_reuse_drop_element(cache, parent, true);
        for (lxb_dom_node_t *child = from; child != NULL;
             child = child->next) {
            if (child->type == LXB_DOM_NODE_TYPE_ELEMENT)
                layout_reuse_drop_subtree(cache, child, true);
        }
        layout_reuse_drop_structural_readers(cache, parent);
        layout_reuse_cache_invalidate_measurements(cache, parent);
    } else if (siblings) {
        /* As an insertion a sibling test can see (invalidate_tree). */
        layout_reuse_invalidate_child_list(cache, parent, NULL, NULL, false,
                                           true, false);
    } else if (nonempty && !was_nonempty) {
        /* Its :empty flipped. */
        if (parent->parent != NULL
            && layout_reuse_empty_reaches_siblings(cache, parent)) {
            layout_reuse_drop_subtree(cache, parent->parent, true);
            layout_reuse_drop_measurements(cache, parent->parent);
        } else {
            layout_reuse_drop_element(cache, parent, true);
        }
        layout_reuse_drop_structural_readers(cache, parent);
    }
    return true;
}

/* Below `node` (a frontier element) the recorded path is gone or was
   reordered: a child moved out or in (the adoption agency), was inserted
   before the path (foster parenting), or was freed, or the path ran past
   the depth bound. Its subtree restyles; outside it, only what its
   :empty and :has() answers reach can move. False when a :has() note
   reset the cache. */
static bool layout_reuse_parser_disordered(LayoutReuseCache *cache,
                                           lxb_dom_node_t *node)
{
    if (!layout_reuse_has_note(cache, node, true, NULL, NULL, 0))
        return false;
    lxb_dom_node_t *scope = node->parent != NULL
        && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT
        && layout_reuse_empty_reaches_siblings(cache, node)
        ? node->parent : node;
    layout_reuse_drop_subtree(cache, scope, true);
    layout_reuse_drop_measurements(cache, scope);
    return true;
}

/* Restyles what the parser added since the frontier was recorded (see the
   top of this file), when the cache keeps its styles across this build;
   then records the frontier again. */
static void layout_reuse_parser_advance(LayoutReuseCache *cache,
                                        bool keeps_styles)
{
    LayoutReuseParserFrontier *frontier = cache->parser;
    if (frontier == NULL || frontier->root == NULL) return;
    if (!keeps_styles || frontier->depth == 0) {
        if (cache->parser_settling) layout_reuse_parser_release(cache);
        else layout_frontier_record(frontier);
        return;
    }
    LayoutSheetFacts facts_storage;
    const LayoutSheetFacts *facts =
        layout_sheet_facts(cache, cache->sheet, &facts_storage)
            ? &facts_storage : NULL;
    /* The first level whose recorded child on the path, or recorded last
       child, left or had a sibling inserted before it; the path below it
       is not followed. */
    size_t levels = frontier->depth;
    bool disorder = frontier->cut || frontier->removed;
    for (size_t i = 0; i + 1u < frontier->depth; i++) {
        if (layout_frontier_in_place(frontier->nodes[i],
                                     frontier->nodes[i + 1u],
                                     frontier->previous[i + 1u])
            && (frontier->last[i] == frontier->nodes[i + 1u]
                || layout_frontier_in_place(frontier->nodes[i],
                                            frontier->last[i],
                                            frontier->last_previous[i])))
            continue;
        levels = i + 1u;
        disorder = true;
        break;
    }
    /* Every :has() note considers all entries (no journal probe ran). */
    uint64_t entries = cache->structure_entries;
    uint32_t serial = cache->structure_serial;
    cache->structure_entries = UINT64_MAX;
    cache->structure_serial = 0;
    /* The deepest node whose content changed: its measurements and its
       ancestors' go. */
    lxb_dom_node_t *grown = NULL;
    bool kept = true;
    for (size_t i = 0; i < levels && kept; i++) {
        lxb_dom_node_t *node = frontier->nodes[i];
        bool leaf = i + 1u == levels;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && layout_frontier_attributes(node) != frontier->attributes[i]) {
            /* An attribute the parser merged into <html> or <body>. */
            layout_reuse_cache_invalidate_relational(cache, node, NULL,
                                                     false);
            grown = node;
            kept = cache->sheet != NULL;
        }
        if (!kept) break;
        if (leaf && disorder) {
            kept = layout_reuse_parser_disordered(cache, node);
            grown = node;
            break;
        }
        if (leaf && node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            (void) document_text_data(node, &length);
            if (length == frontier->text_length) break;
            /* The text's own measurements, and its ancestors' below. */
            layout_reuse_drop_measurements(cache, node);
            grown = node->parent;
            /* Text that was empty: the parent's :empty. */
            if (frontier->text_length == 0 && grown != NULL)
                kept = layout_reuse_parser_disordered(cache, grown);
            break;
        }
        if (!leaf) {
            /* Text the parser merged into the node before the path's next
               one (foster parenting before a table); nothing restyles. */
            lxb_dom_node_t *before = frontier->nodes[i + 1u]->prev;
            if (layout_frontier_text_length(before)
                != frontier->previous_text[i + 1u]) {
                layout_reuse_drop_measurements(cache, before);
                grown = node;
            }
        }
        lxb_dom_node_t *last = frontier->last[i];
        if ((last == NULL ? node->first_child : last->next) == NULL)
            continue;
        kept = layout_reuse_parser_appended(cache, facts, node, last,
                                            disorder);
        grown = node;
    }
    if (grown != NULL && kept) {
        layout_reuse_clear_counters(cache);
        for (size_t i = 0; i < LAYOUT_REUSE_INTRINSIC_CAPACITY; i++) {
            LayoutIntrinsicCacheEntry *entry = &cache->intrinsic[i];
            if (entry->node != NULL
                && layout_node_is_within(grown, entry->node))
                memset(entry, 0, sizeof(*entry));
        }
        for (size_t i = 0; cache->table_rows != NULL
                           && i < LAYOUT_REUSE_TABLE_ROW_CAPACITY; i++) {
            LayoutReuseTableRowEntry *entry = &cache->table_rows[i];
            if (entry->row != NULL && layout_node_is_within(grown, entry->row))
                memset(entry, 0, sizeof(*entry));
        }
    }
    if (grown != NULL) cache->stats.parser_growths++;
    cache->structure_entries = entries;
    cache->structure_serial = serial;
    layout_reuse_cache_flush_invalidations(cache);
    if (cache->parser_settling) layout_reuse_parser_release(cache);
    else layout_frontier_record(frontier);
}

void layout_reuse_cache_settle_parser(LayoutReuseCache *cache)
{
    if (cache == NULL || cache->parser == NULL) return;
    cache->parser->current = NULL;
    cache->parser_settling = true;
}

/* A style read outside a build, before the build that settles the parse:
   settle first, keeping the styles only when they were resolved against
   `sheet` and `fonts` as bound. */
static void layout_reuse_parser_settle_for_read(LayoutReuseCache *cache,
                                                const Stylesheet *sheet,
                                                const FontSet *fonts)
{
    layout_reuse_parser_advance(
        cache, sheet != NULL && cache->sheet == sheet
                   && cache->sheet_generation == sheet->build_generation
                   && cache->fonts == fonts);
}

/* `root` is about to be freed: the frontier keeps only the path above it,
   and the next build restyles the parent it left (the walk never follows
   a recorded pointer, so this keeps addresses a later node reuses from
   being taken for the recorded ones). */
static void layout_reuse_parser_retire(LayoutReuseCache *cache,
                                       const lxb_dom_node_t *root)
{
    LayoutReuseParserFrontier *frontier = cache->parser;
    if (frontier == NULL) return;
    /* A handed-over current node may be inside `root`. */
    frontier->current = NULL;
    /* The shallowest element whose recorded children include `root`. */
    size_t depth = SIZE_MAX;
    for (size_t i = 0; i < frontier->depth && depth == SIZE_MAX; i++) {
        if (i != 0 && (frontier->nodes[i] == root
                       || frontier->previous[i] == root)) depth = i;
        else if (frontier->last[i] == root
                 || frontier->last_previous[i] == root) depth = i + 1u;
    }
    if (depth == SIZE_MAX) return;
    frontier->depth = depth;
    frontier->removed = true;
    frontier->cut = false;
}
