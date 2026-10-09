#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"

#include <string.h>

#define budget_calloc(b, n, s) \
    budget_calloc_category((b), BUDGET_CATEGORY_DOM, (n), (s))

#define DOCUMENT_CSSOM_ANCESTOR_LIMIT 65536u
#define DOCUMENT_CSSOM_SHEET_LIMIT SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX

/* Constructed stylesheets and adoptedStyleSheets lists (document.h).

   The registry owns no DOM: a sheet is a detached <style> element whose
   lifetime the script bridge manages (pinned while any list names it), and
   a list's root is the shadow-root carrier element the bridge created. The
   registry records which elements those are, so the stylesheet builders can
   parse each active sheet once, and drops what a destroyed subtree takes
   with it before the addresses can be reused. One Budget block, allocated
   on the first adoption and freed with the document. */

typedef struct {
    lxb_dom_node_t *node;
    uint32_t revision;
    /* Lists naming this sheet. */
    uint32_t references;
    size_t text_bytes;
    /* The sheet's parsed form at `cache_revision`, kept across stylesheet
       rebuilds (document_constructed_sheet_cache_store): a structural IR
       and a compiled selector fragment, either may be NULL. */
    unsigned char *ir;
    size_t ir_bytes;
    unsigned char *fragment;
    size_t fragment_bytes;
    uint32_t cache_revision;
    /* Parses of the current text so far: the form is kept from the second
       on, so a sheet that is parsed once never pays for a cache. */
    uint32_t parses;
} DocumentConstructedSheet;

typedef struct {
    /* NULL: the document itself. */
    lxb_dom_node_t *root;
    uint8_t count;
    lxb_dom_node_t *sheets[DOCUMENT_ADOPTED_SHEETS_PER_ROOT];
} DocumentAdoptionList;

struct DocumentAdoptedSheets {
    DocumentConstructedSheet sheets[DOCUMENT_CONSTRUCTED_SHEET_LIMIT];
    size_t sheet_count;
    /* In the order each root's list was first set: the cascade order. */
    DocumentAdoptionList lists[DOCUMENT_ADOPTION_ROOT_LIMIT];
    size_t list_count;
    /* Indices into `lists` sorted by root address, for the per-node
       carrier lookups mutation classification makes. */
    uint8_t by_root[DOCUMENT_ADOPTION_ROOT_LIMIT];
    size_t text_bytes;
    /* Bytes of every sheet's cached parsed form. */
    size_t cache_bytes;
    /* Lists whose root is a shadow-root carrier (not the document). */
    size_t shadow_lists;
};

static void adopted_cache_release(const PocDocument *document,
                                  DocumentAdoptedSheets *registry,
                                  DocumentConstructedSheet *entry)
{
    registry->cache_bytes -= entry->ir_bytes + entry->fragment_bytes;
    budget_free(document->budget, entry->ir);
    budget_free(document->budget, entry->fragment);
    entry->ir = entry->fragment = NULL;
    entry->ir_bytes = entry->fragment_bytes = 0;
}

static DocumentAdoptedSheets *adopted_ensure(PocDocument *document)
{
    if (document == NULL || document->budget == NULL) return NULL;
    if (document->adopted_sheets != NULL) return document->adopted_sheets;
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    document->adopted_sheets = budget_calloc(
        document->budget, 1, sizeof(*document->adopted_sheets));
    document_allocation_owner_leave(document, previous);
    return document->adopted_sheets;
}

static DocumentConstructedSheet *adopted_sheet_find(
    const DocumentAdoptedSheets *registry, const lxb_dom_node_t *node)
{
    if (registry == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < registry->sheet_count; i++) {
        if (registry->sheets[i].node == node)
            return (DocumentConstructedSheet *) &registry->sheets[i];
    }
    return NULL;
}

/* Position of `root` in by_root, or where it would be inserted. */
static size_t adopted_root_position(const DocumentAdoptedSheets *registry,
                                    const lxb_dom_node_t *root, bool *found)
{
    size_t low = 0, high = registry->list_count;
    *found = false;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uintptr_t at = (uintptr_t)
            registry->lists[registry->by_root[middle]].root;
        if (at == (uintptr_t) root) {
            *found = true;
            return middle;
        }
        if (at < (uintptr_t) root) low = middle + 1u;
        else high = middle;
    }
    return low;
}

static DocumentAdoptionList *adopted_list_find(
    const DocumentAdoptedSheets *registry, const lxb_dom_node_t *root)
{
    if (registry == NULL || registry->list_count == 0) return NULL;
    bool found = false;
    size_t position = adopted_root_position(registry, root, &found);
    return found ? (DocumentAdoptionList *)
        &registry->lists[registry->by_root[position]] : NULL;
}

static void adopted_reindex(DocumentAdoptedSheets *registry)
{
    /* At most DOCUMENT_ADOPTION_ROOT_LIMIT entries: insertion sort. */
    for (size_t i = 0; i < registry->list_count; i++)
        registry->by_root[i] = (uint8_t) i;
    for (size_t i = 1; i < registry->list_count; i++) {
        uint8_t value = registry->by_root[i];
        uintptr_t key = (uintptr_t) registry->lists[value].root;
        size_t j = i;
        while (j > 0 && (uintptr_t)
               registry->lists[registry->by_root[j - 1u]].root > key) {
            registry->by_root[j] = registry->by_root[j - 1u];
            j--;
        }
        registry->by_root[j] = value;
    }
}

static bool node_connected(const lxb_dom_node_t *node)
{
    size_t depth = 0;
    for (const lxb_dom_node_t *at = node;
         at != NULL && depth++ < DOCUMENT_CSSOM_ANCESTOR_LIMIT; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return true;
    }
    return false;
}

static bool node_inside(const lxb_dom_node_t *node,
                        const lxb_dom_node_t *root)
{
    size_t depth = 0;
    for (const lxb_dom_node_t *at = node;
         at != NULL && depth++ < DOCUMENT_CSSOM_ANCESTOR_LIMIT; at = at->parent) {
        if (at == root) return true;
    }
    return false;
}

typedef struct DocumentCssomSheet {
    struct DocumentCssomSheet *next;
    lxb_dom_node_t *node;
    char *text;
    size_t length, capacity;
    uint32_t revision;
    bool active;
} DocumentCssomSheet;

struct DocumentCssomSheets {
    Budget *budget;
    DocumentCssomSheet *head;
    size_t count, bytes, capacity;
};

static DocumentCssomSheet *cssom_find(const lxb_dom_node_t *node)
{
    struct DocumentCssomSheets *registry = document_cssom_registry(node);
    if (registry == NULL) return NULL;
    size_t visited = 0;
    for (DocumentCssomSheet *entry = registry->head;
         entry != NULL && visited++ < DOCUMENT_CSSOM_SHEET_LIMIT;
         entry = entry->next)
        if (entry->node == node) return entry;
    return NULL;
}

const char *document_cssom_sheet_text(const lxb_dom_node_t *node,
                                     size_t *length)
{
    DocumentCssomSheet *entry = cssom_find(node);
    if (length != NULL) *length = entry != NULL && entry->active
        ? entry->length : 0;
    return entry != NULL && entry->active ? entry->text : NULL;
}

uint32_t document_cssom_sheet_revision(const lxb_dom_node_t *node)
{
    DocumentCssomSheet *entry = cssom_find(node);
    return entry == NULL ? 0 : entry->revision;
}

bool document_cssom_sheet_set_text(PocDocument *document, lxb_dom_node_t *node,
                                   const char *text, size_t length,
                                   bool constructed, bool *active)
{
    if (active != NULL) *active = false;
    if (document == NULL || document->budget == NULL || node == NULL
        || document->html == NULL
        || node->owner_document != &document->html->dom_document
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->local_name != LXB_TAG_STYLE
        || node->ns != LXB_NS_HTML
        || (text == NULL && length != 0)
        || length > DOCUMENT_CONSTRUCTED_TEXT_LIMIT) return false;
    struct DocumentCssomSheets *registry = document->cssom_sheets;
    DocumentCssomSheet *entry = cssom_find(node);
    size_t current = registry == NULL ? 0 : registry->bytes;
    if (entry != NULL) current -= entry->length;
    if (current > document->budget->limit
        || length > document->budget->limit - current
        || (entry == NULL && registry != NULL
            && registry->count == DOCUMENT_CSSOM_SHEET_LIMIT)
        || (constructed && !document_constructed_sheet_text_fits(
                document, node, length))) return false;
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    bool new_registry = registry == NULL, new_entry = entry == NULL;
    bool new_routing_registry = document->nonce_registry == NULL;
    if (new_registry) {
        registry = budget_calloc(document->budget, 1, sizeof(*registry));
        if (registry != NULL) registry->budget = document->budget;
    }
    if (registry != NULL && new_entry)
        entry = budget_calloc(document->budget, 1, sizeof(*entry));
    size_t capacity = entry == NULL ? 0 : entry->capacity;
    char *replacement = NULL;
    if (entry != NULL && length + 1u > capacity) {
        capacity = capacity == 0 ? 128u : capacity;
        while (capacity < length + 1u
               && capacity < DOCUMENT_CONSTRUCTED_TEXT_LIMIT + 1u)
            capacity = capacity > (DOCUMENT_CONSTRUCTED_TEXT_LIMIT + 1u) / 2u
                ? DOCUMENT_CONSTRUCTED_TEXT_LIMIT + 1u : capacity * 2u;
        size_t held = registry->capacity - entry->capacity;
        if (held <= document->budget->limit
            && capacity <= document->budget->limit - held)
            replacement = budget_malloc_category(
                document->budget, BUDGET_CATEGORY_STYLE, capacity);
    }
    bool ok = registry != NULL && entry != NULL
        && (length + 1u <= entry->capacity || replacement != NULL);
    if (ok && new_registry)
        ok = document_cssom_registry_attach(document, registry);
    if (ok && constructed)
        ok = document_constructed_sheet_note_text(
            document, node, length, active);
    if (ok) {
        if (new_registry) document->cssom_sheets = registry;
        if (new_entry) {
            entry->node = node;
            entry->next = registry->head;
            registry->head = entry;
            registry->count++;
        }
        if (replacement != NULL) {
            registry->capacity -= entry->capacity;
            registry->capacity += capacity;
            budget_free(document->budget, entry->text);
            entry->text = replacement;
            entry->capacity = capacity;
        }
        registry->bytes = current + length;
        if (length != 0) memcpy(entry->text, text, length);
        entry->text[length] = '\0';
        entry->length = length;
        entry->active = true;
        entry->revision++;
        if (!constructed && active != NULL) *active = node_connected(node);
    } else {
        budget_free(document->budget, replacement);
        if (new_entry) budget_free(document->budget, entry);
        if (new_registry) {
            if (registry != NULL)
                document_cssom_registry_detach(document, new_routing_registry);
            budget_free(document->budget, registry);
        }
    }
    document_allocation_owner_leave(document, previous);
    return ok;
}

void document_cssom_sheet_reset(const lxb_dom_node_t *node)
{
    if (document_cssom_registry(node) == NULL) return;
    for (size_t depth = 0; node != NULL
         && depth < DOCUMENT_CSSOM_ANCESTOR_LIMIT; depth++, node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || node->local_name != LXB_TAG_STYLE) continue;
        DocumentCssomSheet *entry = cssom_find(node);
        if (entry == NULL) return;
        struct DocumentCssomSheets *registry = document_cssom_registry(node);
        registry->bytes -= entry->length;
        entry->length = 0;
        entry->active = false;
        entry->revision++;
        return;
    }
}

void document_cssom_discard_subtree(PocDocument *document,
                                    const lxb_dom_node_t *root)
{
    struct DocumentCssomSheets *registry = document == NULL
        ? NULL : document->cssom_sheets;
    if (registry == NULL || root == NULL) return;
    DocumentCssomSheet **link = &registry->head;
    size_t visited = 0;
    while (*link != NULL && visited++ < DOCUMENT_CSSOM_SHEET_LIMIT) {
        DocumentCssomSheet *entry = *link;
        if (!node_inside(entry->node, root)) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        registry->bytes -= entry->length;
        registry->capacity -= entry->capacity;
        registry->count--;
        budget_free(registry->budget, entry->text);
        budget_free(registry->budget, entry);
    }
}

void document_cssom_sheets_destroy(PocDocument *document)
{
    struct DocumentCssomSheets *registry = document == NULL
        ? NULL : document->cssom_sheets;
    if (registry == NULL) return;
    DocumentCssomSheet *entry = registry->head;
    for (size_t visited = 0; entry != NULL
         && visited < DOCUMENT_CSSOM_SHEET_LIMIT; visited++) {
        DocumentCssomSheet *next = entry->next;
        budget_free(registry->budget, entry->text);
        budget_free(registry->budget, entry);
        entry = next;
    }
    document_cssom_registry_detach(document, false);
    budget_free(registry->budget, registry);
    document->cssom_sheets = NULL;
}

static bool list_active(const DocumentAdoptionList *list)
{
    return list->count != 0
        && (list->root == NULL || node_connected(list->root));
}

static bool list_names(const DocumentAdoptionList *list,
                       const lxb_dom_node_t *sheet)
{
    for (size_t i = 0; i < list->count; i++) {
        if (list->sheets[i] == sheet) return true;
    }
    return false;
}

static bool sheet_active(const DocumentAdoptedSheets *registry,
                         const lxb_dom_node_t *sheet)
{
    for (size_t i = 0; i < registry->list_count; i++) {
        const DocumentAdoptionList *list = &registry->lists[i];
        if (list_names(list, sheet) && list_active(list)) return true;
    }
    return false;
}

bool document_constructed_sheet_text_fits(const PocDocument *document,
                                          const lxb_dom_node_t *sheet,
                                          size_t text_bytes)
{
    const DocumentAdoptedSheets *registry =
        document == NULL ? NULL : document->adopted_sheets;
    size_t current = registry == NULL ? 0 : registry->text_bytes;
    const DocumentConstructedSheet *entry = adopted_sheet_find(registry, sheet);
    if (entry != NULL) current -= entry->text_bytes;
    return text_bytes <= DOCUMENT_CONSTRUCTED_TEXT_LIMIT - current;
}

bool document_constructed_sheet_note_text(PocDocument *document,
                                          lxb_dom_node_t *sheet,
                                          size_t text_bytes, bool *active)
{
    if (active != NULL) *active = false;
    if (sheet == NULL
        || !document_constructed_sheet_text_fits(document, sheet, text_bytes))
        return false;
    DocumentAdoptedSheets *registry = adopted_ensure(document);
    if (registry == NULL) return false;
    DocumentConstructedSheet *entry = adopted_sheet_find(registry, sheet);
    if (entry == NULL) {
        if (registry->sheet_count == DOCUMENT_CONSTRUCTED_SHEET_LIMIT)
            return false;
        entry = &registry->sheets[registry->sheet_count++];
        *entry = (DocumentConstructedSheet) {.node = sheet};
    }
    registry->text_bytes -= entry->text_bytes;
    registry->text_bytes += text_bytes;
    entry->text_bytes = text_bytes;
    entry->revision++;
    entry->parses = 0;
    /* The parsed form is of the old text. */
    adopted_cache_release(document, registry, entry);
    if (active != NULL) *active = sheet_active(registry, sheet);
    return true;
}

bool document_constructed_sheet_known(const PocDocument *document,
                                      const lxb_dom_node_t *node)
{
    return document != NULL
        && adopted_sheet_find(document->adopted_sheets, node) != NULL;
}

bool document_constructed_sheet_adopted(const PocDocument *document,
                                        const lxb_dom_node_t *sheet)
{
    const DocumentConstructedSheet *entry = document == NULL ? NULL
        : adopted_sheet_find(document->adopted_sheets, sheet);
    return entry != NULL && entry->references != 0;
}

static void adopted_list_remove(DocumentAdoptedSheets *registry,
                                size_t index)
{
    if (registry->lists[index].root != NULL) registry->shadow_lists--;
    memmove(&registry->lists[index], &registry->lists[index + 1u],
            (registry->list_count - index - 1u)
                * sizeof(registry->lists[0]));
    registry->list_count--;
    adopted_reindex(registry);
}

/* Drops one reference from each sheet `list` names, reporting those no
   list names any more. */
static size_t adopted_release_list(DocumentAdoptedSheets *registry,
                                   const DocumentAdoptionList *list,
                                   lxb_dom_node_t **released,
                                   size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < list->count; i++) {
        DocumentConstructedSheet *entry =
            adopted_sheet_find(registry, list->sheets[i]);
        if (entry == NULL || entry->references == 0) continue;
        entry->references--;
        if (entry->references == 0 && released != NULL && count < capacity)
            released[count++] = entry->node;
    }
    return count;
}

bool document_adoption_set(PocDocument *document, lxb_dom_node_t *root,
                           lxb_dom_node_t *const *sheets, size_t count,
                           lxb_dom_node_t **released,
                           size_t *released_count)
{
    if (released_count != NULL) *released_count = 0;
    if (document == NULL || count > DOCUMENT_ADOPTED_SHEETS_PER_ROOT
        || (sheets == NULL && count != 0)) return false;
    DocumentAdoptedSheets *registry = count == 0
        ? document->adopted_sheets : adopted_ensure(document);
    if (registry == NULL) return count == 0;
    for (size_t i = 0; i < count; i++) {
        if (adopted_sheet_find(registry, sheets[i]) == NULL) return false;
        for (size_t j = 0; j < i; j++) {
            if (sheets[j] == sheets[i]) return false;
        }
    }
    bool found = false;
    size_t position = adopted_root_position(registry, root, &found);
    if (!found && count == 0) return true;
    /* An emptied list stays registered until its root is destroyed: the
       registry then vouches that an address it knows is a live node. */
    if (!found && registry->list_count == DOCUMENT_ADOPTION_ROOT_LIMIT)
        return false;
    /* Reference the new list before releasing the old one, so a sheet both
       name never reaches zero. */
    for (size_t i = 0; i < count; i++)
        adopted_sheet_find(registry, sheets[i])->references++;
    size_t index = 0;
    if (found) {
        index = registry->by_root[position];
        size_t freed = adopted_release_list(
            registry, &registry->lists[index], released,
            DOCUMENT_ADOPTED_SHEETS_PER_ROOT);
        if (released_count != NULL) *released_count = freed;
    } else {
        index = registry->list_count++;
        registry->lists[index] = (DocumentAdoptionList) {.root = root};
        if (root != NULL) registry->shadow_lists++;
        adopted_reindex(registry);
    }
    DocumentAdoptionList *list = &registry->lists[index];
    list->count = (uint8_t) count;
    for (size_t i = 0; i < count; i++) list->sheets[i] = sheets[i];
    return true;
}

bool document_adoption_root_has_sheets(const PocDocument *document,
                                       const lxb_dom_node_t *node)
{
    const DocumentAdoptionList *list = document == NULL || node == NULL
        ? NULL : adopted_list_find(document->adopted_sheets, node);
    return list != NULL && list->count != 0;
}

bool document_adoption_root_known(const PocDocument *document,
                                  const lxb_dom_node_t *root)
{
    return document != NULL && root != NULL
        && adopted_list_find(document->adopted_sheets, root) != NULL;
}

bool document_adoption_shadow_roots_present(const PocDocument *document)
{
    return document != NULL && document->adopted_sheets != NULL
        && document->adopted_sheets->shadow_lists != 0;
}

size_t document_adopted_sheets_active(const PocDocument *document,
                                      lxb_dom_node_t **sheets,
                                      uint32_t *revisions, size_t capacity)
{
    const DocumentAdoptedSheets *registry =
        document == NULL ? NULL : document->adopted_sheets;
    if (registry == NULL) return 0;
    size_t count = 0;
    for (size_t i = 0; i < registry->list_count; i++) {
        const DocumentAdoptionList *list = &registry->lists[i];
        if (!list_active(list)) continue;
        for (size_t j = 0; j < list->count; j++) {
            lxb_dom_node_t *sheet = list->sheets[j];
            bool seen = false;
            for (size_t k = 0; k < count && !seen; k++) {
                seen = sheets[k] == sheet;
            }
            if (seen) continue;
            if (count == capacity) return SIZE_MAX;
            const DocumentConstructedSheet *entry =
                adopted_sheet_find(registry, sheet);
            sheets[count] = sheet;
            if (revisions != NULL)
                revisions[count] = entry == NULL ? 0 : entry->revision;
            count++;
        }
    }
    return count;
}

uint64_t document_adopted_tier_hash(uint64_t hash,
                                    const lxb_dom_node_t *sheet,
                                    uint32_t revision)
{
    hash = (hash ^ (uint64_t) (uintptr_t) sheet)
        * UINT64_C(1099511628211);
    hash = (hash ^ revision) * UINT64_C(1099511628211);
    return hash == 0 ? 1 : hash;
}

uint64_t document_adopted_sheets_signature(const PocDocument *document)
{
    const DocumentAdoptedSheets *registry =
        document == NULL ? NULL : document->adopted_sheets;
    if (registry == NULL) return 0;
    uint64_t hash = 0;
    for (size_t i = 0; i < registry->list_count; i++) {
        const DocumentAdoptionList *list = &registry->lists[i];
        if (!list_active(list)) continue;
        /* The root separates lists: scope is part of the identity. */
        hash = document_adopted_tier_hash(hash, list->root, list->count);
        for (size_t j = 0; j < list->count; j++) {
            const DocumentConstructedSheet *entry =
                adopted_sheet_find(registry, list->sheets[j]);
            hash = document_adopted_tier_hash(
                hash, list->sheets[j], entry == NULL ? 0 : entry->revision);
        }
    }
    return hash;
}

bool document_adoption_list_at(const PocDocument *document, size_t index,
                               const lxb_dom_node_t **root,
                               lxb_dom_node_t *const **sheets, size_t *count,
                               bool *active)
{
    const DocumentAdoptedSheets *registry =
        document == NULL ? NULL : document->adopted_sheets;
    if (registry == NULL || index >= registry->list_count) return false;
    const DocumentAdoptionList *list = &registry->lists[index];
    if (root != NULL) *root = list->root;
    if (sheets != NULL) *sheets = list->sheets;
    if (count != NULL) *count = list->count;
    if (active != NULL) *active = list_active(list);
    return true;
}

size_t document_adoptions_discard_subtree(PocDocument *document,
                                          const lxb_dom_node_t *root,
                                          lxb_dom_node_t **released,
                                          size_t capacity)
{
    document_cssom_discard_subtree(document, root);
    DocumentAdoptedSheets *registry =
        document == NULL ? NULL : document->adopted_sheets;
    if (registry == NULL || root == NULL) return 0;
    size_t count = 0;
    for (size_t i = registry->list_count; i > 0; i--) {
        DocumentAdoptionList *list = &registry->lists[i - 1u];
        if (list->root == NULL || !node_inside(list->root, root)) continue;
        count += adopted_release_list(registry, list,
                                      released == NULL ? NULL
                                          : released + count,
                                      capacity - count);
        adopted_list_remove(registry, i - 1u);
    }
    /* A sheet inside the subtree goes with it. The bridge pins adopted
       sheets, so it is normally unadopted already; if not, no list may keep
       naming the address. */
    for (size_t i = registry->sheet_count; i > 0; i--) {
        DocumentConstructedSheet *entry = &registry->sheets[i - 1u];
        if (!node_inside(entry->node, root)) continue;
        for (size_t l = registry->list_count; l > 0; l--) {
            DocumentAdoptionList *list = &registry->lists[l - 1u];
            size_t write = 0;
            for (size_t s = 0; s < list->count; s++) {
                if (list->sheets[s] != entry->node)
                    list->sheets[write++] = list->sheets[s];
            }
            list->count = (uint8_t) write;
            if (write == 0) adopted_list_remove(registry, l - 1u);
        }
        for (size_t r = 0; released != NULL && r < count; r++) {
            if (released[r] == entry->node) {
                released[r] = released[--count];
                break;
            }
        }
        registry->text_bytes -= entry->text_bytes;
        adopted_cache_release(document, registry, entry);
        *entry = registry->sheets[--registry->sheet_count];
    }
    return count;
}

size_t document_constructed_sheet_count(const PocDocument *document)
{
    return document == NULL || document->adopted_sheets == NULL ? 0
        : document->adopted_sheets->sheet_count;
}

bool document_constructed_sheet_cache(const PocDocument *document,
                                      const lxb_dom_node_t *sheet,
                                      uint32_t revision,
                                      const unsigned char **ir,
                                      size_t *ir_bytes,
                                      const unsigned char **fragment,
                                      size_t *fragment_bytes)
{
    const DocumentConstructedSheet *entry = document == NULL ? NULL
        : adopted_sheet_find(document->adopted_sheets, sheet);
    bool cached = entry != NULL && entry->cache_revision == revision
        && (entry->ir != NULL || entry->fragment != NULL);
    *ir = cached ? entry->ir : NULL;
    *ir_bytes = cached ? entry->ir_bytes : 0;
    *fragment = cached ? entry->fragment : NULL;
    *fragment_bytes = cached ? entry->fragment_bytes : 0;
    return cached;
}

static unsigned char *adopted_cache_copy(const PocDocument *document,
                                         const unsigned char *data,
                                         size_t bytes)
{
    if (data == NULL || bytes == 0) return NULL;
    /* Owned by the document, whatever transaction built the stylesheet:
       a rolled-back build must not take the cache with it. */
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    unsigned char *copy = budget_malloc_category(
        document->budget, BUDGET_CATEGORY_STYLE, bytes);
    document_allocation_owner_leave(document, previous);
    if (copy != NULL) memcpy(copy, data, bytes);
    return copy;
}

bool document_constructed_sheet_cache_store(const PocDocument *document,
                                            const lxb_dom_node_t *sheet,
                                            uint32_t revision,
                                            const unsigned char *ir,
                                            size_t ir_bytes,
                                            const unsigned char *fragment,
                                            size_t fragment_bytes)
{
    DocumentAdoptedSheets *registry =
        document == NULL ? NULL : document->adopted_sheets;
    DocumentConstructedSheet *entry = adopted_sheet_find(registry, sheet);
    if (entry == NULL || entry->revision != revision) return false;
    if (ir == NULL) ir_bytes = 0;
    if (fragment == NULL) fragment_bytes = 0;
    if (ir_bytes == 0 && fragment_bytes == 0) return false;
    /* The IR already held at this revision may be passed back to keep it
       beside a new fragment. */
    unsigned char *kept_ir = NULL;
    if (ir != NULL && ir == entry->ir && entry->cache_revision == revision) {
        kept_ir = entry->ir;
        registry->cache_bytes -= entry->ir_bytes;
        entry->ir = NULL;
        entry->ir_bytes = 0;
    }
    adopted_cache_release(document, registry, entry);
    if (ir_bytes > DOCUMENT_CONSTRUCTED_CACHE_LIMIT - registry->cache_bytes) {
        budget_free(document->budget, kept_ir);
        kept_ir = NULL;
        ir_bytes = 0;
    } else if (fragment_bytes > DOCUMENT_CONSTRUCTED_CACHE_LIMIT
                   - registry->cache_bytes - ir_bytes) {
        fragment_bytes = 0;
    }
    unsigned char *ir_copy = kept_ir != NULL ? kept_ir
        : adopted_cache_copy(document, ir, ir_bytes);
    unsigned char *fragment_copy =
        adopted_cache_copy(document, fragment, fragment_bytes);
    entry->ir = ir_copy;
    entry->ir_bytes = ir_copy == NULL ? 0 : ir_bytes;
    entry->fragment = fragment_copy;
    entry->fragment_bytes = fragment_copy == NULL ? 0 : fragment_bytes;
    entry->cache_revision = revision;
    registry->cache_bytes += entry->ir_bytes + entry->fragment_bytes;
    return entry->ir != NULL || entry->fragment != NULL;
}

bool document_constructed_sheet_note_parse(const PocDocument *document,
                                           const lxb_dom_node_t *sheet,
                                           uint32_t revision)
{
    DocumentConstructedSheet *entry = document == NULL ? NULL
        : adopted_sheet_find(document->adopted_sheets, sheet);
    if (entry == NULL || entry->revision != revision) return false;
    if (entry->parses != UINT32_MAX) entry->parses++;
    return entry->parses > 1u;
}

size_t document_constructed_sheet_cache_bytes(const PocDocument *document)
{
    return document == NULL || document->adopted_sheets == NULL ? 0
        : document->adopted_sheets->cache_bytes;
}

void document_adopted_sheets_destroy(PocDocument *document)
{
    if (document == NULL || document->adopted_sheets == NULL) return;
    for (size_t i = 0; i < document->adopted_sheets->sheet_count; i++)
        adopted_cache_release(document, document->adopted_sheets,
                              &document->adopted_sheets->sheets[i]);
    budget_free(document->budget, document->adopted_sheets);
    document->adopted_sheets = NULL;
}
