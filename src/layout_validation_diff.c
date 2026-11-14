/* Validation-only measurement for the layout subtree reuse study (see
   layout_validation_diff.h). Shipping PSP builds compile this file empty. */
#include "layout_validation_diff.h"

#ifdef TILEFINCH_LAYOUT_DIFF

#include "tilefinch/platform.h"
#include "tilefinch/document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *diff_cause = "other";
static char diff_note[320];
static const LayoutDocument *diff_reference;
/* The box array the reference held when it was registered. Layouts move by
   value (a navigation commit copies the whole page, image placement takes
   the boxes), so the arrays can be freed through another owner while the
   registered struct still names them: forget matches the array as well as
   the address, and a report reads the struct only while it still holds
   that array. */
static const LayoutNodeBox *diff_reference_boxes;

static bool diff_enabled(void)
{
#ifdef __PSP__
    return true;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_LAYOUT_DIFF") != NULL;
    return enabled != 0;
#endif
}

bool layout_validation_enabled(void)
{
    return diff_enabled();
}

void layout_validation_set_cause(const char *cause)
{
    diff_cause = cause == NULL ? "other" : cause;
}

void layout_validation_note(const char *note)
{
    if (note == NULL) { diff_note[0] = '\0'; return; }
    size_t used = strlen(diff_note);
    if (used + 2u >= sizeof(diff_note)) return;
    snprintf(diff_note + used, sizeof(diff_note) - used, "%s%s",
             used == 0 ? "" : ",", note);
}

static uint32_t diff_mix(uint32_t hash, uint32_t value)
{
    hash ^= value;
    hash *= UINT32_C(16777619);
    return hash;
}

/* Everything a command paints except its position. */
/* loose leaves out the ImageResource pointer: a reused subtree would rebind
   its image commands (the image table can move), so a box that differs only
   there counts as reusable with a rebind. */
static uint32_t diff_command_shape_mode(const DrawCommand *command, bool loose)
{
    uint32_t hash = UINT32_C(2166136261);
    hash = diff_mix(hash, command->type);
    hash = diff_mix(hash, (uint32_t) command->width);
    hash = diff_mix(hash, (uint32_t) command->height);
    hash = diff_mix(hash, command->color);
    hash = diff_mix(hash, (uint32_t) command->radius);
    hash = diff_mix(hash, (uint32_t) command->z_index);
    hash = diff_mix(hash, (uint32_t) command->scale);
    hash = diff_mix(hash, (uint32_t) command->font_size);
    hash = diff_mix(hash, command->opacity_scale);
    hash = diff_mix(hash, (uint32_t) command->font_family
        | (uint32_t) command->font_weight << 8
        | (uint32_t) command->font_italic << 16
        | (uint32_t) (uint8_t) command->letter_spacing << 24);
    hash = diff_mix(hash, command->image_fit);
    hash = diff_mix(hash, command->text_length);
    hash = loose ? diff_mix(hash, command->image != NULL)
                 : diff_mix(hash, (uint32_t) (uintptr_t) command->image);
    if (command->type == DRAW_TEXT && command->text != NULL
        && command->text_length != 0) {
        size_t length = command->text_length > 256u
            ? 256u : command->text_length;
        for (size_t i = 0; i < length; i++)
            hash = diff_mix(hash, (unsigned char) command->text[i]);
    } else {
        hash = diff_mix(hash, (uint32_t) (uintptr_t) command->text);
    }
    return hash;
}

static uint32_t diff_command_shape(const DrawCommand *command)
{
    return diff_command_shape_mode(command, false);
}

typedef struct {
    int x, y;
    uint32_t shape;
    uint32_t loose;
} DiffCommand;

typedef struct {
    const void *node;
    int x, y, width, height;
    uint32_t command_start, command_end;
} DiffBox;

/* The reference as the comparison reads it: a live layout, or (host) a copy
   of the last published one. */
typedef struct {
    const LayoutDocument *live;
    DiffBox *boxes;
    size_t box_count;
    DiffCommand *commands;
    size_t command_count;
    uint32_t *index;     /* box index + 1, open addressing by node */
    size_t index_capacity;
    unsigned char *matched;
} DiffReference;

static size_t diff_pointer_hash(const void *pointer)
{
    uintptr_t value = (uintptr_t) pointer;
    value ^= value >> 15;
    value *= (uintptr_t) UINT32_C(0x2c1b3c6d);
    value ^= value >> 12;
    return (size_t) value;
}

#ifndef __PSP__
static DiffReference diff_snapshot;

static void diff_snapshot_clear(void)
{
    free(diff_snapshot.boxes);
    free(diff_snapshot.commands);
    free(diff_snapshot.index);
    memset(&diff_snapshot, 0, sizeof(diff_snapshot));
}

static void diff_snapshot_take(const LayoutDocument *layout)
{
    diff_snapshot_clear();
    if (layout == NULL || layout->node_box_count == 0) return;
    size_t boxes = layout->node_box_count, commands = layout->count;
    size_t capacity = 64;
    while (capacity < boxes * 2u) capacity *= 2u;
    diff_snapshot.boxes = malloc(boxes * sizeof(*diff_snapshot.boxes));
    diff_snapshot.commands = malloc(
        (commands == 0 ? 1 : commands) * sizeof(*diff_snapshot.commands));
    diff_snapshot.index = calloc(capacity, sizeof(*diff_snapshot.index));
    if (diff_snapshot.boxes == NULL || diff_snapshot.commands == NULL
        || diff_snapshot.index == NULL) {
        diff_snapshot_clear();
        return;
    }
    diff_snapshot.index_capacity = capacity;
    for (size_t i = 0; i < boxes; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        diff_snapshot.boxes[i] = (DiffBox) {
            .node = box->node, .x = box->x, .y = box->y,
            .width = box->width, .height = box->height,
            .command_start = box->command_start,
            .command_end = box->command_end
        };
        if (box->node == NULL) continue;
        size_t slot = diff_pointer_hash(box->node) & (capacity - 1u);
        while (diff_snapshot.index[slot] != 0) {
            if (diff_snapshot.boxes[diff_snapshot.index[slot] - 1u].node
                == box->node) break;
            slot = (slot + 1u) & (capacity - 1u);
        }
        diff_snapshot.index[slot] = (uint32_t) (i + 1u);
    }
    for (size_t i = 0; i < commands; i++) {
        diff_snapshot.commands[i] = (DiffCommand) {
            .x = layout->commands[i].x, .y = layout->commands[i].y,
            .shape = diff_command_shape(&layout->commands[i]),
            .loose = diff_command_shape_mode(&layout->commands[i], true)
        };
    }
    diff_snapshot.box_count = boxes;
    diff_snapshot.command_count = commands;
}
#endif

void layout_validation_set_reference(const LayoutDocument *layout)
{
    if (!diff_enabled()) return;
    diff_reference_boxes = layout == NULL ? NULL : layout->node_boxes;
    diff_reference = diff_reference_boxes == NULL ? NULL : layout;
#ifndef __PSP__
    diff_snapshot_take(layout);
#endif
}

void layout_validation_forget(const LayoutDocument *layout)
{
    if (layout == NULL || diff_reference == NULL) return;
    if (layout == diff_reference
        || (layout->node_boxes != NULL
            && layout->node_boxes == diff_reference_boxes)) {
        diff_reference = NULL;
        diff_reference_boxes = NULL;
    }
}

static bool diff_reference_box(const DiffReference *reference,
                               const void *node, DiffBox *out,
                               size_t *index)
{
    if (reference->live != NULL) {
        const LayoutNodeBox *box = layout_box_for_node(
            reference->live, (const lxb_dom_node_t *) node);
        if (box == NULL) return false;
        *out = (DiffBox) {
            .node = box->node, .x = box->x, .y = box->y,
            .width = box->width, .height = box->height,
            .command_start = box->command_start,
            .command_end = box->command_end
        };
        *index = (size_t) (box - reference->live->node_boxes);
        return true;
    }
    if (reference->index_capacity == 0) return false;
    size_t mask = reference->index_capacity - 1u;
    size_t slot = diff_pointer_hash(node) & mask;
    while (reference->index[slot] != 0) {
        const DiffBox *box = &reference->boxes[reference->index[slot] - 1u];
        if (box->node == node) {
            *out = *box;
            *index = reference->index[slot] - 1u;
            return true;
        }
        slot = (slot + 1u) & mask;
    }
    return false;
}

static DiffCommand diff_reference_command(const DiffReference *reference,
                                          size_t index)
{
    if (reference->live != NULL) {
        const DrawCommand *command = &reference->live->commands[index];
        return (DiffCommand) {
            .x = command->x, .y = command->y,
            .shape = diff_command_shape(command),
            .loose = diff_command_shape_mode(command, true)
        };
    }
    return reference->commands[index];
}

static size_t diff_reference_commands(const DiffReference *reference)
{
    return reference->live != NULL ? reference->live->count
                                   : reference->command_count;
}

enum {
    DIFF_NEW = 0,
    DIFF_RESIZED,
    DIFF_CONTENT,
    DIFF_REBIND,
    DIFF_MOVED,
    DIFF_SAME
};

#ifndef __PSP__
/* Host flow time attribution: node -> nanoseconds for one owner build. */
typedef struct {
    const void *node;
    uint64_t ns;
} DiffTime;

static DiffTime *diff_time;
static size_t diff_time_capacity;
static size_t diff_time_count;
static bool diff_time_overflow;
static const LayoutDocument *diff_time_owner;
static const void *diff_time_last_node;
static uint64_t diff_time_last_ns;

void layout_validation_time_begin(const LayoutDocument *owner)
{
    if (!diff_enabled()) return;
    if (diff_time == NULL) {
        diff_time_capacity = (size_t) 1u << 17;
        diff_time = calloc(diff_time_capacity, sizeof(*diff_time));
        if (diff_time == NULL) return;
    } else {
        memset(diff_time, 0, diff_time_capacity * sizeof(*diff_time));
    }
    diff_time_count = 0;
    diff_time_overflow = false;
    diff_time_owner = owner;
    diff_time_last_node = NULL;
    diff_time_last_ns = tilefinch_platform_monotonic_time_ns();
}

static DiffTime *diff_time_slot(const void *node)
{
    size_t mask = diff_time_capacity - 1u;
    size_t slot = diff_pointer_hash(node) & mask;
    while (diff_time[slot].node != NULL && diff_time[slot].node != node)
        slot = (slot + 1u) & mask;
    return &diff_time[slot];
}

void layout_validation_time_node(const LayoutDocument *owner,
                                 const void *node)
{
    if (diff_time == NULL || owner != diff_time_owner) return;
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    if (diff_time_last_node != NULL) {
        DiffTime *slot = diff_time_slot(diff_time_last_node);
        if (slot->node == NULL) {
            if (diff_time_count * 2u >= diff_time_capacity) {
                diff_time_overflow = true;
                goto next;
            }
            slot->node = diff_time_last_node;
            diff_time_count++;
        }
        slot->ns += now - diff_time_last_ns;
    }
next:
    diff_time_last_node = node;
    diff_time_last_ns = now;
}
#endif

static size_t diff_box_index_of(const LayoutDocument *fresh,
                                const lxb_dom_node_t *node)
{
    const LayoutNodeBox *box = layout_box_for_node(fresh, node);
    return box == NULL ? SIZE_MAX : (size_t) (box - fresh->node_boxes);
}

/* The nearest ancestor box of box i (by DOM parent chain), or SIZE_MAX. */
static size_t diff_parent_box(const LayoutDocument *fresh, size_t i)
{
    const lxb_dom_node_t *node = fresh->node_boxes[i].node;
    for (node = node == NULL ? NULL : node->parent; node != NULL;
         node = node->parent) {
        size_t at = diff_box_index_of(fresh, node);
        if (at != SIZE_MAX && at != i) return at;
    }
    return SIZE_MAX;
}

/* 1 when box i or an ancestor box is equal to the reference up to a
   translation. state: 0 unknown, 1 not covered, 2 covered. */
static bool diff_covered(const LayoutDocument *fresh,
                         const unsigned char *status, unsigned char *state,
                         size_t *parents, size_t i)
{
    size_t chain[512];
    size_t depth = 0;
    size_t at = i;
    bool covered = false;
    while (at != SIZE_MAX) {
        if (state[at] != 0) { covered = state[at] == 2; break; }
        if (status[at] >= DIFF_REBIND) { covered = true; state[at] = 2; break; }
        if (depth < sizeof(chain) / sizeof(chain[0])) chain[depth++] = at;
        if (parents[at] == SIZE_MAX - 1u)
            parents[at] = diff_parent_box(fresh, at);
        at = parents[at];
    }
    while (depth != 0) state[chain[--depth]] = covered ? 2 : 1;
    return state[i] == 2;
}

#ifndef __PSP__
static bool diff_verbose(void)
{
    static int verbose = -1;
    if (verbose < 0) {
        const char *level = getenv("TILEFINCH_LAYOUT_DIFF");
        verbose = level != NULL && strcmp(level, "2") == 0;
    }
    return verbose != 0;
}

static size_t diff_roots_printed;

static void diff_print_node(const char *label, const lxb_dom_node_t *node)
{
    size_t tag_length = 0, id_length = 0, class_length = 0;
    const char *tag = node == NULL ? NULL
        : document_element_name(node, &tag_length);
    const char *id = node == NULL ? NULL
        : document_attribute(node, "id", &id_length);
    const char *klass = node == NULL ? NULL
        : document_attribute(node, "class", &class_length);
    fprintf(stderr, " %s=%.*s#%.*s.%.*s", label,
            (int) (tag_length > 24 ? 24 : tag_length), tag == NULL ? "" : tag,
            (int) (id_length > 24 ? 24 : id_length), id == NULL ? "" : id,
            (int) (class_length > 48 ? 48 : class_length),
            klass == NULL ? "" : klass);
}

/* Verbose: the outermost reusable boxes and the box they sit in, which
   says which formatting context a reuse pass would have to work in. */
static void diff_print_root(const LayoutDocument *fresh,
                            const LayoutNodeBox *box, size_t parent)
{
    if (!diff_verbose() || diff_roots_printed++ >= 24) return;
    fprintf(stderr, "  layout-diff-root commands=%u box=%d,%d %dx%d",
            box->command_end - box->command_start, box->x, box->y,
            box->width, box->height);
    diff_print_node("node", box->node);
    diff_print_node("parent", parent == SIZE_MAX ? NULL
                    : fresh->node_boxes[parent].node);
    fputc('\n', stderr);
}
#endif

void layout_validation_report_build(const LayoutDocument *fresh)
{
#ifndef __PSP__
    diff_roots_printed = 0;
#endif
    const char *cause = diff_cause;
    diff_cause = "other";
    if (!diff_enabled() || fresh == NULL) { diff_note[0] = '\0'; return; }
    uint64_t diff_started = tilefinch_platform_monotonic_time_us();
    DiffReference reference = {0};
    if (diff_reference != NULL && diff_reference != fresh
        && diff_reference->node_boxes == diff_reference_boxes
        && diff_reference->node_box_count != 0) {
        reference.live = diff_reference;
        reference.box_count = diff_reference->node_box_count;
    }
#ifndef __PSP__
    else if (diff_snapshot.box_count != 0) {
        reference = diff_snapshot;
    }
#endif
    size_t count = fresh->node_box_count;
    size_t counts[6] = {0};
    size_t covered = 0, roots = 0, roots_moved = 0, roots_dx = 0;
    size_t covered_commands = 0, gone = 0, matched = 0;
    uint64_t time_covered = 0, time_dirty = 0, time_unboxed = 0;
    unsigned char *status = count == 0 ? NULL : calloc(count, 1);
    unsigned char *state = count == 0 ? NULL : calloc(count, 1);
    size_t *parents = count == 0 ? NULL : malloc(count * sizeof(*parents));
    unsigned char *seen = reference.box_count == 0 ? NULL
        : calloc(reference.box_count, 1);
    bool complete = count != 0 && status != NULL && state != NULL
        && parents != NULL && (reference.box_count == 0 || seen != NULL);
    size_t reference_commands = diff_reference_commands(&reference);
    for (size_t i = 0; complete && i < count; i++) {
        parents[i] = SIZE_MAX - 1u;
        const LayoutNodeBox *box = &fresh->node_boxes[i];
        DiffBox old;
        size_t old_index = 0;
        if (box->node == NULL || reference.box_count == 0
            || !diff_reference_box(&reference, box->node, &old, &old_index)) {
            status[i] = DIFF_NEW;
            continue;
        }
        if (!seen[old_index]) { seen[old_index] = 1; matched++; }
        if (old.width != box->width || old.height != box->height) {
            status[i] = DIFF_RESIZED;
            continue;
        }
        int dx = box->x - old.x, dy = box->y - old.y;
        size_t length = box->command_end >= box->command_start
            ? box->command_end - box->command_start : 0;
        size_t old_length = old.command_end >= old.command_start
            ? old.command_end - old.command_start : 0;
        bool equal = length == old_length
            && box->command_end <= fresh->count
            && old.command_end <= reference_commands;
        bool exact = equal;
        for (size_t k = 0; equal && k < length; k++) {
            const DrawCommand *now =
                &fresh->commands[box->command_start + k];
            DiffCommand then = diff_reference_command(
                &reference, old.command_start + k);
            equal = now->x - dx == then.x && now->y - dy == then.y
                && diff_command_shape_mode(now, true) == then.loose;
            if (equal && exact) exact = diff_command_shape(now) == then.shape;
        }
        status[i] = !equal ? DIFF_CONTENT
            : !exact ? DIFF_REBIND
            : (dx == 0 && dy == 0 ? DIFF_SAME : DIFF_MOVED);
    }
    for (size_t i = 0; complete && i < count; i++) {
        counts[status[i]]++;
        if (!diff_covered(fresh, status, state, parents, i)) continue;
        covered++;
        if (status[i] < DIFF_REBIND) continue;
        size_t parent = parents[i];
        if (parent == SIZE_MAX - 1u) parent = parents[i] =
            diff_parent_box(fresh, i);
        bool parent_covered = parent != SIZE_MAX
            && diff_covered(fresh, status, state, parents, parent);
        if (parent_covered) continue;
        roots++;
        const LayoutNodeBox *box = &fresh->node_boxes[i];
#ifndef __PSP__
        diff_print_root(fresh, box, parent);
#endif
        covered_commands += box->command_end >= box->command_start
            ? box->command_end - box->command_start : 0;
        if (status[i] == DIFF_MOVED) {
            roots_moved++;
            DiffBox old;
            size_t old_index = 0;
            if (diff_reference_box(&reference, box->node, &old, &old_index)
                && old.x != box->x) roots_dx++;
        }
    }
    gone = reference.box_count > matched ? reference.box_count - matched : 0;
    /* Where the change starts: dirty boxes no dirty box lies inside. */
    size_t origins = 0, origins_new = 0, origins_resized = 0;
    unsigned char *dirty_child = complete ? calloc(count, 1) : NULL;
    for (size_t i = 0; dirty_child != NULL && i < count; i++) {
        if (state[i] == 2) continue;
        size_t parent = parents[i];
        if (parent == SIZE_MAX - 1u) parent = parents[i] =
            diff_parent_box(fresh, i);
        if (parent != SIZE_MAX) dirty_child[parent] = 1;
    }
#ifndef __PSP__
    static int verbose = -1;
    if (verbose < 0) {
        const char *level = getenv("TILEFINCH_LAYOUT_DIFF");
        verbose = level != NULL && strcmp(level, "2") == 0;
    }
    size_t printed = 0;
#endif
    for (size_t i = 0; dirty_child != NULL && i < count; i++) {
        if (state[i] == 2 || dirty_child[i]) continue;
        origins++;
        if (status[i] == DIFF_NEW) origins_new++;
        if (status[i] == DIFF_RESIZED) origins_resized++;
#ifndef __PSP__
        if (verbose && printed++ < 24) {
            const LayoutNodeBox *box = &fresh->node_boxes[i];
            size_t tag_length = 0, id_length = 0, class_length = 0;
            const char *tag = document_element_name(box->node, &tag_length);
            const char *id = document_attribute(box->node, "id", &id_length);
            const char *klass = document_attribute(box->node, "class",
                                                   &class_length);
            DiffBox old = {0};
            size_t old_index = 0;
            bool had = diff_reference_box(&reference, box->node, &old,
                                          &old_index);
            fprintf(stderr, "  layout-diff-origin status=%d node=%.*s#%.*s.%.*s "
                    "box=%d,%d %dx%d was=%d,%d %dx%d commands=%u\n",
                    (int) status[i], (int) (tag_length > 16 ? 16 : tag_length),
                    tag == NULL ? "" : tag,
                    (int) (id_length > 24 ? 24 : id_length), id == NULL ? "" : id,
                    (int) (class_length > 40 ? 40 : class_length),
                    klass == NULL ? "" : klass, box->x, box->y, box->width,
                    box->height, had ? old.x : -1, had ? old.y : -1,
                    had ? old.width : -1, had ? old.height : -1,
                    box->command_end - box->command_start);
        }
#endif
    }
    free(dirty_child);
#ifndef __PSP__
    bool timed = complete && diff_time != NULL && diff_time_owner == fresh;
    for (size_t s = 0; timed && s < diff_time_capacity; s++) {
        if (diff_time[s].node == NULL) continue;
        const lxb_dom_node_t *node = diff_time[s].node;
        size_t at = SIZE_MAX;
        for (; node != NULL && at == SIZE_MAX; node = node->parent)
            at = diff_box_index_of(fresh, node);
        if (at == SIZE_MAX) time_unboxed += diff_time[s].ns;
        else if (state[at] == 2) time_covered += diff_time[s].ns;
        else time_dirty += diff_time[s].ns;
    }
    if (diff_time_owner == fresh) diff_time_owner = NULL;
#endif
    const LayoutPerformance *p = &fresh->performance;
    char message[720];
    snprintf(message, sizeof(message),
        "tilefinch-layout-diff: cause=%s ref=%s boxes=%zu same=%zu moved=%zu "
        "rebind=%zu resized=%zu content=%zu new=%zu gone=%zu covered=%zu roots=%zu "
        "roots-moved=%zu roots-dx=%zu commands=%zu covered-commands=%zu "
        "total-us=%llu flow-us=%llu style-us=%llu styles=%llu/%llu "
        "compact-us=%llu focus-us=%llu paint-us=%llu spatial-us=%llu "
        "finalize-us=%llu work=%zu origins=%zu/%zu/%zu time-covered-us=%llu time-dirty-us=%llu "
        "time-unboxed-us=%llu diff-us=%llu complete=%d note=%s",
        cause,
        reference.live != NULL ? "live"
            : (reference.box_count != 0 ? "snapshot" : "none"),
        count, counts[DIFF_SAME], counts[DIFF_MOVED], counts[DIFF_REBIND],
        counts[DIFF_RESIZED],
        counts[DIFF_CONTENT], counts[DIFF_NEW], gone, covered, roots,
        roots_moved, roots_dx, fresh->count, covered_commands,
        (unsigned long long) p->total_us, (unsigned long long) p->flow_us,
        (unsigned long long) p->style_resolve_us,
        (unsigned long long) p->style_cache_hits,
        (unsigned long long) p->style_cache_misses,
        (unsigned long long) p->compact_us,
        (unsigned long long) p->focus_index_us,
        (unsigned long long) p->paint_order_us,
        (unsigned long long) p->spatial_index_us,
        (unsigned long long) p->finalize_us,
        fresh->layout_work_units, origins, origins_new, origins_resized,
        (unsigned long long) (time_covered / 1000u),
        (unsigned long long) (time_dirty / 1000u),
        (unsigned long long) (time_unboxed / 1000u),
        (unsigned long long) (tilefinch_platform_monotonic_time_us()
                              - diff_started),
        (int) complete, diff_note[0] == '\0' ? "-" : diff_note);
    tilefinch_platform_log_message(message);
    diff_note[0] = '\0';
    free(status);
    free(state);
    free(parents);
    free(seen);
}

#endif
