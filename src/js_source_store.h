#ifndef TILEFINCH_JS_SOURCE_STORE_H
#define TILEFINCH_JS_SOURCE_STORE_H

/*
 * Function-source store: the text of classic scripts compiled without
 * Function.prototype.toString() text in the JavaScript heap
 * (JS_STRIP_SOURCE), kept in the page Budget outside the QuickJS heap so
 * toString() can still answer exactly (JS_SetExternalSourceHooks()). See
 * docs/ARCHITECTURE.md, "Function source outside the heap".
 *
 * Each script's text is split into SCRIPT_SOURCE_STORE_BLOCK_BYTES blocks,
 * each held either as a plain copy or as an independent LZ block
 * (script_text_lz.h). A read decodes only the blocks it spans, each only
 * up to where the read ends; the last few decoded blocks are kept for later
 * reads. Entries are counted in
 * BUDGET_CATEGORY_JAVASCRIPT. Under memory pressure plain blocks are
 * compressed, and then whole entries no uncompiled lazy function needs
 * (script_source_store_require()) and no compile or restore in progress
 * holds (script_source_store_lease()) are dropped, least recently read first;
 * a read of a dropped entry fails (toString() then answers with the
 * "[native code]" form).
 */

#include "tilefinch/budget.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_SOURCE_STORE_BLOCK_BYTES (16u * 1024u)
/* Decoded blocks kept for later reads (first-call compiles of nearby
   functions read the same blocks). */
#ifndef SCRIPT_SOURCE_STORE_DECODED_BLOCKS
#define SCRIPT_SOURCE_STORE_DECODED_BLOCKS 4u
#endif

typedef enum {
    /* Every block compressed when the script is added. */
    SCRIPT_SOURCE_STORE_COMPRESS_EAGER = 0,
    /* Plain copies; never compressed (eviction only). */
    SCRIPT_SOURCE_STORE_COMPRESS_NEVER,
    /* Plain copies while the page has room; compressed, oldest first,
       once plain text passes the allowance its caller computes from the
       Budget's free room (script_source_store_balance()) or under
       pressure (script_source_store_relieve()). */
    SCRIPT_SOURCE_STORE_COMPRESS_UNDER_PRESSURE
} ScriptSourceStoreCompression;

typedef struct ScriptSourceStoreStats {
    size_t entries;
    size_t text_bytes;      /* script text the entries hold */
    size_t held_bytes;      /* Budget bytes holding it (blocks + tables) */
    size_t plain_bytes;     /* text held in plain blocks */
    size_t compressible_plain_bytes; /* of which not yet found incompressible */
    size_t decode_buffer_bytes;
    size_t peak_held_bytes;
    uint64_t added;         /* scripts added */
    uint64_t added_bytes;
    uint64_t refused;       /* adds refused for memory */
    uint64_t compress_attempts;
    uint64_t scratch_tables;  /* LZ match tables allocated */
    size_t leases;            /* operations in progress (see _lease()) */
    uint64_t compressed_blocks;
    uint64_t compressed_in_bytes;
    uint64_t compressed_out_bytes;
    uint64_t compress_ns;
    uint64_t reads;
    uint64_t read_bytes;
    uint64_t read_failures; /* dropped entry or decode failure */
    uint64_t block_decodes;
    uint64_t decode_ns;
    uint64_t read_ns;
    uint64_t evicted;
    uint64_t evicted_text_bytes;
    size_t required_entries;
    uint64_t released;
} ScriptSourceStoreStats;

typedef struct ScriptSourceStore ScriptSourceStore;

ScriptSourceStore *script_source_store_create(
    Budget *budget, ScriptSourceStoreCompression compression);
void script_source_store_destroy(ScriptSourceStore *store);
/* Copies (and maybe compresses) `length` bytes of script text; returns its
   id, or 0 if the Budget refused (nothing is then held). */
uint32_t script_source_store_add(ScriptSourceStore *store, const char *text,
                                 size_t length);
/* An entry for `length` bytes of text that script_source_store_fill()
   supplies later (reads fail until then); returns its id, or 0. Restoring
   bytecode reserves the id it reads with and fills the entry only if the
   restored functions refer to it. */
uint32_t script_source_store_reserve(ScriptSourceStore *store, size_t length);
/* Copies the text of a reserved entry (as script_source_store_add()); false
   if the entry is gone, already filled, or the Budget refused (the entry
   then stays unfilled). */
bool script_source_store_fill(ScriptSourceStore *store, uint32_t id,
                              const char *text);
/* An operation (a compile, a bytecode restore) that will create functions
   referring to entry `id` takes a lease before it starts and gives it back
   (take false) on every way out; while leased, an entry is never dropped
   (script_source_store_relieve()), as a required one is not. Taking fails
   for an entry that is gone. */
bool script_source_store_lease(ScriptSourceStore *store, uint32_t id,
                               bool take);
/* Marks entry `id` required (functions compiled lazily from it still need
   its text to compile) or no longer: a required entry is never dropped. */
void script_source_store_require(ScriptSourceStore *store, uint32_t id,
                                 bool required);
/* Forgets entry `id` (no-op if already dropped). */
void script_source_store_release(ScriptSourceStore *store, uint32_t id);
/* Copies `length` bytes at `offset` of entry `id` into `dest`; 0 on
   success, -1 if the entry was dropped, the span is out of range or a
   block could not be decoded. */
int script_source_store_read(ScriptSourceStore *store, uint32_t id,
                             uint32_t offset, uint32_t length, char *dest);
/* What a read of that span would find, without reading it: -1 if it
   would fail (the entry was dropped or is unfilled, or the span is out of
   range), 1 if every block it spans is ASCII (each block is classified
   once: when compressed, or by the first query that spans it), else 0. A -1 counts as a read failure in the stats. */
int script_source_store_query(ScriptSourceStore *store, uint32_t id,
                              uint32_t offset, uint32_t length);
/* Compresses plain blocks, oldest entry first, until at most
   `plain_allowance` bytes stay plain (UNDER_PRESSURE only). */
void script_source_store_balance(ScriptSourceStore *store,
                                 size_t plain_allowance);
/* Releases about `needed` Budget bytes: the decode buffers, then (except
   under COMPRESS_NEVER) plain blocks by compression, then whole entries no
   uncompiled lazy function needs, least recently read first. Returns the
   bytes released. Allocates only Budget memory (compressed blocks smaller
   than what they replace and a match table), never the QuickJS heap, so the
   realm's growth hook may call it. */
size_t script_source_store_relieve(ScriptSourceStore *store, size_t needed);
/* Text bytes the entries hold (0 for NULL). */
size_t script_source_store_text_bytes(const ScriptSourceStore *store);
void script_source_store_stats(const ScriptSourceStore *store,
                               ScriptSourceStoreStats *stats);

#endif
