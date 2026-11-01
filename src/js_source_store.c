/* Function-source store (js_source_store.h): script text kept outside the
   QuickJS heap for Function.prototype.toString(). */
#include "js_source_store.h"

#include "script_text_lz.h"
#include "tilefinch/platform.h"

#include <string.h>

#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define SOURCE_STORE_TIMING 1
#else
#define SOURCE_STORE_TIMING 0
#endif

/* A block that would not shrink below this fraction (in 1/16ths) of its
   text stays plain: decoding it would cost time for little room. */
#define SOURCE_STORE_KEEP_PLAIN_SIXTEENTHS 15u

typedef struct SourceStoreBlock {
    unsigned char *data;
    /* 0: plain (data holds the block's text); else compressed size */
    uint32_t stored;
    /* plain because compressing it did not pay: never tried again */
    bool kept_plain;
    /* SOURCE_STORE_ASCII_*: found the first time a toString() asks
       (script_source_store_query()) or when the block is compressed, so
       adding a script stays a plain copy */
    uint8_t ascii;
} SourceStoreBlock;

typedef struct SourceStoreDecoded {
    unsigned char *text; /* SCRIPT_SOURCE_STORE_BLOCK_BYTES, or NULL */
    uint32_t id; /* 0: empty */
    uint32_t block;
    size_t length;
    size_t input;
    uint64_t used; /* store clock at its last read */
} SourceStoreDecoded;

typedef struct SourceStoreEntry {
    struct SourceStoreEntry *next; /* oldest first */
    uint32_t id;
    uint32_t length;
    uint32_t block_count;
    uint32_t plain_blocks;
    bool required; /* see script_source_store_require() */
    uint32_t leases; /* see script_source_store_lease() */
    bool filled; /* blocks hold the text (script_source_store_reserve()) */
    uint64_t last_read; /* store clock at add or last read */
    SourceStoreBlock blocks[];
} SourceStoreEntry;

struct ScriptSourceStore {
    Budget *budget;
    ScriptSourceStoreCompression compression;
    SourceStoreEntry *entries; /* oldest first */
    SourceStoreEntry *last;
    uint32_t next_id;
    uint64_t clock;
    /* the most recently decoded blocks, each decoded up to its length (its
       input up to its input position) */
    SourceStoreDecoded decoded[SCRIPT_SOURCE_STORE_DECODED_BLOCKS];
    ScriptSourceStoreStats stats;
};

enum { SOURCE_STORE_ASCII_UNKNOWN = 0, SOURCE_STORE_ASCII_YES,
       SOURCE_STORE_ASCII_NO };

static uint8_t text_ascii_state(const unsigned char *text, size_t length)
{
    uint64_t bits = 0;
    size_t i = 0;
    for (; i + 8u <= length; i += 8u) {
        uint64_t word;
        memcpy(&word, text + i, sizeof(word));
        bits |= word;
    }
    for (; i < length; i++) bits |= text[i];
    return (bits & UINT64_C(0x8080808080808080)) == 0
        ? SOURCE_STORE_ASCII_YES : SOURCE_STORE_ASCII_NO;
}

static uint64_t store_now_ns(void)
{
#if SOURCE_STORE_TIMING
    return tilefinch_platform_monotonic_time_ns();
#else
    return 0;
#endif
}

static uint32_t block_text_length(const SourceStoreEntry *entry,
                                  uint32_t block)
{
    uint32_t start = block * SCRIPT_SOURCE_STORE_BLOCK_BYTES;
    uint32_t left = entry->length - start;
    return left < SCRIPT_SOURCE_STORE_BLOCK_BYTES
        ? left : SCRIPT_SOURCE_STORE_BLOCK_BYTES;
}

static size_t entry_table_bytes(uint32_t block_count)
{
    return sizeof(SourceStoreEntry)
        + (size_t) block_count * sizeof(SourceStoreBlock);
}

static size_t block_held_bytes(const SourceStoreEntry *entry, uint32_t block)
{
    const SourceStoreBlock *b = &entry->blocks[block];
    return b->stored != 0 ? b->stored : block_text_length(entry, block);
}

static void store_note_held(ScriptSourceStore *store)
{
    size_t held = store->stats.held_bytes + store->stats.decode_buffer_bytes;
    if (held > store->stats.peak_held_bytes)
        store->stats.peak_held_bytes = held;
}

/* Takes plain block `block` of `entry` out of the plain-text totals. */
static void block_unaccount_plain(ScriptSourceStore *store,
                                  const SourceStoreEntry *entry,
                                  uint32_t block)
{
    const SourceStoreBlock *b = &entry->blocks[block];
    if (b->stored != 0) return;
    uint32_t length = block_text_length(entry, block);
    store->stats.plain_bytes -= length;
    if (!b->kept_plain) store->stats.compressible_plain_bytes -= length;
}

ScriptSourceStore *script_source_store_create(
    Budget *budget, ScriptSourceStoreCompression compression)
{
    if (budget == NULL) return NULL;
    ScriptSourceStore *store = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, sizeof(*store));
    if (store == NULL) return NULL;
    memset(store, 0, sizeof(*store));
    store->budget = budget;
    store->compression = compression;
    store->next_id = 1;
    return store;
}

static void store_drop_decoded(ScriptSourceStore *store)
{
    for (size_t i = 0; i < SCRIPT_SOURCE_STORE_DECODED_BLOCKS; i++) {
        budget_free(store->budget, store->decoded[i].text);
        memset(&store->decoded[i], 0, sizeof(store->decoded[i]));
    }
    store->stats.decode_buffer_bytes = 0;
}

static void entry_free(ScriptSourceStore *store, SourceStoreEntry *entry)
{
    for (uint32_t i = 0; i < entry->block_count; i++) {
        if (entry->blocks[i].data == NULL) continue;
        store->stats.held_bytes -= block_held_bytes(entry, i);
        block_unaccount_plain(store, entry, i);
        budget_free(store->budget, entry->blocks[i].data);
    }
    store->stats.held_bytes -= entry_table_bytes(entry->block_count);
    store->stats.text_bytes -= entry->length;
    store->stats.entries--;
    for (size_t i = 0; i < SCRIPT_SOURCE_STORE_DECODED_BLOCKS; i++)
        if (store->decoded[i].id == entry->id) store->decoded[i].id = 0;
    budget_free(store->budget, entry);
}

void script_source_store_destroy(ScriptSourceStore *store)
{
    if (store == NULL) return;
    SourceStoreEntry *entry = store->entries;
    while (entry != NULL) {
        SourceStoreEntry *next = entry->next;
        entry_free(store, entry);
        entry = next;
    }
    store_drop_decoded(store);
    budget_free(store->budget, store);
}

static void store_unlink(ScriptSourceStore *store, SourceStoreEntry *entry,
                         SourceStoreEntry *previous)
{
    if (previous == NULL) store->entries = entry->next;
    else previous->next = entry->next;
    if (store->last == entry) store->last = previous;
}

/* Compresses plain block `block` of `entry` with `table`; false if the
   Budget refused the output (the block stays plain). */
static bool block_compress(ScriptSourceStore *store, SourceStoreEntry *entry,
                           uint32_t block, uint32_t *table)
{
    SourceStoreBlock *b = &entry->blocks[block];
    if (b->stored != 0 || b->kept_plain) return true;
    uint32_t length = block_text_length(entry, block);
    uint64_t started = store_now_ns();
    store->stats.compress_attempts++;
    size_t capacity = script_text_lz_bound(length);
    unsigned char *out = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_JAVASCRIPT, capacity);
    if (out == NULL) return false;
    if (b->ascii == SOURCE_STORE_ASCII_UNKNOWN)
        b->ascii = text_ascii_state(b->data, length);
    size_t written = script_text_lz_compress(b->data, length, out, capacity,
                                             table);
    store->stats.compress_ns += store_now_ns() - started;
    if (written == 0
        || written > (size_t) length * SOURCE_STORE_KEEP_PLAIN_SIXTEENTHS
                         / 16u) {
        /* Incompressible: keep it plain, and do not try it again. */
        budget_free(store->budget, out);
        b->kept_plain = true;
        store->stats.compressible_plain_bytes -= length;
        return true;
    }
    unsigned char *trimmed = budget_realloc_category(
        store->budget, BUDGET_CATEGORY_JAVASCRIPT, out, written);
    if (trimmed != NULL) out = trimmed;
    budget_free(store->budget, b->data);
    b->data = out;
    b->stored = (uint32_t) written;
    entry->plain_blocks--;
    store->stats.plain_bytes -= length;
    store->stats.compressible_plain_bytes -= length;
    store->stats.held_bytes -= length;
    store->stats.held_bytes += written;
    store->stats.compressed_blocks++;
    store->stats.compressed_in_bytes += length;
    store->stats.compressed_out_bytes += written;
    return true;
}

/* Compresses plain blocks, oldest entries first, until `plain` bytes or
   fewer stay plain or none is left to try. Blocks already found
   incompressible are skipped, and with nothing left to try no match table
   is allocated. */
static void store_compress_until(ScriptSourceStore *store, size_t plain)
{
    if (store->stats.plain_bytes <= plain
        || store->stats.compressible_plain_bytes == 0) return;
    uint32_t *table = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_JAVASCRIPT, SCRIPT_TEXT_LZ_TABLE_BYTES);
    if (table == NULL) return;
    store->stats.scratch_tables++;
    for (SourceStoreEntry *entry = store->entries;
         entry != NULL && store->stats.plain_bytes > plain
             && store->stats.compressible_plain_bytes != 0;
         entry = entry->next) {
        for (uint32_t i = 0; i < entry->block_count && entry->plain_blocks
                 && store->stats.plain_bytes > plain; i++) {
            if (entry->blocks[i].stored == 0 && !entry->blocks[i].kept_plain
                && !block_compress(store, entry, i, table)) {
                budget_free(store->budget, table);
                return;
            }
        }
    }
    budget_free(store->budget, table);
}

/* A new entry for `length` bytes with no blocks yet, linked newest. */
static SourceStoreEntry *store_new_entry(ScriptSourceStore *store,
                                         size_t length)
{
    if (store == NULL || length == 0
        || length > UINT32_MAX - SCRIPT_SOURCE_STORE_BLOCK_BYTES) return NULL;
    uint32_t block_count = (uint32_t) ((length
        + SCRIPT_SOURCE_STORE_BLOCK_BYTES - 1u)
        / SCRIPT_SOURCE_STORE_BLOCK_BYTES);
    SourceStoreEntry *entry = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_JAVASCRIPT,
        entry_table_bytes(block_count));
    if (entry == NULL) {
        store->stats.refused++;
        return NULL;
    }
    memset(entry, 0, entry_table_bytes(block_count));
    entry->length = (uint32_t) length;
    entry->block_count = block_count;
    store->stats.held_bytes += entry_table_bytes(block_count);
    store->stats.text_bytes += length;
    store->stats.entries++;
    entry->id = store->next_id++;
    if (store->next_id == 0) store->next_id = 1;
    entry->last_read = ++store->clock;
    if (store->last == NULL) store->entries = entry;
    else store->last->next = entry;
    store->last = entry;
    return entry;
}

/* Copies (and under COMPRESS_EAGER compresses) the text into `entry`'s
   blocks; false if the Budget refused a block (the entry then holds
   none). */
static bool store_fill(ScriptSourceStore *store, SourceStoreEntry *entry,
                       const char *text)
{
    uint32_t *table = NULL;
    if (store->compression == SCRIPT_SOURCE_STORE_COMPRESS_EAGER) {
        table = budget_malloc_category(store->budget,
                                       BUDGET_CATEGORY_JAVASCRIPT,
                                       SCRIPT_TEXT_LZ_TABLE_BYTES);
        if (table != NULL) store->stats.scratch_tables++;
    }
    for (uint32_t i = 0; i < entry->block_count; i++) {
        uint32_t block_length = block_text_length(entry, i);
        const unsigned char *block_text = (const unsigned char *) text
            + (size_t) i * SCRIPT_SOURCE_STORE_BLOCK_BYTES;
        SourceStoreBlock *b = &entry->blocks[i];
        b->data = budget_malloc_category(store->budget,
                                         BUDGET_CATEGORY_JAVASCRIPT,
                                         block_length);
        if (b->data == NULL) goto refused;
        memcpy(b->data, block_text, block_length);
        entry->plain_blocks++;
        store->stats.plain_bytes += block_length;
        store->stats.compressible_plain_bytes += block_length;
        store->stats.held_bytes += block_length;
        if (table != NULL) (void) block_compress(store, entry, i, table);
    }
    budget_free(store->budget, table);
    entry->filled = true;
    store->stats.added++;
    store->stats.added_bytes += entry->length;
    store_note_held(store);
    return true;
refused:
    budget_free(store->budget, table);
    for (uint32_t i = 0; i < entry->block_count; i++) {
        if (entry->blocks[i].data == NULL) continue;
        store->stats.held_bytes -= block_held_bytes(entry, i);
        block_unaccount_plain(store, entry, i);
        budget_free(store->budget, entry->blocks[i].data);
        entry->blocks[i].data = NULL;
        entry->blocks[i].stored = 0;
        entry->blocks[i].kept_plain = false;
    }
    entry->plain_blocks = 0;
    store->stats.refused++;
    return false;
}

uint32_t script_source_store_add(ScriptSourceStore *store, const char *text,
                                 size_t length)
{
    if (text == NULL) return 0;
    SourceStoreEntry *entry = store_new_entry(store, length);
    if (entry == NULL) return 0;
    uint32_t id = entry->id;
    if (!store_fill(store, entry, text)) {
        script_source_store_release(store, id);
        store->stats.released--;
        return 0;
    }
    return id;
}

uint32_t script_source_store_reserve(ScriptSourceStore *store, size_t length)
{
    SourceStoreEntry *entry = store_new_entry(store, length);
    return entry == NULL ? 0 : entry->id;
}

static SourceStoreEntry *store_find(ScriptSourceStore *store, uint32_t id,
                                    SourceStoreEntry **previous)
{
    SourceStoreEntry *before = NULL;
    for (SourceStoreEntry *entry = store->entries; entry != NULL;
         before = entry, entry = entry->next) {
        if (entry->id == id) {
            if (previous != NULL) *previous = before;
            return entry;
        }
    }
    return NULL;
}

bool script_source_store_fill(ScriptSourceStore *store, uint32_t id,
                              const char *text)
{
    if (store == NULL || text == NULL) return false;
    SourceStoreEntry *entry = store_find(store, id, NULL);
    if (entry == NULL || entry->filled) return false;
    return store_fill(store, entry, text);
}

bool script_source_store_lease(ScriptSourceStore *store, uint32_t id,
                               bool take)
{
    if (store == NULL) return false;
    SourceStoreEntry *entry = store_find(store, id, NULL);
    if (take) {
        if (entry == NULL || entry->leases == UINT32_MAX) return false;
        entry->leases++;
        store->stats.leases++;
        return true;
    }
    /* The entry may be gone: the engine released it (no function refers to
       it) while the operation ran. The store-wide count still balances. */
    if (entry != NULL && entry->leases != 0) entry->leases--;
    if (store->stats.leases != 0) store->stats.leases--;
    return true;
}

void script_source_store_require(ScriptSourceStore *store, uint32_t id,
                                 bool required)
{
    if (store == NULL) return;
    SourceStoreEntry *entry = store_find(store, id, NULL);
    if (entry == NULL || entry->required == required) return;
    entry->required = required;
    if (required) store->stats.required_entries++;
    else store->stats.required_entries--;
}

void script_source_store_release(ScriptSourceStore *store, uint32_t id)
{
    if (store == NULL || id == 0) return;
    SourceStoreEntry *previous = NULL;
    SourceStoreEntry *entry = store_find(store, id, &previous);
    if (entry == NULL) return;
    if (entry->required) store->stats.required_entries--;
    store_unlink(store, entry, previous);
    entry_free(store, entry);
    store->stats.released++;
}

/* The text of compressed block `block`, decoded into the store's buffer
   at least up to `end`; NULL if the buffer cannot be allocated or the
   block does not decode. Decoding stops at `end` and a later read of the
   same block continues from there. */
static const unsigned char *store_decoded_block(ScriptSourceStore *store,
                                                SourceStoreEntry *entry,
                                                uint32_t block, uint32_t end)
{
    SourceStoreDecoded *slot = NULL, *victim = &store->decoded[0];
    for (size_t i = 0; i < SCRIPT_SOURCE_STORE_DECODED_BLOCKS; i++) {
        SourceStoreDecoded *d = &store->decoded[i];
        if (d->id == entry->id && d->block == block) { slot = d; break; }
        if (d->id == 0 ? victim->id != 0 || d->used < victim->used
                       : victim->id != 0 && d->used < victim->used)
            victim = d;
    }
    if (slot == NULL) {
        slot = victim;
        if (slot->text == NULL) {
            slot->text = budget_malloc_category(
                store->budget, BUDGET_CATEGORY_JAVASCRIPT,
                SCRIPT_SOURCE_STORE_BLOCK_BYTES);
            if (slot->text == NULL) return NULL;
            store->stats.decode_buffer_bytes += SCRIPT_SOURCE_STORE_BLOCK_BYTES;
            store_note_held(store);
        }
        slot->id = entry->id;
        slot->block = block;
        slot->length = 0;
        slot->input = 0;
        store->stats.block_decodes++;
    }
    slot->used = store->clock;
    if (slot->length >= end) return slot->text;
    uint64_t started = store_now_ns();
    const SourceStoreBlock *b = &entry->blocks[block];
    if (!script_text_lz_decompress_until(
            b->data, b->stored, slot->text,
            block_text_length(entry, block), &slot->input, &slot->length,
            end)
        || slot->length < end) {
        slot->id = 0;
        return NULL;
    }
    store->stats.decode_ns += store_now_ns() - started;
    return slot->text;
}

int script_source_store_query(ScriptSourceStore *store, uint32_t id,
                              uint32_t offset, uint32_t length)
{
    SourceStoreEntry *entry = store == NULL ? NULL
        : store_find(store, id, NULL);
    if (entry == NULL || !entry->filled || offset > entry->length
        || length > entry->length - offset) {
        if (store != NULL) store->stats.read_failures++;
        return -1;
    }
    if (length == 0) return 1;
    uint32_t first = offset / SCRIPT_SOURCE_STORE_BLOCK_BYTES;
    uint32_t last = (offset + length - 1u) / SCRIPT_SOURCE_STORE_BLOCK_BYTES;
    for (uint32_t block = first; block <= last; block++) {
        SourceStoreBlock *b = &entry->blocks[block];
        /* a compressed block was classified when it was compressed */
        if (b->ascii == SOURCE_STORE_ASCII_UNKNOWN && b->stored == 0)
            b->ascii = text_ascii_state(b->data,
                                        block_text_length(entry, block));
        if (b->ascii != SOURCE_STORE_ASCII_YES) return 0;
    }
    return 1;
}

int script_source_store_read(ScriptSourceStore *store, uint32_t id,
                             uint32_t offset, uint32_t length, char *dest)
{
    if (store == NULL) return -1;
    uint64_t started = store_now_ns();
    store->stats.reads++;
    SourceStoreEntry *entry = store_find(store, id, NULL);
    if (entry == NULL || !entry->filled || offset > entry->length
        || length > entry->length - offset || (length != 0 && dest == NULL)) {
        store->stats.read_failures++;
        return -1;
    }
    entry->last_read = ++store->clock;
    uint32_t at = offset, end = offset + length;
    while (at < end) {
        uint32_t block = at / SCRIPT_SOURCE_STORE_BLOCK_BYTES;
        uint32_t within = at - block * SCRIPT_SOURCE_STORE_BLOCK_BYTES;
        uint32_t take = block_text_length(entry, block) - within;
        if (take > end - at) take = end - at;
        const SourceStoreBlock *b = &entry->blocks[block];
        const unsigned char *text = b->stored == 0
            ? b->data : store_decoded_block(store, entry, block, within + take);
        if (text == NULL) {
            store->stats.read_failures++;
            return -1;
        }
        memcpy(dest + (at - offset), text + within, take);
        at += take;
    }
    store->stats.read_bytes += length;
    store->stats.read_ns += store_now_ns() - started;
    return 0;
}

void script_source_store_balance(ScriptSourceStore *store,
                                 size_t plain_allowance)
{
    if (store == NULL
        || store->compression != SCRIPT_SOURCE_STORE_COMPRESS_UNDER_PRESSURE)
        return;
    store_compress_until(store, plain_allowance);
}

size_t script_source_store_relieve(ScriptSourceStore *store, size_t needed)
{
    if (store == NULL || needed == 0) return 0;
    size_t before = store->stats.held_bytes + store->stats.decode_buffer_bytes;
    store_drop_decoded(store);
    if (store->compression != SCRIPT_SOURCE_STORE_COMPRESS_NEVER
        && before - store->stats.held_bytes < needed) {
        /* Compression frees at least a sixteenth of each block it takes;
           its output is a Budget allocation, never the QuickJS heap. */
        store_compress_until(store, 0);
    }
    while (before - store->stats.held_bytes < needed) {
        /* the least recently read entry no lazy function needs */
        SourceStoreEntry *victim = NULL, *victim_previous = NULL;
        SourceStoreEntry *previous = NULL;
        for (SourceStoreEntry *entry = store->entries; entry != NULL;
             previous = entry, entry = entry->next) {
            if (!entry->required && entry->leases == 0
                && (victim == NULL || entry->last_read < victim->last_read)) {
                victim = entry;
                victim_previous = previous;
            }
        }
        if (victim == NULL) break;
        store_unlink(store, victim, victim_previous);
        store->stats.evicted++;
        store->stats.evicted_text_bytes += victim->length;
        entry_free(store, victim);
    }
    size_t after = store->stats.held_bytes;
    return before > after ? before - after : 0;
}

size_t script_source_store_text_bytes(const ScriptSourceStore *store)
{
    return store == NULL ? 0 : store->stats.text_bytes;
}

void script_source_store_stats(const ScriptSourceStore *store,
                               ScriptSourceStoreStats *stats)
{
    if (stats == NULL) return;
    if (store == NULL) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    *stats = store->stats;
}
